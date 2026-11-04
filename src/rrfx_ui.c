/* rrfx_ui.c -- RRFX v1.0.0: all 8 Beat FX on the Pioneer XDJ-RR (firmware 1.08).
 *
 * Copyright (C) 2026 Johan Li and contributors.
 * License: GPL-3.0-or-later (see LICENSE).
 *
 * The stock XDJ-RR exposes 3 Beat FX on its type switch (Echo, Reverb, Flanger),
 * although the audio engine contains 8 (Reverb, Trans, Roll, Flanger, Echo,
 * Delay, Spiral, Pitch). This library is loaded into the running player process
 * (rbp) by rbp_inject and makes all 8 selectable from the front panel.
 *
 * SAFETY MODEL: RAM-only. Nothing is written to flash. rbp is never restarted.
 * The library swaps 7 function pointers in rbp's in-memory vtables after
 * checking that every one of them still holds the stock value (all-or-nothing).
 * A power-off returns the unit to stock.
 *
 * GESTURE: hold BEAT < and > together >= 1.2 s and release -> select mode.
 * < / > browse the 8 types (only the shown name changes). Hold both >= 1.2 s
 * again and release -> the shown type is applied exactly like the stock type
 * switch would apply one of its 3 types. 10 s idle or moving the physical type
 * switch cancels select mode; moving the switch always returns to stock types.
 *
 * Why the UI state matters: ui::MasterInnards+60 holds the switch code that
 * drives the on-screen name, the FX ON button, the arrow parameter scheme and
 * quantize. Setting only the engine leaves the UI acting on the old type, so
 * the commit writes the code whose name image matches the new type
 * (codes 0..7 = Delay Echo Spiral Reverb Trans Flanger Pitch Roll).
 *
 * Hooks (vtable slot swaps only, no code patching):
 *   MasterInnards slot 13 onEv_BeatEffectType  : physical switch moved -> stock again
 *   MasterInnards slot 14 onEv_BeatEffectEnable: FX ON engages the selected type
 *   MasterInnards slot 16 onEv_BeatFxBeat      : Pitch uses its own -50..+100 % steps
 *   Mixer asEventCode                          : BEAT < / > gesture
 *   Mixer updateMasterStatus / getMasterStat / thunk: show the browsed name
 *
 * Pitch: the engine's real range is -50..+100 %; the arrows step through
 * -50,-25,-10,+10,+25,+50,+75,+100 and the engine keeps each type's last value.
 * Debug: if /dev/shm/rrfx_debug exists (autoexec.sh creates it when
 * RRFX_DEBUG.txt is on the stick), every other mixer key event is also logged.
 *
 * Backup selection: /dev/shm/rrfx_req (written by autoexec.sh from
 * RRFX_SELECT.txt) is executed on the JUCE message thread.
 * Version history: CHANGELOG.md. Addresses: docs/HOW_IT_WORKS.md.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define VERSION "1.0.0"

/* ---- fixed rbp 1.08 addresses (binary is not PIE; SHA1 5501b312...) ---- */
#define A_DJ_GETINSTANCE   0x00029ed8u
#define A_DJ_SETTYPE       0x0004a740u
#define A_DJ_GETTYPE       0x0004a7f0u
#define A_DJ_SWITCHMAXMIN  0x0004b2a0u
#define A_DJ_QVALID        0x0004b4d4u
#define A_DJ_SETQ          0x0004b350u
#define A_DJ_SETLEVEL      0x0004ac98u
#define A_MM_INSTANCE      0x018764ccu  /* juce::MessageManager::instance      */
#define A_MM_CALLONMSG     0x0034a930u  /* MessageManager::callFunctionOnMessageThread */
#define A_MM_ISMSGTHREAD   0x00343414u
#define A_UIOBJMGR_HOLDER  0x01872d70u  /* [+0x110] = UiObjectManager*        */

#define MI_VPTR            0x004777f8u  /* vtable for ui::MasterInnards (+8)   */
#define MI_SLOT_TYPE       (MI_VPTR + 13 * 4)
#define MI_SLOT_ENABLE     (MI_VPTR + 14 * 4)
#define MI_SLOT_BEAT       (MI_VPTR + 16 * 4)
#define MI_ORIG_TYPE       0x002bfa34u
#define MI_ORIG_ENABLE     0x002bedfcu
#define MI_ORIG_BEAT       0x002c0c10u

