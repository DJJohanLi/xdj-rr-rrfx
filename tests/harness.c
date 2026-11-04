/* harness.c -- qemu-arm test bench for rrfx_ui.so (RRFX v1.0.0, GPL-3.0-or-later). Recreates, at rbp's exact
 * fixed addresses, everything rrfx_ui.so touches: engine-function stubs,
 * the MasterInnards and Mixer vtable slots, the UiObjectManager pointer chain,
 * the JUCE MessageManager. Then dlopen()s the real library and drives every
 * hook path, checking the exact engine-call sequence. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static char trace[8192];
static void T(const char *fmt, long a, long b)
{
    char x[128];
    snprintf(x, sizeof x, fmt, a, b);
    strcat(trace, x);
    strcat(trace, ";");
}

static int engine_type;
static char fake_dj[64];
static void *s_getinst(void) { return fake_dj; }
static void s_settype(void *d, int t) { engine_type = t; T("setType(%ld)", t, 0); }
static int  s_gettype(void *d) { return engine_type; }
static void s_maxmin(void *d, int t) { T("maxmin(%ld)", t, 0); }
static int  s_qvalid(void *d) { T("qvalid", 0, 0); return 1; }
static void s_setq(void *d, int q) { T("setQ(%ld)", q, 0); }
static int  s_getbpm(void *d) { return 1285; }
static int  pct = -50;                     /* engine's Pitch default */
static int  s_getpct(void *d) { return pct; }
static void s_setpct(void *d, int v) { pct = v; T("setPct(%ld)", v, 0); }
static void s_setlevel(void *d, uint32_t bits) { T("setLevel(0x%lx)", (long)bits, 0); }
static int  s_orig_type(void *mi, int sw) { T("ORIG_type(%ld)", sw, 0); return 1; }
static int  s_orig_enable(void *mi, int sw) { T("ORIG_enable(%ld)", sw, 0); return 1; }
static int  s_orig_beat(void *mi, int dir) { T("ORIG_beat(%ld,code60=%ld)", dir, *(int *)((char *)mi + 60)); return 1; }
static int  s_orig_aec(void *mx, const void *k, int *x)
{   /* emulate stock: press -> 0x200e/0x200f, release -> 0 */
    const uint8_t *b = k; uint16_t id = b[8] | (b[9] << 8);
    if ((b[11] & 15) != 0) return 0;
    return id == 0x4491 ? 0x200e : id == 0x4492 ? 0x200f : 0;
}
static int  s_ismsg(void *mm) { return 1; }
static int  stat_refreshes;
static void s_fill_stat(void *mixer) { uint8_t *st = (uint8_t *)mixer + 184; st[10] = (uint8_t)*(int *)(*(char **)((char *)mixer + 0xa0) + 60); st[8] = *((uint8_t *)*(char **)((char *)mixer + 0xa0) + 68); stat_refreshes++; }
static int  s_updmaster(void *mx, int b) { s_fill_stat(mx); return 1; }
static void *s_getmaster(void *mx) { s_fill_stat(mx); return (char *)mx + 184; }
static void *s_getmthunk(void *ifc) { return s_getmaster((char *)ifc - 116); }
static void *s_callonmsg(void *mm, void *(*fn)(void *), void *d) { T("callOnMsg", 0, 0); return fn(d); }

static void map_page(uintptr_t a)
{
    uintptr_t p = a & ~0xfffu;
    void *r = mmap((void *)p, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r != (void *)p) { printf("MAP FAIL %lx\n", (long)p); exit(2); }
}
static void stub(uintptr_t a, void *fn)
{
    uint32_t *w = (uint32_t *)a;
    w[0] = 0xe51ff004u;               /* ldr pc, [pc, #-4] */
    w[1] = (uint32_t)(uintptr_t)fn;
    __builtin___clear_cache((char *)a, (char *)a + 8);
}
#define W32(a) (*(volatile uint32_t *)(uintptr_t)(a))

