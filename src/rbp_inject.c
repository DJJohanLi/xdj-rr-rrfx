/* rbp_inject.c -- ptrace-based dlopen() injector for ARM / glibc 2.13 (XDJ-RR fw 1.08).
 * Part of RRFX. Copyright (C) 2026 Johan Li and contributors.
 * License: GPL-3.0-or-later (see LICENSE).
 *
 * Attaches to the already-running player process (rbp), makes it call
 * dlopen(so_path, RTLD_NOW) once on its own main thread, reads the returned
 * handle, then restores the ORIGINAL register state exactly and detaches. The
 * target resumes precisely where it was, with the library loaded and its
 * constructor (which installs the hooks) already run. rbp is never stopped for
 * longer than the single call and is never restarted.
 *
 * Details that matter:
 * - The tracee is usually stopped inside a syscall (select/poll/futex), so
 *   orig_r0 is forced to -1 before SETREGS+CONT; otherwise the kernel's
 *   syscall-restart logic interferes with the injected call.
 * - The call returns into lr = 0, which traps (SIGSEGV) back to us; that trap
 *   is expected and is how the return value is collected.
 * - libdl's base address is found in /proc/<pid>/maps by the bare substring
 *   "libdl": the maps file shows the symlink target "libdl-2.13.so", which
 *   does not contain "libdl.so".
 * - dlopen's offset inside the device's libdl.so.2 (0xae4, ARM mode) is
 *   hardcoded. The calling script verifies /lib/libdl.so.2 by SHA1 first.
 *
 * Status: proven on real hardware since v1.0.16 (every release since uses this
 * exact binary). Build: arm-linux-gnueabi-gcc -static -O2 -Wall
 * (Ubuntu 24.04 gcc 13.3 gives SHA1 62b52de1fd0e9aabc7dbe5751ef8d5334b07841d).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>

/* struct user_regs { unsigned long uregs[18]; } -- r0..r15, cpsr, orig_r0 */
#define ARM_r0       0
#define ARM_r1       1
#define ARM_sp       13
#define ARM_lr       14
#define ARM_pc       15
#define ARM_cpsr     16
#define ARM_orig_r0  17

/* dlopen's offset within the real device's /lib/libdl.so.2 (SHA1
 * 700c14efe86238f6cd1dcb1fa969e9f78b32e824, checked by the caller).
 * Symbol value has bit0==0 -> plain ARM-mode code, not Thumb, so no
 * CPSR T-bit handling or address masking is needed. */
#define DLOPEN_LIBDL_OFFSET 0x00000ae4UL