#define MX_SLOT_AEC        0x0046d3dcu  /* vtable for ui::Mixer: asEventCode   */
#define MX_ORIG_AEC        0x002475d0u

#define MX_SLOT_UPDMASTER  0x0046d3ccu  /* ui::Mixer::updateMasterStatus(bool)  */
#define MX_ORIG_UPDMASTER  0x00247a50u
#define MX_SLOT_GETMASTER  0x0046d3f4u  /* ui::Mixer::getMasterStat()          */
#define MX_ORIG_GETMASTER  0x00247a1cu
#define MX_SLOT_GETMTHUNK  0x0046d438u  /* non-virtual thunk (this-116)        */
#define MX_ORIG_GETMTHUNK  0x00247a14u
#define MX_STAT_OFF        184          /* Mixer+184 = tMasterStat             */
#define ST_CODE            10           /* tMasterStat byte: +60 code (name)   */
#define A_DJ_GETPCT        0x0004b0a8u  /* DjEngineIF::getBeatEffectPercent() const */
#define A_DJ_SETPCT        0x0004aff8u  /* DjEngineIF::setBeatEffectPercent(long)   */

#define HOLD_MS            1200
#define CHORD_UNDO_MS      600
#define SELECT_TIMEOUT_MS  10000

#define KEY_BEAT_LEFT      0x4491
#define KEY_BEAT_RIGHT     0x4492

/* MasterInnards fields */
#define OFF_CODE   60   /* int: raw switch code -> drives name/arrows/quantize */
#define OFF_FXON   68   /* u8 : Beat FX on                                     */
#define OFF_Q70    70   /* u8 : cleared when quantize turns on                 */
#define OFF_220    220  /* int: set to 12 by the stock type handler            */
#define OFF_LEVEL  228  /* float: last LEVEL/DEPTH knob value                  */
#define OFF_MODE   236  /* int: 0 => quantize forced off                       */

typedef void *(*fn_getinst)(void);
typedef void  (*fn_setint)(void *, int);
typedef int   (*fn_getint)(void *);
typedef void  (*fn_setlevel)(void *, uint32_t);       /* softfp: float bits in r1 */
typedef int   (*fn_mi_int)(void *, int);              /* bool onEv_X(this, int)   */
typedef int   (*fn_aec)(void *, const void *, int *);
typedef void *(*fn_msgcb)(void *);
typedef int   (*fn_upd)(void *, int);
typedef void *(*fn_getstat)(void *);
typedef void *(*fn_callonmsg)(void *, fn_msgcb, void *);
typedef int   (*fn_ismsg)(void *);

#define CALL(t, a) ((t)(uintptr_t)(a))

static const char *NAMES[9] = { "Off", "Reverb", "Trans", "Roll", "Flanger",
                                "Echo", "Delay", "Spiral", "Pitch" };
/* MasterInnards+60 code that makes the GUI draw each type's name
 * (code -> name image: Delay0 Echo1 Spiral2 Reverb3 Trans4 Flanger5 Pitch6 Roll7).
 * (GUI object names say PHASER for code 2, but the image drawn is SPIRAL --
 * confirmed on hardware.) */
static const int DISPCODE[9] = { -1, 3, 4, 7, 5, 1, 0, 2, 6 };

static volatile int g_override;          /* 0 = stock behaviour, else 1..8   */
static volatile int g_installed;

/* gesture state (written on the key thread; read by the display overlay) */
static volatile int  g_down[2];          /* 0 = BEAT <, 1 = BEAT >            */
static volatile long g_chord_ms;         /* 0 = no chord; else time both down */
static volatile int  g_mode;             /* 0 normal, 1 select                */
static volatile int  g_cand;             /* candidate type 1..8 in select mode */
static volatile long g_activity_ms;
static volatile long g_lastbeat_ms;      /* last stock arrow step (for undo)  */
static volatile int  g_lastbeat_dir, g_lastbeat_r;
static volatile int  g_lastbeat_pitch, g_lastbeat_prevpct; /* Pitch undo restores exact value */
static volatile int  g_lastcand_dir;     /* last select-mode step (for undo)  */
static volatile long g_lastcand_ms;
static FILE *g_log;
static pthread_mutex_t g_logmx = PTHREAD_MUTEX_INITIALIZER;