static int fails;
static void expect(const char *what, const char *want)
{
    int ok = strcmp(trace, want) == 0;
    printf("%s %-34s got: %s\n", ok ? "PASS" : "FAIL", what, trace);
    if (!ok) { printf("     want: %s\n", want); fails++; }
    trace[0] = 0;
}

int main(void)
{
    uintptr_t pages[] = { 0x29000, 0x4a000, 0x4b000, 0x247000, 0x2be000, 0x2bf000, 0x2c0000,
                          0x343000, 0x34a000, 0x46d000, 0x477000, 0x1872000, 0x1876000 };
    for (unsigned i = 0; i < sizeof pages / sizeof *pages; i++) map_page(pages[i]);
    stub(0x29ed8, s_getinst); stub(0x4a740, s_settype); stub(0x4a7f0, s_gettype);
    stub(0x4b2a0, s_maxmin); stub(0x4b4d4, s_qvalid); stub(0x4b350, s_setq);
    stub(0x4ac98, s_setlevel); stub(0x4b4cc, s_getbpm); stub(0x2bfa34, s_orig_type); stub(0x2bedfc, s_orig_enable);
    stub(0x2c0c10, s_orig_beat); stub(0x2475d0, s_orig_aec); stub(0x343414, s_ismsg);
    stub(0x34a930, s_callonmsg); stub(0x4b0a8, s_getpct); stub(0x4aff8, s_setpct);
    stub(0x247a50, s_updmaster); stub(0x247a1c, s_getmaster); stub(0x247a14, s_getmthunk);
    /* vtable slots hold the stock function addresses */
    W32(0x4777f8 + 52) = 0x2bfa34; W32(0x4777f8 + 56) = 0x2bedfc; W32(0x4777f8 + 64) = 0x2c0c10;
    W32(0x46d3dc) = 0x2475d0; W32(0x46d3cc) = 0x247a50; W32(0x46d3f4) = 0x247a1c; W32(0x46d438) = 0x247a14;
    /* object chain: holder+0x110 -> mgr; mgr+0xe4 -> mixer; mixer+0xa0 -> mi */
    static uint32_t mgr[128], mixer[128], mi[128], mm[32];
    W32(0x1872d70 + 0x110) = (uint32_t)(uintptr_t)mgr;
    mgr[0xe4 / 4] = (uint32_t)(uintptr_t)mixer;
    mixer[0xa0 / 4] = (uint32_t)(uintptr_t)mi;
    mi[0] = 0x4777f8; mi[60 / 4] = 3; /* stock: switch at REVERB */ ((uint8_t *)mi)[68] = 0;
    mi[236 / 4] = 1; float lv = 0.5f; memcpy(&mi[228 / 4], &lv, 4);
    W32(0x18764cc) = (uint32_t)(uintptr_t)mm;
    unlink("/dev/shm/rrfx_ui100.log"); unlink("/dev/shm/rrfx_req"); unlink("/dev/shm/rrfx_debug");

    void *h = dlopen("./rrfx_ui100.so", RTLD_NOW);
    if (!h) { printf("DLOPEN FAIL %s\n", dlerror()); return 2; }
    printf("%s vtable patched: type=%x enable=%x beat=%x aec=%x\n",
           (W32(0x4777f8 + 52) != 0x2bfa34 && W32(0x46d3dc) != 0x2475d0) ? "PASS" : "FAIL",
           W32(0x4777f8 + 52), W32(0x4777f8 + 56), W32(0x4777f8 + 64), W32(0x46d3dc));
    trace[0] = 0;
    typedef int (*mf)(void *, int);
    mf v_type = (mf)(uintptr_t)W32(0x4777f8 + 52), v_en = (mf)(uintptr_t)W32(0x4777f8 + 56),
       v_beat = (mf)(uintptr_t)W32(0x4777f8 + 64);
    typedef int (*af)(void *, const void *, int *);
    af v_aec = (af)(uintptr_t)W32(0x46d3dc);

#define CHECK(cond, what) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", what); if (!ok_) fails++; } while (0)
#define REQ(s) do { FILE *f = fopen("/dev/shm/rrfx_req", "w"); fputs(s, f); fclose(f); usleep(700000); } while (0)
    /* 1. select Trans while FX OFF: stock-off path = switchMaxMin, code60 = 4 */
    REQ("Trans\n");
    expect("select Trans (FX off)", "callOnMsg;maxmin(2);qvalid;setQ(1);setLevel(0x3f000000);");
    printf("%s code60=%d (want 4 -> TRANS name)\n", mi[15] == 4 ? "PASS" : "FAIL", (int)mi[15]); if (mi[15] != 4) fails++;
    /* 2. press FX ON: override path sets engine type 2 */
    v_en(mi, 1);
    expect("FX ON with override", "setType(2);setLevel(0x3f000000);");
    printf("%s fxon=%d engine=%d\n", ((uint8_t *)mi)[68] == 1 && engine_type == 2 ? "PASS" : "FAIL", ((uint8_t *)mi)[68], engine_type);
    /* 3. arrows on Trans: stock beat path, code60 untouched */
    v_beat(mi, 1);
    expect("arrow on Trans", "ORIG_beat(1,code60=4);");
    /* 4. select Pitch while FX ON: setType, code60 = 6; arrows use percent (3) */
    REQ("8");
    expect("select Pitch (FX on)", "callOnMsg;setType(8);qvalid;setQ(1);setLevel(0x3f000000);");
    /* 4b. Pitch ladder: -50 is the bottom; < does nothing and returns 0 */
    { int r = v_beat(mi, -1);
      expect("P1 Pitch < at -50: no change", "");
      CHECK(r == 0 && pct == -50, "P1 returns 0 at the bottom (stock limit behaviour)"); }
    { static const int up[] = { -25, -10, 10, 25, 50, 75, 100 }; int ok = 1;
      for (int i = 0; i < 7; i++) { v_beat(mi, 1); if (pct != up[i]) ok = 0; }
      trace[0] = 0;
      CHECK(ok, "P2 Pitch > walks -25,-10,+10,+25,+50,+75,+100");
      int r = v_beat(mi, 1);
      CHECK(r == 0 && pct == 100, "P3 Pitch > at +100: no change, returns 0"); trace[0] = 0; }
    { static const int dn[] = { 75, 50, 25, 10, -10, -25, -50 }; int ok = 1;
      for (int i = 0; i < 7; i++) { v_beat(mi, -1); if (pct != dn[i]) ok = 0; }
      trace[0] = 0;
      CHECK(ok, "P4 Pitch < walks back down to -50"); }
    pct = 37; v_beat(mi, 1);
    expect("P5 off-ladder 37 > snaps to 50", "setPct(50);");
    pct = 37; v_beat(mi, -1);
    expect("P6 off-ladder 37 < snaps to 25", "setPct(25);");
    pct = 0; v_beat(mi, 1);
    expect("P7 0 > goes to +10 (no 0 step)", "setPct(10);");
    CHECK(mi[15] == 6, "P8 code60 stays 6 (PITCH name), never swapped");
    CHECK(engine_type == 8, "P9 engine type still Pitch");
    /* 5. select Reverb: stock forces quantize off without asking engine */
    REQ("reverb");
    expect("select Reverb (quantize forced off)", "callOnMsg;setType(1);setQ(0);setLevel(0x3f000000);");
    /* 6. FX OFF goes to stock handler */
    ((uint8_t *)mi)[68] = 1; v_en(mi, 0);
    expect("FX OFF -> stock", "ORIG_enable(0);");
    /* 7. physical switch moved: stock handler runs, override cleared */
    v_type(mi, 1);
    ((uint8_t *)mi)[68] = 0; v_en(mi, 1);
    expect("switch moved then FX ON -> stock", "ORIG_type(1);ORIG_enable(1);");
    /* 8. garbage request refused, nothing called */
    REQ("banana");
    expect("garbage request refused", "");
    /* ---------------- gesture tests ---------------- */
    uint32_t *vt_upd = (uint32_t *)0x46d3cc;
    typedef int (*uf)(void *, int); uf v_upd = (uf)(uintptr_t)*vt_upd;
    typedef void *(*gf)(void *); gf v_getm = (gf)(uintptr_t)W32(0x46d3f4), v_thunk = (gf)(uintptr_t)W32(0x46d438);
    uint8_t *st = (uint8_t *)mixer + 184;
    uint8_t key[64]; int x = -1;
    /* drive one key event the way MixerSkeleton::onKey would: aec, then dispatch */
#define KEY(id, act) do { memset(key, 0, sizeof key); key[8] = (id) & 0xff; key[9] = (id) >> 8; key[11] = (act); \
        int ev = v_aec(mixer, key, &x); if (ev == 0x200e) v_beat(mi, -1); else if (ev == 0x200f) v_beat(mi, 1); } while (0)