static int find_lib_base(pid_t pid, const char *libname, unsigned long *base_out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, libname)) {
            unsigned long start, offset;
            if (sscanf(line, "%lx-%*x %*4s %lx", &start, &offset) == 2 && offset == 0) {
                *base_out = start;
                found = 1;
                break;
            }
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <pid> <so_path>\n", argv[0]);
        return 1;
    }
    pid_t pid = (pid_t)atol(argv[1]);
    const char *so_path = argv[2];
    size_t pathlen = strlen(so_path) + 1;
    if (pathlen > 512) { fprintf(stderr, "RESULT=PATH_TOO_LONG\n"); return 1; }

    printf("RBP_INJECT=attaching pid=%d\n", (int)pid);
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0) {
        printf("RESULT=ATTACH_FAILED errno=%d\n", errno);
        return 1;
    }
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
        printf("RESULT=ATTACH_WAIT_UNEXPECTED status=%d\n", status);
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }
    printf("RBP_INJECT=attached_and_stopped\n");

    struct user_regs orig_regs;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &orig_regs) != 0) {
        printf("RESULT=GETREGS_FAILED errno=%d\n", errno);
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }
    printf("RBP_INJECT=saved_orig_regs pc=%08lx sp=%08lx\n",
           orig_regs.uregs[ARM_pc], orig_regs.uregs[ARM_sp]);

    if (getenv("RBP_INJECT_DRYRUN")) {
        /* Attach-and-detach-only dry run: prove the attach/getregs/
         * detach lifecycle alone is safe on this device, without ever
         * touching registers or memory. Recommended first step on real
         * hardware before the real injection. */
        if (ptrace(PTRACE_DETACH, pid, NULL, NULL) != 0) {
            printf("RESULT=DRYRUN_DETACH_FAILED errno=%d\n", errno);
            return 1;
        }
        printf("RESULT=DRYRUN_OK_NOTHING_TOUCHED\n");
        return 0;
    }

    /* Match the resolved real filename, not just the symlink name: on
     * this device /lib/libdl.so.2 is itself a symlink to libdl-2.13.so
     * (confirmed via the extracted rootfs: lrwxrwxrwx libdl.so.2 ->
     * libdl-2.13.so, same naming convention as libc-2.13.so and
     * libpthread-2.13.so). /proc/pid/maps shows the path of the actual
     * opened file after symlink resolution -- "libdl-2.13.so" -- which
     * does NOT contain the substring "libdl.so" (dot vs dash right after
     * "libdl"). This was the exact cause of the real-device
     * LIBDL_BASE_NOT_FOUND failure in v1.0.15. Search for the bare
     * "libdl" prefix instead, which matches both the symlink name and
     * the resolved real name. */
    unsigned long libdl_base = 0;
    if (find_lib_base(pid, "libdl", &libdl_base) != 0) {
        printf("RESULT=LIBDL_BASE_NOT_FOUND\n");
        ptrace(PTRACE_SETREGS, pid, NULL, &orig_regs);
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }
    unsigned long target_dlopen = libdl_base + DLOPEN_LIBDL_OFFSET;
    printf("RBP_INJECT=libdl_base=%08lx target_dlopen=%08lx\n", libdl_base, target_dlopen);

    /* Write the path string onto the target's own stack, well below its
     * current sp, 8-byte aligned (ARM EABI stack alignment), one 4-byte
     * word at a time. */
    unsigned long str_addr = (orig_regs.uregs[ARM_sp] - 2048) & ~7UL;
    for (size_t i = 0; i < pathlen; i += 4) {
        long word = 0;
        memcpy(&word, so_path + i, (pathlen - i < 4) ? (pathlen - i) : 4);
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(str_addr + i), (void *)word) != 0) {
            printf("RESULT=POKE_STRING_FAILED errno=%d\n", errno);
            ptrace(PTRACE_SETREGS, pid, NULL, &orig_regs);
            ptrace(PTRACE_DETACH, pid, NULL, NULL);
            return 1;
        }
    }
    printf("RBP_INJECT=wrote_path_at=%08lx\n", str_addr);

    struct user_regs work_regs = orig_regs;
    work_regs.uregs[ARM_r0] = str_addr;        /* dlopen arg 1: filename */
    work_regs.uregs[ARM_r1] = 2;               /* dlopen arg 2: RTLD_NOW */
    work_regs.uregs[ARM_sp] = (str_addr - 256) & ~7UL;
    work_regs.uregs[ARM_lr] = 0;                /* return -> jump to 0 -> trap, our completion signal */
    work_regs.uregs[ARM_pc] = target_dlopen;
    /* Carried over from the x86_64 rehearsal: the tracee is almost
     * certainly stopped mid-syscall. Force orig_r0 to -1 so the kernel
     * does not try to restart/interfere with that syscall once we
     * change pc out from under it. */
    work_regs.uregs[ARM_orig_r0] = (unsigned long)-1;

    if (ptrace(PTRACE_SETREGS, pid, NULL, &work_regs) != 0) {
        printf("RESULT=SETREGS_CALL_FAILED errno=%d\n", errno);
        ptrace(PTRACE_SETREGS, pid, NULL, &orig_regs);
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }
    printf("RBP_INJECT=call_registers_set\n");

    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0) {
        printf("RESULT=CONT_FAILED errno=%d\n", errno);
        ptrace(PTRACE_SETREGS, pid, NULL, &orig_regs);
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }
    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
        printf("RESULT=TRAP_WAIT_UNEXPECTED status=%d (target may be in an unknown state)\n", status);
        /* Do not touch registers here -- we don't know what state the
         * tracee is actually in; a blind restore could make things
         * worse. Just try to detach and let the kernel do whatever it
         * would do with a live, non-stopped tracee. */
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }
    printf("RBP_INJECT=trapped_as_expected signal=%d\n", WSTOPSIG(status));

    struct user_regs after_regs;
    unsigned long handle = 0;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &after_regs) == 0) {
        handle = after_regs.uregs[ARM_r0];
    }
    printf("RBP_INJECT=dlopen_returned handle=%08lx\n", handle);

    if (ptrace(PTRACE_SETREGS, pid, NULL, &orig_regs) != 0) {
        printf("RESULT=RESTORE_FAILED errno=%d (target left attached, NOT detached, for safety)\n", errno);
        return 1;
    }
    printf("RBP_INJECT=original_regs_restored\n");

    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) != 0) {
        printf("RESULT=DETACH_FAILED errno=%d\n", errno);
        return 1;
    }
    printf("RBP_INJECT=detached\n");

    if (handle == 0) {
        printf("RESULT=DLOPEN_RETURNED_NULL\n");
        return 2;
    }
    printf("RESULT=INJECT_OK handle=%08lx\n", handle);
    return 0;
}