/* Raw ARM EABI clock_gettime (nr 263, 32-bit timespec): avoids the time64
 * symbol redirects of modern headers, which glibc 2.13 on the device lacks. */
static long now_ms(void)
{
    struct { int32_t sec, nsec; } ts = {0, 0};
    syscall(263, 1 /* CLOCK_MONOTONIC */, &ts);
    return (long)ts.sec * 1000L + ts.nsec / 1000000L;
}

static int is_msg_thread(void)
{
    void *mm = *(void **)(uintptr_t)A_MM_INSTANCE;
    return mm ? CALL(fn_ismsg, A_MM_ISMSGTHREAD)(mm) : -1;
}

static void logmsg(const char *fmt, ...)
{
    pthread_mutex_lock(&g_logmx);
    if (g_log) {
        if (ftell(g_log) > 512 * 1024) {
            pthread_mutex_unlock(&g_logmx);
            return;                       /* cap: never fill tmpfs */
        }
        va_list ap;
        fprintf(g_log, "%ld tid=%ld ", now_ms(), (long)syscall(SYS_gettid));
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    pthread_mutex_unlock(&g_logmx);
}

static void *dj(void) { return CALL(fn_getinst, A_DJ_GETINSTANCE)(); }

static void *find_mi(void)
{
    void *mgr = *(void **)(uintptr_t)(A_UIOBJMGR_HOLDER + 0x110);
    if (!mgr) return NULL;
    void *mixer = *(void **)((char *)mgr + 0xe4);
    if (!mixer) return NULL;
    void *mi = *(void **)((char *)mixer + 0xa0);
    if (!mi || *(uint32_t *)mi != MI_VPTR) return NULL;
    return mi;
}

#define I32(p, o) (*(volatile int32_t *)((char *)(p) + (o)))
#define U8(p, o)  (*(volatile uint8_t *)((char *)(p) + (o)))
#define U32(p, o) (*(volatile uint32_t *)((char *)(p) + (o)))

static void resend_level(void *mi)
{
    void *d = dj();
    if (d) CALL(fn_setlevel, A_DJ_SETLEVEL)(d, U32(mi, OFF_LEVEL));
}

/* Stock ui::MasterInnards::onEv_BeatEffectType (0x2bfa34), generalised:
 * same writes, same engine calls, same order -- plus re-sending the cached
 * knob value so the engine level matches the physical LEVEL/DEPTH position. */
static int apply_select(void *mi, int t)
{
    void *d = dj();
    if (!d) return -1;
    int fxon = U8(mi, OFF_FXON);
    I32(mi, OFF_220) = 12;
    if (fxon) CALL(fn_setint, A_DJ_SETTYPE)(d, t);
    else      CALL(fn_setint, A_DJ_SWITCHMAXMIN)(d, t);
    I32(mi, OFF_CODE) = DISPCODE[t];
    int q;
    if (I32(mi, OFF_MODE) == 0 || t == 1 /* Reverb: stock forces off */) q = 0;
    else q = CALL(fn_getint, A_DJ_QVALID)(dj()) ? 1 : 0;
    CALL(fn_setint, A_DJ_SETQ)(dj(), q);
    if (q) U8(mi, OFF_Q70) = 0;
    resend_level(mi);
    g_override = t;
    logmsg("SELECT_APPLIED type=%d(%s) fxon=%d code60=%d quantize=%d engine_type=%d",
           t, NAMES[t], fxon, DISPCODE[t], q,
           CALL(fn_getint, A_DJ_GETTYPE)(dj()));
    return 0;
}

static void *msg_select(void *arg)            /* runs on the JUCE message thread */
{
    int t = (int)(intptr_t)arg;
    void *mi = find_mi();
    logmsg("MSG_THREAD_CALLBACK t=%d is_msg_thread=%d mi=%p", t, is_msg_thread(), mi);
    if (!mi) { logmsg("SELECT_REFUSED MI_NOT_FOUND"); return NULL; }
    if (t == 0) {
        /* Not cleared here on purpose: +60 may hold a code the stock FX ON
         * table cannot map. Moving the physical switch restores stock state
         * correctly (h_type clears the override after the stock handler). */
        logmsg("STOCK_REQUEST_IGNORED -- move the physical BEAT FX switch to return to stock");
        return NULL;
    }
    apply_select(mi, t);
    return NULL;
}

/* ------------------------------- hooks ---------------------------------- */
static int h_type(void *mi, int sw)
{
    int r = CALL(fn_mi_int, MI_ORIG_TYPE)(mi, sw);
    logmsg("HOOK onEv_BeatEffectType sw=%d -> r=%d code60=%d (override %d cleared)",
           sw, r, I32(mi, OFF_CODE), g_override);
    g_override = 0;                          /* physical switch wins again */
    if (g_mode) { g_mode = 0; logmsg("SELECT_MODE_CANCELLED physical switch"); }
    return r;
}

static int h_enable(void *mi, int sw)
{
    int fxon = U8(mi, OFF_FXON);
    int t = g_override;
    int turning_on = ((sw == 1) || (sw == 2)) && fxon == 0;
    if (t && turning_on) {
        void *d = dj();
        if (d) {
            CALL(fn_setint, A_DJ_SETTYPE)(d, t);
            U8(mi, OFF_FXON) = 1;
            resend_level(mi);
            logmsg("HOOK onEv_BeatEffectEnable sw=%d ON with override %d(%s)", sw, t, NAMES[t]);
            return 1;
        }
    }
    int r = CALL(fn_mi_int, MI_ORIG_ENABLE)(mi, sw);
    logmsg("HOOK onEv_BeatEffectEnable sw=%d fxon_before=%d -> stock r=%d", sw, fxon, r);
    return r;
}

/* Pitch: the engine's own range is -50..+100 %. Step through this ladder.
 * Values between steps snap to the next step in the arrow direction; at either
 * end nothing changes and 0 is returned, like the stock handler at a limit. */
static const int PITCH_LADDER[] = { -50, -25, -10, 10, 25, 50, 75, 100 };
#define PITCH_N ((int)(sizeof PITCH_LADDER / sizeof *PITCH_LADDER))

static int pitch_next(int cur, int dir)
{
    if (dir > 0) { for (int i = 0; i < PITCH_N; i++) if (PITCH_LADDER[i] > cur) return PITCH_LADDER[i]; }
    else         { for (int i = PITCH_N - 1; i >= 0; i--) if (PITCH_LADDER[i] < cur) return PITCH_LADDER[i]; }
    return cur;                                   /* at the end: no change */
}

static int pitch_step(int dir, int *prev)
{
    void *d = dj();
    if (!d) return 0;
    int cur = CALL(fn_getint, A_DJ_GETPCT)(d);
    int nv = pitch_next(cur, dir);
    *prev = cur;
    if (nv == cur) return 0;
    CALL(fn_setint, A_DJ_SETPCT)(dj(), nv);
    logmsg("PITCH_STEP dir=%d %d%% -> %d%%", dir, cur, nv);
    return 1;
}

static int h_beat(void *mi, int dir)
{
    int t = g_override, r, prev = 0;
    if (t == 8) r = pitch_step(dir, &prev);
    else        r = CALL(fn_mi_int, MI_ORIG_BEAT)(mi, dir);
    g_lastbeat_ms = now_ms(); g_lastbeat_dir = dir; g_lastbeat_r = r;
    g_lastbeat_pitch = (t == 8); g_lastbeat_prevpct = prev;
    logmsg("HOOK onEv_BeatFxBeat dir=%d override=%d code60=%d -> r=%d", dir, t, I32(mi, OFF_CODE), r);
    return r;
}

/* Effective type right now: override, else the stock switch code. */
static int current_type(void *mi)
{
    if (g_override) return g_override;
    switch (I32(mi, OFF_CODE)) { case 1: return 5; case 3: return 1; case 5: return 4; }
    return 1;
}

static int step_type(int t, int dir) { t += dir; if (t < 1) t = 8; if (t > 8) t = 1; return t; }

/* BEAT key gesture state machine. Runs on rbp's key/UI thread, the same thread as the stock switch handler. Returning 0 swallows the
 * event exactly like stock code does for key releases. */
static int gesture(void *mixer, int k, int pressed, int r)
{
    long now = now_ms();
    int other = k ^ 1;
    void *mi = *(void **)((char *)mixer + 0xa0);
    if (!mi || *(uint32_t *)mi != MI_VPTR) return r;

    if (g_mode && now - g_activity_ms > SELECT_TIMEOUT_MS) {
        g_mode = 0;
        logmsg("SELECT_MODE_TIMEOUT -- nothing changed");
    }
    if (pressed) {
        g_down[k] = 1;
        if (g_down[other]) {                         /* chord starts now */
            g_chord_ms = now;
            if (g_mode) {
                if (now - g_lastcand_ms < CHORD_UNDO_MS && g_lastcand_dir) {
                    g_cand = step_type(g_cand, -g_lastcand_dir);   /* undo 1st key's step */
                    g_lastcand_dir = 0;
                }
            } else if (now - g_lastbeat_ms < CHORD_UNDO_MS && g_lastbeat_r) {
                if (g_lastbeat_pitch) {               /* Pitch: restore the exact value */
                    void *d = dj();
                    if (d) CALL(fn_setint, A_DJ_SETPCT)(d, g_lastbeat_prevpct);
                    logmsg("PITCH_UNDO -> %d%%", g_lastbeat_prevpct);
                } else {
                    h_beat(mi, -g_lastbeat_dir);      /* undo 1st key's param step */
                }
                g_lastbeat_r = 0;
            }
            g_activity_ms = now;
            logmsg("GESTURE chord_start mode=%d", g_mode);
            return 0;                                /* swallow 2nd press */
        }
        if (g_mode) {                                /* browse */
            int d = k ? 1 : -1;
            g_cand = step_type(g_cand, d);
            g_lastcand_dir = d; g_lastcand_ms = now; g_activity_ms = now;
            logmsg("SELECT_MODE browse -> %d(%s)", g_cand, NAMES[g_cand]);
            return 0;
        }
        return r;                                    /* normal stock step */
    }
    /* release */
    int was_chord = g_chord_ms != 0;
    long held = was_chord ? now - g_chord_ms : 0;
    g_down[k] = 0;
    if (was_chord) {
        g_chord_ms = 0;                              /* chord ends at first release */
        g_activity_ms = now;
        if (held >= HOLD_MS) {
            if (!g_mode) {
                g_cand = current_type(mi);
                g_mode = 1; g_lastcand_dir = 0;
                logmsg("SELECT_MODE_ENTER held=%ldms start=%d(%s)", held, g_cand, NAMES[g_cand]);
            } else {
                g_mode = 0;
                logmsg("SELECT_MODE_COMMIT held=%ldms -> %d(%s)", held, g_cand, NAMES[g_cand]);
                apply_select(mi, g_cand);
            }
        } else {
            logmsg("GESTURE chord_too_short held=%ldms (need %d)", held, HOLD_MS);
        }
    }
    return g_mode ? 0 : r;
}

/* DEBUG ONLY (off by default): with /dev/shm/rrfx_debug present, record every
 * other mixer key. Rate-limited per (id, action): a repeat of the same id+action
 * within 250 ms is not logged. Never changes the event. */
static volatile int g_debug;
#define KEYLOG_N 64
static uint16_t g_kl_id[KEYLOG_N];
static uint8_t  g_kl_act[KEYLOG_N];
static long     g_kl_ms[KEYLOG_N];
static void keylog(uint16_t id, const void *key, int r)
{
    if (!g_debug) return;
    const uint8_t *b = (const uint8_t *)key;
    long now = now_ms();
    unsigned i = (id * 2654435761u) >> 26;        /* 0..63 */
    if (g_kl_id[i] == id && g_kl_act[i] == b[11] && now - g_kl_ms[i] < 250) return;
    g_kl_id[i] = id; g_kl_act[i] = b[11]; g_kl_ms[i] = now;
    logmsg("KEYLOG id=0x%04x b10=%02x b11=%02x v32=%d -> ev=0x%x", id, b[10], b[11],
           *(const int32_t *)(b + 32), r);
}

static int h_aec(void *mixer, const void *key, int *x)
{
    int r = CALL(fn_aec, MX_ORIG_AEC)(mixer, key, x);
    uint16_t id = *(const uint16_t *)((const char *)key + 8);
    if (id == KEY_BEAT_LEFT || id == KEY_BEAT_RIGHT) {
        const uint8_t *b = (const uint8_t *)key;
        int pressed = (b[11] & 0x0f) == 0;
        int out = gesture(mixer, id == KEY_BEAT_RIGHT, pressed, r);
        logmsg("KEY %s b11=%02x v32=%d -> ev=0x%x out=0x%x mode=%d",
               id == KEY_BEAT_LEFT ? "BEAT_LEFT " : "BEAT_RIGHT", b[11],
               *(const int32_t *)(b + 32), r, out, g_mode);
        return out;
    }
    keylog(id, key, r);
    return r;
}

/* ---- display-only overlay: never touches MasterInnards+60 (getStat reads it) ---- */
static volatile unsigned g_refresh_calls;
static void *g_mixer;                      /* learned from the hooks */

static void overlay(void *mixer)
{
    g_refresh_calls++;
    g_mixer = mixer;
    long now = now_ms();
    uint8_t *st = (uint8_t *)mixer + MX_STAT_OFF;
    if (g_mode) {
        if (now - g_activity_ms > SELECT_TIMEOUT_MS) {
            g_mode = 0;                               /* display-only state: safe from any thread */
            return;
        }
        st[ST_CODE] = (uint8_t)DISPCODE[g_cand];     /* name only */
    }
}

static int h_updmaster(void *mixer, int b)
{
    int r = CALL(fn_upd, MX_ORIG_UPDMASTER)(mixer, b);
    overlay(mixer);
    return r;
}

static void *h_getmaster(void *mixer)
{
    void *p = CALL(fn_getstat, MX_ORIG_GETMASTER)(mixer);
    overlay(mixer);
    return p;
}

static void *h_getmthunk(void *iface)
{
    void *p = CALL(fn_getstat, MX_ORIG_GETMTHUNK)(iface);
    overlay((char *)iface - 116);
    return p;
}

/* Display stat buffer the screen reads: StatWatcher+28 (refreshed from
 * Mixer::getMasterStat via the IMixStatServer thunk). Fallback: Mixer+184. */
static uint8_t *stat_buf(void)
{
    void *mgr = *(void **)(uintptr_t)(A_UIOBJMGR_HOLDER + 0x110);
    if (mgr) {
        void *sw = *(void **)((char *)mgr + 0x38);
        if (sw) { uint8_t *p = *(uint8_t **)((char *)sw + 28); if (p) return p; }
        void *mixer = *(void **)((char *)mgr + 0xe4);
        if (mixer) return (uint8_t *)mixer + MX_STAT_OFF;
    }
    return NULL;
}

/* 125 ms display timer: shows the browsed name, ends select mode on timeout,
 * restores the real name the moment select mode ends. */
static void *display_thread(void *arg)
{
    (void)arg;
    int was_active = 0;
    unsigned last_calls = g_refresh_calls; long last_rate_ms = now_ms();
    for (;;) {
        usleep(125 * 1000);
        long now = now_ms();
        if (now - last_rate_ms >= 5000) {          /* diagnostics: refresh-hook rate */
            unsigned c = g_refresh_calls;
            if (g_mode || g_chord_ms)
                logmsg("DIAG stat refresh hooks: %u calls in %ld ms", c - last_calls, now - last_rate_ms);
            last_calls = c; last_rate_ms = now;
        }
        if (g_mode && now - g_activity_ms > SELECT_TIMEOUT_MS) {
            g_mode = 0;
            logmsg("SELECT_MODE_TIMEOUT -- nothing changed");
        }
        int active = g_mode;
        uint8_t *st = stat_buf();
        void *mi = find_mi();
        if (!st || !mi) continue;
        if (active) {
            st[ST_CODE] = (uint8_t)DISPCODE[g_cand];          /* name only */
            if (!was_active) logmsg("DIAG select display on stat=%p", st);
        } else if (was_active) {                    /* restore the real values at once */
            st[ST_CODE] = (uint8_t)I32(mi, OFF_CODE);         /* real name back */
            logmsg("DIAG select display off, name restored");
        }
        was_active = active;
    }
    return NULL;
}

/* --------------------------- hook installation ---------------------------- */
static int patch_slot(uint32_t slot, uint32_t expect, void *fn)
{
    uint32_t *p = (uint32_t *)(uintptr_t)slot;
    if (*p != expect) { logmsg("PATCH_REFUSED slot=0x%x has 0x%x expected 0x%x", slot, *p, expect); return -1; }
    uintptr_t pg = slot & ~0xfffu;
    if (mprotect((void *)pg, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        logmsg("MPROTECT_RW_FAILED slot=0x%x errno=%d", slot, errno); return -1;
    }
    __atomic_store_n(p, (uint32_t)(uintptr_t)fn, __ATOMIC_SEQ_CST);
    mprotect((void *)pg, 4096, PROT_READ | PROT_EXEC);
    logmsg("PATCHED slot=0x%x 0x%x -> %p", slot, expect, fn);
    return 0;
}

static int parse_req(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    if (*s >= '0' && *s <= '8' && (s[1] == 0 || s[1] == '\n' || s[1] == '\r' || s[1] == ' ')) return *s - '0';
    for (int i = 0; i < 9; i++) if (strncasecmp(s, NAMES[i], strlen(NAMES[i])) == 0) return i;
    if (strncasecmp(s, "stock", 5) == 0) return 0;
    return -1;
}

static void *poll_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(250 * 1000);
        int dbg = access("/dev/shm/rrfx_debug", F_OK) == 0;
        if (dbg != g_debug) { g_debug = dbg; logmsg("DEBUG_KEYLOG %s", dbg ? "ON" : "OFF"); }
        int fd = open("/dev/shm/rrfx_req", O_RDONLY);
        if (fd < 0) continue;
        char buf[64] = {0};
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        unlink("/dev/shm/rrfx_req");
        if (n <= 0) continue;
        int t = parse_req(buf);
        logmsg("REQUEST '%s' -> %d", strtok(buf, "\r\n"), t);
        if (t < 0) { logmsg("REQUEST_REFUSED unparseable"); continue; }
        void *mm = *(void **)(uintptr_t)A_MM_INSTANCE;
        if (!mm) { logmsg("REQUEST_REFUSED no MessageManager"); continue; }
        CALL(fn_callonmsg, A_MM_CALLONMSG)(mm, msg_select, (void *)(intptr_t)t);
        logmsg("REQUEST_DONE %d", t);
    }
    return NULL;
}