#define L 0x4491
#define R 0x4492
    /* start: physical switch on ECHO, FX on, no override */
    v_type(mi, 1); mi[15] = 1; ((uint8_t *)mi)[68] = 1; trace[0] = 0;
    /* G1: normal single taps still step the parameter */
    KEY(L, 0); KEY(L, 2);
    expect("G1 normal tap < steps param", "ORIG_beat(-1,code60=1);");
    /* G2: short chord (< 1.2 s): 2nd press swallowed, 1st step undone, no mode */
    KEY(L, 0); usleep(30000); KEY(R, 0); usleep(300000); KEY(L, 2); KEY(R, 2);
    expect("G2 short chord: undo, no mode", "ORIG_beat(-1,code60=1);ORIG_beat(1,code60=1);");
    /* G3: hold both 1.7 s -> nothing on screen changes while held; select mode on release */
    KEY(L, 0); usleep(4000); KEY(R, 0); usleep(1700000);
    v_upd(mixer, 1); usleep(300000);
    *(int32_t *)(st + 12) = 1285;
    CHECK(*(int32_t *)(st + 12) == 1285 && st[8] == 1, "G3 holding both: BPM and name colour untouched");
    KEY(L, 2); KEY(R, 2);
    trace[0] = 0;
    /* G4: browse: > > shows Flanger.. from Echo(5): 6 Delay, 7 Spiral; params untouched */
    KEY(R, 0); KEY(R, 2); KEY(R, 0); KEY(R, 2);
    expect("G4 browse swallows arrows (no param step)", "");
    { st[8] = 1; *(int32_t *)(st + 12) = 1285; usleep(400000);
      CHECK(*(int32_t *)(st + 12) == 1285, "G4b BPM never touched in select mode");
      CHECK(st[8] == 1, "G4b FX-on byte (name colour) never touched");
      CHECK(st[10] == 2, "G4b candidate name shown with no refresh calls"); }
    v_getm(mixer);
    CHECK(st[10] == 2, "G4 display shows Spiral code 2 while browsing (getMasterStat)");
    v_thunk((char *)mixer + 116);
    CHECK(st[10] == 2, "G4 thunk path overlays too");
    CHECK(mi[15] == 1, "G4 real code60 untouched while browsing");
    /* G5: < once -> Delay(6); then hold both 1.6 s -> commit Delay */
    KEY(L, 0); KEY(L, 2);
    KEY(L, 0); usleep(5000); KEY(R, 0); usleep(1600000); KEY(R, 2); KEY(L, 2);
    expect("G5 commit Delay (FX on)", "setType(6);qvalid;setQ(1);setLevel(0x3f000000);");
    CHECK(mi[15] == 0, "G5 code60=0 -> DELAY name");
    v_upd(mixer, 1);
    usleep(300000);
    CHECK(st[10] == 0 && st[8] == 1 && *(int32_t *)(st + 12) == 1285, "G5 after commit: real name, colour, BPM restored");
    /* G6: enter, then 10 s idle -> timeout, nothing changed */
    KEY(L, 0); KEY(R, 0); usleep(1600000); KEY(L, 2); KEY(R, 2); trace[0] = 0;
    KEY(R, 0); KEY(R, 2);
    printf("     (waiting 10.5 s for select-mode timeout)\n"); fflush(stdout);
    st[10] = 2; usleep(10500000);
    CHECK(st[10] == 0 && st[8] == 1 && *(int32_t *)(st + 12) == 1285, "G6 timeout: real name + BPM restored with NO refresh call");
    KEY(R, 0); KEY(R, 2);
    expect("G6 after timeout arrows step param again", "ORIG_beat(1,code60=0);");
    /* G7: enter, then physical switch moved -> cancelled */
    KEY(L, 0); KEY(R, 0); usleep(1600000); KEY(L, 2); KEY(R, 2);
    v_type(mi, 3); trace[0] = 0;
    KEY(R, 0); KEY(R, 2);
    expect("G7 switch cancels select mode (stub switch leaves code60)", "ORIG_beat(1,code60=0);");
    /* G8: 1.3 s hold now enters select mode (HOLD_MS 1200) */
    v_type(mi, 1); mi[15] = 1; ((uint8_t *)mi)[68] = 1; trace[0] = 0;
    KEY(L, 0); usleep(4000); KEY(R, 0); usleep(1300000); KEY(L, 2); KEY(R, 2);
    trace[0] = 0; KEY(R, 0); KEY(R, 2);
    expect("G8 1.3 s hold entered select mode (arrow swallowed)", "");
    usleep(10500000); trace[0] = 0;            /* let it time out */
    /* G9: 1.1 s is still too short */
    KEY(L, 0); usleep(4000); KEY(R, 0); usleep(1100000); KEY(L, 2); KEY(R, 2);
    trace[0] = 0; KEY(R, 0); KEY(R, 2);
    expect("G9 1.1 s hold too short: arrow steps param", "ORIG_beat(1,code60=1);");
    /* G10: Pitch chord undo restores the exact previous percent */
    REQ("pitch"); trace[0] = 0; pct = 25;
    KEY(L, 0); usleep(30000); KEY(R, 0); usleep(300000); KEY(L, 2); KEY(R, 2);
    expect("G10 Pitch short chord: step then exact undo", "setPct(10);setPct(25);");
    CHECK(pct == 25, "G10 Pitch value back to 25 after the chord");
    /* G11: other keys pass through unchanged; logged ONLY with the debug flag */
    { memset(key, 0, sizeof key); key[8] = 0x20; key[9] = 0x50; int ev = v_aec(mixer, key, &x);
      CHECK(ev == 0, "G11 non-BEAT key passes through unchanged (stub returns 0)");
      CHECK(system("grep -q KEYLOG /dev/shm/rrfx_ui100.log") != 0, "G11 debug off by default: no KEYLOG");
      FILE *f = fopen("/dev/shm/rrfx_debug", "w"); fclose(f); usleep(600000);
      key[8] = 0xa5; ev = v_aec(mixer, key, &x);
      CHECK(ev == 0 && system("grep -q 'KEYLOG id=0x50a5' /dev/shm/rrfx_ui100.log") == 0,
            "G12 debug flag present: key logged, event unchanged");
      unlink("/dev/shm/rrfx_debug"); usleep(600000);
      key[8] = 0xa4; v_aec(mixer, key, &x);
      CHECK(system("grep -q 'KEYLOG id=0x50a4' /dev/shm/rrfx_ui100.log") != 0, "G13 debug flag removed: logging stops"); }
    trace[0] = 0;
    printf("\n--- /dev/shm/rrfx_ui.log ---\n"); fflush(stdout);
    system("cat /dev/shm/rrfx_ui100.log");
    printf("\nRESULT %s (%d failures)\n", fails ? "FAIL" : "ALL_PASS", fails);
    return fails != 0;
}