__attribute__((constructor)) static void rrfx_init(void)
{
    if (g_installed) return;
    g_installed = 1;
    g_log = fopen("/dev/shm/rrfx_ui100.log", "a");
    logmsg("rrfx_ui v%s loaded pid=%d is_msg_thread=%d", VERSION, (int)getpid(), is_msg_thread());
    void *mi = find_mi();
    logmsg("MasterInnards=%p code60=%d fxon=%d", mi, mi ? I32(mi, OFF_CODE) : -1, mi ? U8(mi, OFF_FXON) : -1);
    if (!mi) { logmsg("INSTALL_REFUSED MI_NOT_FOUND -- no hooks installed"); return; }
    /* All-or-nothing gate: check every original before patching any. */
    if (*(uint32_t *)MI_SLOT_TYPE != MI_ORIG_TYPE || *(uint32_t *)MI_SLOT_ENABLE != MI_ORIG_ENABLE ||
        *(uint32_t *)MI_SLOT_BEAT != MI_ORIG_BEAT || *(uint32_t *)MX_SLOT_AEC != MX_ORIG_AEC ||
        *(uint32_t *)MX_SLOT_UPDMASTER != MX_ORIG_UPDMASTER ||
        *(uint32_t *)MX_SLOT_GETMASTER != MX_ORIG_GETMASTER ||
        *(uint32_t *)MX_SLOT_GETMTHUNK != MX_ORIG_GETMTHUNK) {
        logmsg("INSTALL_REFUSED vtable contents unexpected -- no hooks installed");
        return;
    }
    int bad = 0;
    bad |= patch_slot(MI_SLOT_TYPE,   MI_ORIG_TYPE,   (void *)h_type);
    bad |= patch_slot(MI_SLOT_ENABLE, MI_ORIG_ENABLE, (void *)h_enable);
    bad |= patch_slot(MI_SLOT_BEAT,   MI_ORIG_BEAT,   (void *)h_beat);
    bad |= patch_slot(MX_SLOT_AEC,    MX_ORIG_AEC,    (void *)h_aec);
    bad |= patch_slot(MX_SLOT_UPDMASTER, MX_ORIG_UPDMASTER, (void *)h_updmaster);
    bad |= patch_slot(MX_SLOT_GETMASTER, MX_ORIG_GETMASTER, (void *)h_getmaster);
    bad |= patch_slot(MX_SLOT_GETMTHUNK, MX_ORIG_GETMTHUNK, (void *)h_getmthunk);
    logmsg(bad ? "INSTALL_PARTIAL" : "INSTALL_OK");
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, poll_thread, NULL);
    pthread_create(&th, &at, display_thread, NULL);
}
