/**
 * Midnight Club 3: DUB Edition -- hand-written overrides of lifted functions
 * and ICALL diagnostics. Started from xboxrecomp's templates/new-game (MIT).
 *
 * tools/regen.sh passes this file to the recompiler (--exclude-manual): any
 * sub_XXXXXXXX defined here is not generated, so the hand-written body links
 * instead. After editing, LIFT_ONLY=1 tools/regen.sh.
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* The generated register model (gen/recomp_types.h): the guest registers,
 * MEM32 and friends, and the ICALL trace ring the runtime owns. Include it
 * rather than re-declaring registers by hand: they are thread-local
 * (RECOMP_TLS), and a plain `extern uint32_t g_eax` is a TLS mismatch that
 * GCC refuses to link on Linux and the Switch. */
#include "recomp_types.h"

/* ── Native replacements ───────────────────────────────────── */

/* memmove (CRT), 0x002AB610 -- func_id: memmove, confidence 0.9.
 *
 * The lifted body dispatches its head and tail copies through two jump tables
 * (0x002AB670, 0x002AB76C) whose targets are labels inside the function. The
 * vendored lifter emits those as indirect calls to other functions, which
 * resolve to nothing ([ICALL] Failed to resolve VA 0x002AB8B4, first run): the
 * copy silently stops short. Upstream fixed this class of jump table after our
 * base; until it is merged, the host's memmove does the job (faster, too).
 *
 * cdecl: dest, src, count on the guest stack above the return address;
 * returns dest in eax; the caller pops the arguments, we pop the return
 * address as `ret` would. */
void sub_002AB610(void)
{
    uint32_t dst = MEM32(g_esp + 4);
    uint32_t src = MEM32(g_esp + 8);
    uint32_t len = MEM32(g_esp + 12);

    if (len)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), len);
    g_eax = dst;
    g_esp += 4;
}

/* Bink's MMX detection, 0x002B9C90 (called by the blit set-up sub_0037DB10).
 *
 * The classic test: flip EFLAGS.ID with pushfd/popfd to see whether CPUID
 * exists, then CPUID leaf 1, EDX bit 23 = MMX. The vendored lifter emits
 * pushfd, popfd and cpuid as silent no-ops, so the ID bit never "sticks",
 * Bink decides the CPU has no MMX, never installs its pixel routines
 * (0x5209C8.. stay 0), and the blit loop -- whose stdcall calls through those
 * null pointers are skipped without popping their argument -- drifts its
 * stack and never ends (2026-10-14 runs, [BINKLOOP] trace).
 *
 * The Xbox CPU is a Pentium III: CPUID and MMX are always there. Same
 * results and side effects as the original:
 *   [0x482AA4] = cached "has MMX" (-1 until detected)
 *   [0x52A43C] = "use MMX", unless the caller's flags carry 0x40000
 * cdecl: flags at [esp+4]; returns the detection result in eax. */
void sub_002B9C90(void)
{
    uint32_t flags = MEM32(g_esp + 4);
    uint32_t has_mmx = MEM32(0x482AA4);

    if ((int32_t)has_mmx < 0) {
        has_mmx = 1;
        MEM32(0x482AA4) = has_mmx;
    }
    MEM32(0x52A43C) = (has_mmx && !(flags & 0x40000u)) ? 1u : 0u;
    g_eax = has_mmx;
    g_esp += 4;
}

/* D3D fence wait, 0x003DF4D0 (D3D section): waits until the GPU-written
 * semaphore at *(device + 0x30) reaches a fence. The NV2A executor writes
 * BACK_END_WRITE_SEMAPHORE_RELEASE values there only once the project names
 * the address (nv2a_pb_set_semaphore_target, xboxrecomp nv2a_backend.h);
 * until then every release is dropped and the wait never ends (2026-10-14
 * run 15: main thread here with PUT == GET). MC3's D3D keeps its device
 * pointer at 0x003ECAE8 (mov esi, [0x3ECAE8] at the top of this function).
 * Name the semaphore on the first wait, then run the original: its kick-off
 * queues new releases, which now land. */
/*
 * The wait's last loop (0x003DF607) reads the time handed out once
 * (+0x2C, into a register) and spins while
 *     handed - target < handed - gpu      (unsigned)
 * i.e. while the GPU is behind the fence inside the window [.., handed].
 * Its poll-loop yield lets the other guest threads run -- MC3's loading
 * screen keeps drawing from its own thread -- and when they push and the
 * GPU runs past `handed` before this thread looks again, handed - gpu
 * wraps to ~4 G and the loop never ends: the career loading-screen hang
 * (2026-10-21; snapshot: fence 000F6AEF, GPU at 000F90B5, main thread
 * spinning there with the loading screen still animating). On the Xbox the
 * window is microseconds and nothing pushes in it. So while the original
 * runs, a spin hook checks at each yield: once the GPU has passed the fence
 * it stores the fence itself in the semaphore, which ends the loop on its
 * next read, and puts the GPU's value back when the wait returns (unless
 * the GPU has written a newer one). Lower is the safe direction: a reader
 * in between waits, never runs ahead. MC3_FENCE_FIX=0 turns it off.
 */
static uint32_t s_fence_sem_va;
static RECOMP_TLS uint32_t t_fence_target, t_fence_saved;
static RECOMP_TLS int t_fence_fixed;
extern void recomp_set_spin_hook(void (*fn)(void));

/* Compare-and-swap on guest memory: the executor writes it from its thread. */
static uint32_t fence_cas(uint32_t va, uint32_t expect, uint32_t want)
{
    uint32_t *p = (uint32_t *)((uintptr_t)va + g_xbox_mem_offset);
    __atomic_compare_exchange_n(p, &expect, want, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expect;                            /* the value found */
}

static void fence_spin_hook(void)
{
    uint32_t gpu;
    if (!s_fence_sem_va)
        return;
    if (t_fence_fixed) {
        /* Still spinning after the fix: some other loop -- give it the
         * real value back. */
        fence_cas(s_fence_sem_va, t_fence_target, t_fence_saved);
        t_fence_fixed = 0;
        return;
    }
    gpu = __atomic_load_n((uint32_t *)((uintptr_t)s_fence_sem_va + g_xbox_mem_offset), __ATOMIC_SEQ_CST);
    if (gpu == t_fence_target || (int32_t)(gpu - t_fence_target) < 0)
        return;                                /* not there yet, or exact */
    if (fence_cas(s_fence_sem_va, gpu, t_fence_target) == gpu) {
        static int logged;
        t_fence_saved = gpu;
        t_fence_fixed = 1;
        if (logged++ < 5)
            fprintf(stderr, "[D3D] fence wait: GPU at %08X ran past fence %08X and the "
                            "window -- released\n", gpu, t_fence_target);
    }
}

extern void sub_003DF4D0_gen(void);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
void sub_003DF4D0(void)
{
    static uint32_t sem_va;
    static int fix = -1;
    uint32_t save_target = t_fence_target, save_saved = t_fence_saved;
    int save_fixed = t_fence_fixed;

    if (!sem_va) {
        uint32_t device = MEM32(0x003ECAE8u);
        uint32_t sem = device ? MEM32(device + 0x30u) : 0;
        if (sem) {
            sem_va = sem;
            nv2a_pb_set_semaphore_target(sem_va);
            fprintf(stderr, "[D3D] GPU semaphore at 0x%08X (device 0x%08X)\n",
                    sem_va, device);
            s_fence_sem_va = sem_va;
        }
    }
    if (fix < 0) {
        const char *e = getenv("MC3_FENCE_FIX");
        fix = !(e && e[0] == '0');
    }
    if (!fix || !s_fence_sem_va) {
        sub_003DF4D0_gen();
        return;
    }
    t_fence_target = MEM32(g_esp + 4);         /* (time, flags), stdcall */
    t_fence_fixed = 0;
    recomp_set_spin_hook(fence_spin_hook);
    sub_003DF4D0_gen();
    if (t_fence_fixed)
        fence_cas(s_fence_sem_va, t_fence_target, t_fence_saved);
    t_fence_target = save_target;
    t_fence_saved = save_saved;
    t_fence_fixed = save_fixed;
    recomp_set_spin_hook(save_target ? fence_spin_hook : NULL);
}

/* ── Differential tests (src/difftest.c, tools/difftest.py) ── */

/* Wrapped so one call can be captured and replayed on real x86 (make difftest
 * FUNC=<addr> CALL=<n>). Unarmed, a wrapper costs one counter and a compare.
 * Movie frames and movie audio decode wrongly (2026-10-15); BinkDoFrame
 * (0x00377020) itself waits on Bink's I/O thread, so capture below it:
 *   0x0037CCD0  Bink video: decodes the frame's planes (4 x sub_0037AF10,
 *               block decoders 0x003798C0.., transforms 0x00380DF0..);
 *               called from BinkDoFrame at 0x0037758A
 *   0x00378B30  Bink audio: decodes the frame's audio (sub_00378640 and the
 *               .text maths at 0x002B9B00 / 0x002B99C0); at 0x0037730D */
extern void mc3_difftest_call(uint32_t func, void (*lifted)(void), long *calls);
extern long mc3_difftest_wanted(void);

/* ── Music: the WMA decoder XACT drives (MC3_WMA_TRACE=1) ──
 *
 * The music (audio/XStreams/mx.xwb, 110 tracks) is WMA, stereo 44.1 kHz;
 * XACT decodes it on the CPU through the WMADEC section's media object and
 * plays the PCM from a looping buffer voice. On 2026-10-16 that voice ran
 * but every sample it read was zero, so the menu song was silent. This logs
 * what the decoder is asked and answers: XACT creates it with
 * sub_003EFE0D (0x003D6868, when the wave's format tag is 2 = WMA); its
 * vtable is at 0x0046EEA8 (constructor sub_003EFD63). Per method: the first
 * calls in full, then every 500th, with stdcall arguments (this, a, b), the
 * HRESULT, and for any argument that looks like an XMEDIAPACKET
 * {pvBuffer, dwMaxSize, pdwCompletedSize, ...} the completed size and the
 * peak 16-bit sample in it. Each is difftest-capable: Process is slot 4,
 * `make difftest FUNC=003EFCE7 CALL=<n>`. */
static int wma_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_WMA_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}

/* An XMEDIAPACKET's completed size and 16-bit peak, or 0 if `va` is not one. */
static int wma_packet(uint32_t va, uint32_t *done, uint32_t *max, int *peak)
{
    uint32_t buf, size, done_va, n, i;
    if (va < 0x10000u || va >= 0x84000000u)
        return 0;
    buf = MEM32(va); size = MEM32(va + 4); done_va = MEM32(va + 8);
    if (buf < 0x10000u || buf >= 0x84000000u || !size || size > 0x400000u)
        return 0;
    n = done_va && done_va < 0x84000000u ? MEM32(done_va) : size;
    if (n > size) n = size;
    *done = n; *max = size; *peak = 0;
    for (i = 0; i + 1 < n; i += 2) {
        int v = (int16_t)MEM16(buf + i);
        if (v < 0) v = -v;
        if (v > *peak) *peak = v;
    }
    return 1;
}

static void wma_call(uint32_t addr, int method, void (*fn)(void))
{
    static long calls[11], dcalls[11];
    uint32_t a0, a1, a2;
    long n;
    /* Creation is rare and the question is often whether it happens at
     * all: logged (first 4) with or without the trace. */
    if (!wma_trace_on() && (method != 10 || calls[10] >= 4)) {
        mc3_difftest_call(addr, fn, &dcalls[method]);
        return;
    }
    a0 = MEM32(g_esp + 4); a1 = MEM32(g_esp + 8); a2 = MEM32(g_esp + 12);
    n = ++calls[method];
    mc3_difftest_call(addr, fn, &dcalls[method]);
    if (n <= 12 || n % 500 == 0) {
        uint32_t d, m;
        int pk;
        fprintf(stderr, "[WMA] %s #%ld (%08X, %08X, %08X) -> %08X",
                method == 10 ? "create" : "method", n, a0, a1, a2, g_eax);
        if (method < 10)
            fprintf(stderr, " [vtable +0x%02X]", method * 4);
        if (wma_packet(a1, &d, &m, &pk))
            fprintf(stderr, " | a: %u/%u bytes peak %d", d, m, pk);
        if (wma_packet(a2, &d, &m, &pk))
            fprintf(stderr, " | b: %u/%u bytes peak %d", d, m, pk);
        fprintf(stderr, "\n");
    }
}

/* Written out, not by macro: tools/recomp/manual_scan.py reads the text. */
extern void sub_003EFE0D_gen(void);
void sub_003EFE0D(void) { wma_call(0x003EFE0Du, 10, sub_003EFE0D_gen); }   /* create: (out object, ...) */
extern void sub_003EF9BF_gen(void);
void sub_003EF9BF(void) { wma_call(0x003EF9BFu, 2, sub_003EF9BF_gen); }
extern void sub_003EFA01_gen(void);
void sub_003EFA01(void) { wma_call(0x003EFA01u, 3, sub_003EFA01_gen); }
extern void sub_003EFCE7_gen(void);
void sub_003EFCE7(void) { wma_call(0x003EFCE7u, 4, sub_003EFCE7_gen); }
extern void sub_003EF9FC_gen(void);
void sub_003EF9FC(void) { wma_call(0x003EF9FCu, 5, sub_003EF9FC_gen); }
extern void sub_003EFC83_gen(void);
void sub_003EFC83(void) { wma_call(0x003EFC83u, 6, sub_003EFC83_gen); }
extern void sub_003EFABA_gen(void);
void sub_003EFABA(void) { wma_call(0x003EFABAu, 7, sub_003EFABA_gen); }
extern void sub_003EFB20_gen(void);
void sub_003EFB20(void) { wma_call(0x003EFB20u, 8, sub_003EFB20_gen); }
extern void sub_003EF8EF_gen(void);
void sub_003EF8EF(void) { wma_call(0x003EF8EFu, 9, sub_003EF8EF_gen); }

/* ── Music: XACT's API as the game calls it (MC3_XACT_TRACE=1) ──
 *
 * With MC3_WMA_TRACE on, XACT never created a WMA decoder (2026-10-16):
 * the songs fail before decoding, and the in-game music player shuffles
 * through them forever. These are the 37 XACT functions the game calls
 * directly (0x003D1159..0x003D1794; the last ten are jmp thunks). Each
 * logs its first 3 calls, and every call returning a failure HRESULT (bit
 * 31 set; a few may return counts or pointers instead), with ecx and four
 * stack arguments. */
extern void mc3_difftest_call(uint32_t func, void (*lifted)(void), long *calls);

static int xact_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_XACT_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}

static void xact_call(uint32_t addr, int slot, void (*fn)(void))
{
    static long calls[64], failures, dcalls[64];
    uint32_t c, a0, a1, a2, a3;
    long n;
    /* Every XACT wrapper can also be captured: make difftest FUNC=<addr>. */
    if (!xact_trace_on()) { mc3_difftest_call(addr, fn, &dcalls[slot]); return; }
    c = g_ecx;
    a0 = MEM32(g_esp + 4); a1 = MEM32(g_esp + 8);
    a2 = MEM32(g_esp + 12); a3 = MEM32(g_esp + 16);
    uint32_t desc[4] = { 0, 0, 0, 0 };
    n = ++calls[slot];
    /* sub_003D162D / sub_003D8B33's second argument is a descriptor:
     * flags (bit 2: streamed), then three words (+0xC: the stream's
     * source). Read before the call, while it is the caller's. */
    if ((addr == 0x003D162Du || addr == 0x003D8B33u) && a1 >= 0x10000u && a1 < 0x84000000u) {
        desc[0] = MEM32(a1); desc[1] = MEM32(a1 + 4);
        desc[2] = MEM32(a1 + 8); desc[3] = MEM32(a1 + 12);
    }
    mc3_difftest_call(addr, fn, &dcalls[slot]);
    if (n <= 3 || ((g_eax & 0x80000000u) && failures++ < 400)) {
        fprintf(stderr, "[XACT] sub_%08X #%ld (ecx %08X; %08X %08X %08X %08X) -> %08X%s",
                addr, n, c, a0, a1, a2, a3, g_eax,
                (g_eax & 0x80000000u) ? "  FAILED" : "");
        if (desc[0] | desc[1] | desc[2] | desc[3])
            fprintf(stderr, " desc {%08X %08X %08X %08X}", desc[0], desc[1], desc[2], desc[3]);
        fprintf(stderr, "\n");
    }
}

/* Written out, not by macro: tools/recomp/manual_scan.py reads the text. */
/* XACT's variation choice, 0x003D796F: thiscall (table offset, flags,
 * float), returns the chosen entry or 0. sub_003D7E76 turns 0 into wave
 * index 0xFFFF and sub_003D89F6 into E_FAIL (0x003D8AA5) -- every song in
 * the music player. Pure computation (mode (flags >> 13) & 15, an RNG at
 * sub_003D21D9 / sub_003D21E2): wrapped for difftest, and logged with
 * MC3_XACT_TRACE. The first call is at boot (the menu song). */
extern void sub_003D796F_gen(void);
void sub_003D796F(void)
{
    static long calls, logged;
    uint32_t c = g_ecx, a0 = MEM32(g_esp + 4), a1 = MEM32(g_esp + 8), a2 = MEM32(g_esp + 12);
    uint32_t entry = 0;
    if (c >= 0x10000u && c < 0x84000000u)
        entry = MEM32(MEM32(c + 4) + a0);
    mc3_difftest_call(0x003D796Fu, sub_003D796F_gen, &calls);
    if (xact_trace_on() && logged++ < 40)
        fprintf(stderr, "[XACT] variation sub_003D796F #%ld (ecx %08X; %08X %08X %08X)"
                " table word %08X (mode %u, count %u) -> %08X\n", logged, c, a0, a1, a2,
                entry, (entry >> 13) & 15, entry & 0x1FFF, g_eax);
}

/* XACT sound states (2026-10-17). Starting a sound moves it to state 5
 * (sub_003D7750 -> sub_003D549F(5)); the only refused transition is from
 * state 7, "stopped", and that refusal is the music player's E_FAIL: every
 * song is stopped before it starts. sub_003D5C0F is XACT's stop (state 7 +
 * cleanup), called from nine places. With MC3_XACT_TRACE these log who stops
 * a sound (the return address names the caller) and every move to state 7 or
 * refused transition, with the sound object and its state before. */
extern void sub_003D5C0F_gen(void);
void sub_003D5C0F(void)
{
    static long n;
    uint32_t snd = g_ecx, ret = MEM32(g_esp), flags = MEM32(g_esp + 4);
    uint32_t state = snd >= 0x10000u && snd < 0x84000000u ? MEM16(snd + 0x30) : 0xFFFF;
    if (xact_trace_on() && n++ < 200)
        fprintf(stderr, "[XACT] stop sound %08X (state %u, flags %08X) called from %08X\n",
                snd, state, flags, ret);
    sub_003D5C0F_gen();
}

extern void sub_003D549F_gen(void);
void sub_003D549F(void)
{
    static long n;
    uint32_t snd = g_ecx, ret = MEM32(g_esp), want = MEM32(g_esp + 4);
    uint32_t before = snd >= 0x10000u && snd < 0x84000000u ? MEM16(snd + 0x30) : 0xFFFF;
    sub_003D549F_gen();
    if (xact_trace_on() && (want == 7 || !g_eax) && n++ < 300)
        fprintf(stderr, "[XACT] sound %08X state %u -> %u %s (from %08X)\n",
                snd, before, want, g_eax ? "ok" : "REFUSED", ret);
}

/* One level down, inside sub_003D162D -> sub_003D8B33 (2026-10-16: E_FAIL
 * 403x while the music player shuffled): which step fails. */
extern void sub_003D8B33_gen(void);
void sub_003D8B33(void) { xact_call(0x003D8B33u, 40, sub_003D8B33_gen); }
extern void sub_003D0E04_gen(void);
void sub_003D0E04(void) { xact_call(0x003D0E04u, 41, sub_003D0E04_gen); }   /* streamed (flag 4) */
extern void sub_003D89F6_gen(void);
void sub_003D89F6(void) { xact_call(0x003D89F6u, 42, sub_003D89F6_gen); }
extern void sub_003D7C30_gen(void);
void sub_003D7C30(void) { xact_call(0x003D7C30u, 43, sub_003D7C30_gen); }
extern void sub_003DAB5B_gen(void);
void sub_003DAB5B(void) { xact_call(0x003DAB5Bu, 44, sub_003DAB5B_gen); }
extern void sub_003D1159_gen(void);
void sub_003D1159(void) { xact_call(0x003D1159u, 0, sub_003D1159_gen); }
extern void sub_003D118B_gen(void);
void sub_003D118B(void) { xact_call(0x003D118Bu, 1, sub_003D118B_gen); }
extern void sub_003D1230_gen(void);
void sub_003D1230(void) { xact_call(0x003D1230u, 2, sub_003D1230_gen); }
extern void sub_003D1248_gen(void);
void sub_003D1248(void) { xact_call(0x003D1248u, 3, sub_003D1248_gen); }
extern void sub_003D128E_gen(void);
void sub_003D128E(void) { xact_call(0x003D128Eu, 4, sub_003D128E_gen); }
extern void sub_003D12CD_gen(void);
void sub_003D12CD(void) { xact_call(0x003D12CDu, 5, sub_003D12CD_gen); }
extern void sub_003D1310_gen(void);
void sub_003D1310(void) { xact_call(0x003D1310u, 6, sub_003D1310_gen); }
extern void sub_003D1353_gen(void);
void sub_003D1353(void) { xact_call(0x003D1353u, 7, sub_003D1353_gen); }
extern void sub_003D1392_gen(void);
void sub_003D1392(void) { xact_call(0x003D1392u, 8, sub_003D1392_gen); }
extern void sub_003D13CD_gen(void);
void sub_003D13CD(void) { xact_call(0x003D13CDu, 9, sub_003D13CD_gen); }
extern void sub_003D140C_gen(void);
void sub_003D140C(void) { xact_call(0x003D140Cu, 10, sub_003D140C_gen); }
extern void sub_003D142C_gen(void);
void sub_003D142C(void) { xact_call(0x003D142Cu, 11, sub_003D142C_gen); }
extern void sub_003D146B_gen(void);
void sub_003D146B(void) { xact_call(0x003D146Bu, 12, sub_003D146B_gen); }
extern void sub_003D14A6_gen(void);
void sub_003D14A6(void) { xact_call(0x003D14A6u, 13, sub_003D14A6_gen); }
extern void sub_003D14E1_gen(void);
void sub_003D14E1(void) { xact_call(0x003D14E1u, 14, sub_003D14E1_gen); }
extern void sub_003D1520_gen(void);
void sub_003D1520(void) { xact_call(0x003D1520u, 15, sub_003D1520_gen); }
extern void sub_003D1563_gen(void);
void sub_003D1563(void) { xact_call(0x003D1563u, 16, sub_003D1563_gen); }
extern void sub_003D159B_gen(void);
void sub_003D159B(void) { xact_call(0x003D159Bu, 17, sub_003D159B_gen); }
extern void sub_003D15B3_gen(void);
void sub_003D15B3(void) { xact_call(0x003D15B3u, 18, sub_003D15B3_gen); }
extern void sub_003D15CF_gen(void);
void sub_003D15CF(void) { xact_call(0x003D15CFu, 19, sub_003D15CF_gen); }
extern void sub_003D15FA_gen(void);
void sub_003D15FA(void) { xact_call(0x003D15FAu, 20, sub_003D15FA_gen); }
extern void sub_003D162D_gen(void);
void sub_003D162D(void) { xact_call(0x003D162Du, 21, sub_003D162D_gen); }
extern void sub_003D1660_gen(void);
void sub_003D1660(void) { xact_call(0x003D1660u, 22, sub_003D1660_gen); }
extern void sub_003D1697_gen(void);
void sub_003D1697(void) { xact_call(0x003D1697u, 23, sub_003D1697_gen); }
extern void sub_003D16CE_gen(void);
void sub_003D16CE(void) { xact_call(0x003D16CEu, 24, sub_003D16CE_gen); }
extern void sub_003D1701_gen(void);
void sub_003D1701(void) { xact_call(0x003D1701u, 25, sub_003D1701_gen); }
extern void sub_003D1734_gen(void);
void sub_003D1734(void) { xact_call(0x003D1734u, 26, sub_003D1734_gen); }
extern void sub_003D1767_gen(void);
void sub_003D1767(void) { xact_call(0x003D1767u, 27, sub_003D1767_gen); }
extern void sub_003D176C_gen(void);
void sub_003D176C(void) { xact_call(0x003D176Cu, 28, sub_003D176C_gen); }
extern void sub_003D1771_gen(void);
void sub_003D1771(void) { xact_call(0x003D1771u, 29, sub_003D1771_gen); }
extern void sub_003D1776_gen(void);
void sub_003D1776(void) { xact_call(0x003D1776u, 30, sub_003D1776_gen); }
extern void sub_003D177B_gen(void);
void sub_003D177B(void) { xact_call(0x003D177Bu, 31, sub_003D177B_gen); }
extern void sub_003D1780_gen(void);
void sub_003D1780(void) { xact_call(0x003D1780u, 32, sub_003D1780_gen); }
extern void sub_003D1785_gen(void);
void sub_003D1785(void) { xact_call(0x003D1785u, 33, sub_003D1785_gen); }
extern void sub_003D178A_gen(void);
void sub_003D178A(void) { xact_call(0x003D178Au, 34, sub_003D178A_gen); }
extern void sub_003D178F_gen(void);
void sub_003D178F(void) { xact_call(0x003D178Fu, 35, sub_003D178F_gen); }
extern void sub_003D1794_gen(void);
void sub_003D1794(void) { xact_call(0x003D1794u, 36, sub_003D1794_gen); }

/* ── Movies: where a frame's time goes (MC3_BINK_TRACE=1) ──
 *
 * The movie loop (sub_0032E7F0) per frame: BinkDoFrame, lock the texture,
 * BinkCopyToBuffer, BinkNextFrame, then spin on BinkWait until the next frame
 * is due; its caller draws and presents. Once a second this prints the
 * average milliseconds per frame in each phase -- "rest" is everything from
 * the end of the wait to the next BinkDoFrame (draw, Present, game). */

enum { BT_DO, BT_VIDEO, BT_AUDIO, BT_LOCK, BT_COPY, BT_NEXT, BT_WAIT, BT_REST, BT_N };
static const char *const s_bt_name[BT_N] =
    { "doframe", "video", "audio", "lock", "copy", "next", "wait", "rest" };
static double s_bt_sum[BT_N], s_bt_mark[BT_N], s_bt_report;
static long s_bt_frames, s_bt_polls;
static double s_bt_wait_start;       /* 0 when not in the wait loop */

static double bt_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int bt_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_BINK_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}

static void bt_report(double now)
{
    char line[256];
    int n = 0, i;
    if (!s_bt_report) { s_bt_report = now; return; }
    if (now - s_bt_report < 1000.0 || !s_bt_frames) return;
    n += snprintf(line + n, sizeof line - n, "[BINKT] %.1f fps, ms/frame:",
                  s_bt_frames * 1000.0 / (now - s_bt_report));
    for (i = 0; i < BT_N; i++)
        n += snprintf(line + n, sizeof line - n, " %s %.1f", s_bt_name[i], s_bt_sum[i] / s_bt_frames);
    snprintf(line + n, sizeof line - n, " (%ld polls/frame)", s_bt_polls / s_bt_frames);
    fprintf(stderr, "%s\n", line);
    memset(s_bt_sum, 0, sizeof s_bt_sum);
    s_bt_frames = s_bt_polls = 0;
    s_bt_report = now;
}

/* Time one lifted call into phase `p`. */
static void bt_time(int p, void (*fn)(void))
{
    double t0;
    if (!bt_on()) { fn(); return; }
    t0 = bt_now();
    fn();
    s_bt_sum[p] += bt_now() - t0;
}

/* BinkDoFrame, 0x00377020: stdcall (hbink). Opens a frame. */
extern void sub_00377020_gen(void);
void sub_00377020(void)
{
    double t0;
    if (!bt_on()) { sub_00377020_gen(); return; }
    t0 = bt_now();
    if (s_bt_mark[BT_REST]) s_bt_sum[BT_REST] += t0 - s_bt_mark[BT_REST];
    bt_report(t0);
    s_bt_frames++;
    sub_00377020_gen();
    s_bt_mark[BT_LOCK] = bt_now();
    s_bt_sum[BT_DO] += s_bt_mark[BT_LOCK] - t0;
}

/* BinkCopyToBuffer, 0x00376FE0: the YUV -> RGB copy into the locked texture.
 * stdcall (hbink, dest, pitch, height, x, y, flags), returns 0 when copied.
 * With the renderer upscaling the frame (bink_native.c mc3_bink_native_copy)
 * the copy is skipped: nothing but the drawn texture reads it. */
extern int mc3_bink_native_copy(uint32_t hbink, uint32_t dest_va, const uint8_t *dest,
                                uint32_t pitch, uint32_t height, uint32_t x, uint32_t y, int after);
extern void sub_00376FE0_gen(void);
void sub_00376FE0(void)
{
    uint32_t hbink = MEM32(g_esp + 4), dest = MEM32(g_esp + 8), pitch = MEM32(g_esp + 12);
    uint32_t height = MEM32(g_esp + 16), x = MEM32(g_esp + 20), y = MEM32(g_esp + 24);
    if (bt_on() && s_bt_mark[BT_LOCK]) {
        s_bt_sum[BT_LOCK] += bt_now() - s_bt_mark[BT_LOCK];
        s_bt_mark[BT_LOCK] = 0;
    }
    if (dest && mc3_bink_native_copy(hbink, dest, (const uint8_t *)XBOX_PTR(dest),
                                     pitch, height, x, y, 0)) {
        g_eax = 0;
        g_esp += 4 + 28;                     /* return address + 7 arguments */
        return;
    }
    bt_time(BT_COPY, sub_00376FE0_gen);
    if (dest)
        mc3_bink_native_copy(hbink, dest, (const uint8_t *)XBOX_PTR(dest), pitch, height, x, y, 1);
}

/* BinkNextFrame, 0x00377630. */
extern void sub_00377630_gen(void);
void sub_00377630(void)
{
    bt_time(BT_NEXT, sub_00377630_gen);
}

/* BinkWait, 0x00376B50: stdcall (hbink), nonzero = too early, call again. */
extern void sub_00376B50_gen(void);
void sub_00376B50(void)
{
    double now;
    if (!bt_on()) { sub_00376B50_gen(); return; }
    now = bt_now();
    if (!s_bt_wait_start) s_bt_wait_start = now;
    s_bt_polls++;
    sub_00376B50_gen();
    if (!g_eax) {
        now = bt_now();
        s_bt_sum[BT_WAIT] += now - s_bt_wait_start;
        s_bt_wait_start = 0;
        s_bt_mark[BT_REST] = now;
    }
}

/* ── Movies: Bink video decoded by FFmpeg (src/bink_native.c) ── */

/* Bink's HBINK, as far as the native path needs it. */
#define BINK_WIDTH      0x00u
#define BINK_HEIGHT     0x04u
#define BINK_FRAMENUM   0x0Cu       /* 1-based frame being decoded */
#define BINK_DIRTY_MAP  0x3A0u      /* changed-block map, 1 byte per 16x16 block;
                                     * HBINK+0xDC points here, the decoder's 5th
                                     * argument */

extern int  mc3_bink_native_enabled(void);
extern void mc3_bink_native_open(uint32_t hbink, const char *xbox_name);
extern int  mc3_bink_native_frame(uint32_t hbink, int frame, uint8_t *dest, int w, int h);
extern void mc3_bink_native_close(uint32_t hbink);
extern void mc3_bink_native_audio(uint32_t hbink, int frame);
extern int  mc3_bink_native_audio_active(void);

/* MC3_SKIP_MOVIES: for unattended runs (headless, recordings) where nobody
 * is there to press Enter. Off by default: in a window, press it yourself.
 * A comma list of file names without the folder ("mc3intro.bik"), or 1 =
 * the boot intro (rockstar, sdlogo, mc3intro). While such a movie is open,
 * pad 1 pulses A + Start (3 frames held, 3 released: fresh presses) -- input
 * like a player's, through the title's own handling, so only what the game
 * lets you skip is skipped: the Rockstar and Rockstar San Diego logos play
 * in full (as on an Xbox), mc3intro is cut short. */
extern void usb_gamepad_set_held(int dev, uint8_t digital, uint8_t analog);
#define PAD_START 0x10u                         /* digital mask */
#define PAD_A     0x01u                         /* analog mask */
static uint32_t s_skip_hbink;                   /* the movie being pressed through */
static unsigned s_skip_frames;

static int movie_skipped(const char *path)
{
    const char *list = getenv("MC3_SKIP_MOVIES"), *base = path, *p;
    size_t n;

    if (!list || !*list || !strcmp(list, "0"))
        return 0;
    if (!strcmp(list, "1"))
        list = "rockstar.bik,sdlogo.bik,mc3intro.bik";
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    n = strlen(base);
    for (p = list; *p; ) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncasecmp(p, base, n))
            return 1;
        p += len + (e ? 1 : 0);
    }
    return 0;
}

/* BinkOpen, 0x003752F0: stdcall (name, flags), returns the HBINK. */
extern void sub_003752F0_gen(void);
void sub_003752F0(void)
{
    char name[260] = "";
    uint32_t name_va = MEM32(g_esp + 4);

    if (name_va)
        strncpy(name, (const char *)XBOX_PTR(name_va), sizeof name - 1);
    if (name[0] && movie_skipped(name)) {
        sub_003752F0_gen();
        s_skip_hbink = g_eax;
        s_skip_frames = 0;
        fprintf(stderr, "[BINK] pressing A+Start through %s (MC3_SKIP_MOVIES)\n", name);
    } else {
        sub_003752F0_gen();
    }
    if (name[0] && g_eax)
        mc3_bink_native_open(g_eax, name);
}

/* BinkClose, 0x003777D0: stdcall (hbink). */
extern void sub_003777D0_gen(void);
void sub_003777D0(void)
{
    if (s_skip_hbink && MEM32(g_esp + 4) == s_skip_hbink) {
        usb_gamepad_set_held(0, 0, 0);
        fprintf(stderr, "[BINK] skipped after %u frames\n", s_skip_frames);
        s_skip_hbink = 0;
    }
    mc3_bink_native_close(MEM32(g_esp + 4));
    sub_003777D0_gen();
}

/* Bink video decode, 0x0037CCD0: stdcall, 14 arguments -- (dest frame,
 * previous frame, 0, 0, changed-block map = HBINK+0x3A0, w, h, padded w, padded h,
 * packet, ...),
 * returns 1. Called from BinkDoFrame at 0x0037758A. The lifted decoder runs
 * when FFmpeg has no frame for this movie, and for difftest captures. */
extern void sub_0037CCD0_gen(void);
static void bink_video_decode(void)
{
    static long calls;
    uint32_t map = MEM32(g_esp + 20);
    uint32_t hbink = map - BINK_DIRTY_MAP;

    if (s_skip_hbink && hbink == s_skip_hbink) {
        int down = (s_skip_frames++ % 6) < 3;
        usb_gamepad_set_held(0, down ? PAD_START : 0, down ? PAD_A : 0);
    }

    if (!mc3_difftest_wanted() && mc3_bink_native_enabled()) {
        int w = (int)MEM32(hbink + BINK_WIDTH), h = (int)MEM32(hbink + BINK_HEIGHT);
        int frame = (int)MEM32(hbink + BINK_FRAMENUM) - 1;
        mc3_bink_native_audio(hbink, frame);
        if (mc3_bink_native_frame(hbink, frame, (uint8_t *)XBOX_PTR(MEM32(g_esp + 4)), w, h)) {
            /* Every block changed, so BinkCopyToBuffer copies the whole frame:
             * it counts the map's leading non-zero bytes against the block
             * total. Same size as BinkDoFrame's own clear (0x0037748F): the
             * 8th and 9th arguments (padded width, height) over 16. */
            uint32_t blocks = (MEM32(g_esp + 32) >> 4) * (MEM32(g_esp + 36) >> 4);
            memset((void *)XBOX_PTR(map), 1, blocks);
            g_eax = 1;
            g_esp += 4 + 0x38;
            return;
        }
    }
    mc3_difftest_call(0x0037CCD0u, sub_0037CCD0_gen, &calls);
}

void sub_0037CCD0(void)
{
    bt_time(BT_VIDEO, bink_video_decode);
}

/* Bink inverse DCT for one 8x8 block, 0x0037F610: (dest, pitch, coeffs) on
 * the stack. In frame 600 its call #59 writes the one wrong block the
 * video difftest found (dest 0x80F3D400, coefficients at 0x00F7FADC). */
extern void sub_0037F610_gen(void);
void sub_0037F610(void)
{
    static long calls;
    mc3_difftest_call(0x0037F610u, sub_0037F610_gen, &calls);
}

extern void sub_00378B30_gen(void);
/* Bink audio decode, 0x00378B30: stdcall (state, &out_ptr, &out_bytes,
 * packet, &packet_pos), called from BinkDoFrame at 0x0037730D, which then
 * copies *out_bytes of PCM from *out_ptr into its DirectSound ring. While
 * FFmpeg plays the movie's sound (src/bink_native.c), that PCM is zeroed:
 * Bink's streaming and clock carry on, on silence. */
static void bink_audio_decode(void)
{
    static long calls;
    uint32_t out_ptr_va = MEM32(g_esp + 8), out_bytes_va = MEM32(g_esp + 12);

    mc3_difftest_call(0x00378B30u, sub_00378B30_gen, &calls);
    if (mc3_bink_native_audio_active() && !mc3_difftest_wanted()) {
        uint32_t out = MEM32(out_ptr_va), bytes = MEM32(out_bytes_va);
        if (out && bytes && bytes <= 0x100000u)
            memset((void *)XBOX_PTR(out), 0, bytes);
    }
}

void sub_00378B30(void)
{
    bt_time(BT_AUDIO, bink_audio_decode);
}

/* ── Game timing: the clocks MC3 reads (MC3_TIME_TRACE=1) ──
 *
 * 2026-10-17: gameplay ran ~10x fast at a steady 60 fps while music and
 * movies kept time, and every host clock checked out at the source. These
 * wrap the title's own timer entry points and print once a second, per
 * clock, how often it was read and how far it moved (in its own units):
 *   sub_00188170  ms since boot: rdtsc / 733333        (1000 per second)
 *   sub_002F64FB  QueryPerformanceCounter (rdtsc)      (733.33 M per second)
 *   sub_002F650C  QueryPerformanceFrequency            (733333333)
 *   sub_001EF3D0  rdtsc, low 32 bits                   (733.33 M per second) */
static int time_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_TIME_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}

static struct { const char *name; long calls; uint64_t first, last; } s_clk[4] = {
    { "ms clock (0x188170)" }, { "QPC (0x2F64FB)" }, { "QPF (0x2F650C)" }, { "rdtsc lo (0x1EF3D0)" },
};

static void qpc_callers_print(void);

static void time_note(int k, uint64_t v)
{
    static long long next;
    struct timespec ts;
    long long now;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (!s_clk[k].calls++) s_clk[k].first = v;
    s_clk[k].last = v;
    if (!next) next = now + 1000;
    if (now < next) return;
    next = now + 1000;
    for (int i = 0; i < 4; i++) {
        if (!s_clk[i].calls) continue;
        fprintf(stderr, "[TIME] %-20s %6ld reads/s, moved %llu (last %llu)\n", s_clk[i].name,
                s_clk[i].calls, (unsigned long long)(s_clk[i].last - s_clk[i].first),
                (unsigned long long)s_clk[i].last);
        s_clk[i].calls = 0;
    }
    qpc_callers_print();
}

extern void sub_00188170_gen(void);
void sub_00188170(void)
{
    sub_00188170_gen();
    if (time_trace_on()) time_note(0, ((uint64_t)g_edx << 32) | g_eax);
}
/* QPC's callers: the return addresses of the last second, top 4 printed
 * with the [TIME] lines (2026-10-17: 600k reads/s during the intro
 * flythrough, none in the garage -- who polls it). */
typedef struct { uint32_t ret; long n; } CallerCount;
static CallerCount s_qpc_by[16], s_bink_by[16];

static void caller_note(CallerCount *t, uint32_t ret)
{
    int i, low = 0;
    for (i = 0; i < 16; i++) {
        if (t[i].ret == ret) { t[i].n++; return; }
        if (t[i].n < t[low].n) low = i;
    }
    t[low].ret = ret;
    t[low].n = 1;
}
static void qpc_caller(uint32_t ret) { caller_note(s_qpc_by, ret); }

static void callers_print(CallerCount *t, const char *what)
{
    for (int k = 0; k < 4; k++) {
        int best = -1;
        for (int i = 0; i < 16; i++)
            if (t[i].n > 0 && (best < 0 || t[i].n > t[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, "[TIME]   %s called from %08X: %ld\n", what, t[best].ret, t[best].n);
        t[best].n = 0;                              /* printed */
    }
    memset(t, 0, 16 * sizeof *t);
}

static void qpc_callers_print(void)
{
    callers_print(s_qpc_by, "QPC");
    callers_print(s_bink_by, "Bink timer");
}

/* Bink's millisecond timer (RADTimerRead): its callers, for the loop that
 * reads it ~500k times a second during the intro flythrough. */
extern void sub_00378160_gen(void);
void sub_00378160(void)
{
    if (time_trace_on()) caller_note(s_bink_by, MEM32(g_esp));
    sub_00378160_gen();
}

extern void sub_002F64FB_gen(void);
void sub_002F64FB(void)
{
    uint32_t out = MEM32(g_esp + 4), ret = MEM32(g_esp);
    sub_002F64FB_gen();
    if (time_trace_on()) {
        qpc_caller(ret);
        time_note(1, ((uint64_t)MEM32(out + 4) << 32) | MEM32(out));
    }
}
extern void sub_002F650C_gen(void);
void sub_002F650C(void)
{
    uint32_t out = MEM32(g_esp + 4);
    sub_002F650C_gen();
    if (time_trace_on()) time_note(2, ((uint64_t)MEM32(out + 4) << 32) | MEM32(out));
}
extern void sub_001EF3D0_gen(void);
void sub_001EF3D0(void)
{
    sub_001EF3D0_gen();
    if (time_trace_on()) time_note(3, g_eax);
}

/* ── D3D's vblank handling, counted (MC3_PERF reports it) ──
 *
 * sub_003E4FD0 is D3D's vblank work: it advances the device's vblank count
 * (+0x1C0) and retires flips. Its callers run it when PMC_INTR_0's display
 * bit (0x01000000) is set -- on any interrupt, so a pushbuffer trap raised
 * while that bit is still up is counted as one more vblank. Should be 60 a
 * second; the report prints the real rate (2026-10-17: the intro flythrough
 * runs fast on Linux and reads no clock, so its time base is suspect). */
volatile long g_mc3_d3d_vblanks;
volatile uint32_t g_mc3_d3d_device;       /* ecx of the vblank handler: D3D's device */
extern void sub_003E4FD0_gen(void);
void sub_003E4FD0(void)
{
    g_mc3_d3d_vblanks++;
    g_mc3_d3d_device = g_ecx;
    sub_003E4FD0_gen();
}

/* ── The main loop's passes, counted (MC3_PERF reports them) ──
 *
 * sub_0001F2C1 is the game's main loop; it calls sub_002A58D0 once per
 * pass, before the update. On the console a pass is paced by the display
 * (60 a second); the intro flythrough runs ~10x fast on Linux while the
 * presented frame rate, the frame time and every clock read are right
 * (2026-10-18) -- so: passes per second against frames presented. */
volatile long g_mc3_loop_passes;
extern void sub_002A58D0_gen(void);
void sub_002A58D0(void)
{
    g_mc3_loop_passes++;
    sub_002A58D0_gen();
}

/* ── D3D's PGRAPH handler: the trap protocol with an acknowledgement ──
 *
 * sub_003E5540 is D3D's PGRAPH interrupt handler: it reads PGRAPH_INTR and
 * NSOURCE (0xFD400100/0108), and for a software method (NOP) hands the
 * trapped parameter to the pushbuffer callback sub_003E52D0. It acknowledges
 * by writing the status back -- write-one-to-clear on the chip, a no-op on
 * the plain memory here -- so the trap stayed visible, and D3D's interrupt
 * routine ran the handler again and again on it before the executor saw it
 * taken: 412,082 flips queued in one run (2026-10-18), D3D's flip targets
 * millions of vblanks ahead and its frame bookkeeping far ahead of the
 * screen. The toolkit's documented cure for that (NFSU2 hit the same): wrap
 * the handler under the executor's trap lock and report a trap it found as
 * taken, which clears what the hardware would (nv2a_pb_exec.c). */
extern void nv2a_pb_trap_lock(void);
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);
extern void sub_003E5540_gen(void);
void sub_003E5540(void)
{
    static int on = -1;
    uint32_t nsource;
    if (on < 0) {                      /* MC3_TRAP_HOOK=0: the old protocol, to compare */
        const char *e = getenv("MC3_TRAP_HOOK");
        on = !(e && e[0] == '0');
    }
    if (!on) {
        sub_003E5540_gen();
        return;
    }
    nv2a_pb_trap_lock();
    nsource = MEM32(0xFD400108u);
    sub_003E5540_gen();
    if (nsource)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
}

/* ── Frame pacing: D3D's flip retirement (vblank) ──
 *
 * D3D queues each flip with a target vblank: previous target + the
 * presentation interval the title asked for (the low bits of the callback's
 * parameter; MC3's menus use 2, i.e. 30 fps). sub_003E4EF0, called from the
 * vblank handler, retires the flips whose target has come and returns how
 * many. The executor's FLIP_STALL waits for those retirements once the
 * project reports them (nv2a_pb_flip_retired); unreported, it waited one
 * vblank whatever the interval, and the 30 fps menus, flythrough and garage
 * played at 60 -- the game advances them per frame. A first attempt
 * (2026-10-17) stuck at 4 fps: the trap storm above had wrecked D3D's flip
 * queue (targets millions of vblanks ahead). With the trap acknowledged,
 * the queue is sane and retirement can pace the flips. MC3_FLIP_PACE=0
 * falls back to one flip per vblank. */
extern void nv2a_pb_flip_retired(unsigned n);
extern void sub_003E4EF0_gen(void);
void sub_003E4EF0(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_FLIP_PACE");
        on = !(e && e[0] == '0');
    }
    sub_003E4EF0_gen();
    if (on)
        nv2a_pb_flip_retired(g_eax);
}

/* ── D3D's pushbuffer callback, logged (MC3_FLIP_TRACE=1) ──
 *
 * sub_003E52D0(param) runs when the GPU reaches one of D3D's software
 * methods (the executor's NOP trap). param & 0x1F is the command; for a
 * flip, (param >> 5) & 7 is the presentation interval and the frame's
 * target vblank becomes max(previous target +0x1C8 + interval, vblank
 * count +0x1C0). It showed the trap storm (targets millions of vblanks
 * ahead) and the menus' interval 2 (2026-10-18). First 40 calls, then
 * every 120th. */
extern void sub_003E52D0_gen(void);
void sub_003E52D0(void)
{
    static int on = -1;
    static long n;
    uint32_t dev = g_ecx, param = MEM32(g_esp + 4), t_before = 0, vb = 0;
    if (on < 0) {
        const char *e = getenv("MC3_FLIP_TRACE");
        on = e && e[0] == '1';
    }
    if (on && dev) { t_before = MEM32(dev + 0x1C8); vb = MEM32(dev + 0x1C0); }
    sub_003E52D0_gen();
    if (on && dev && (++n <= 40 || n % 120 == 0))
        fprintf(stderr, "[FLIP] #%ld param %08X cmd %u data %X | target %u -> %u, next %u, vblank %u (dev %08X)\n",
                n, param, param & 0x1F, param >> 5, t_before, MEM32(dev + 0x1C8),
                MEM32(dev + 0x1C4), vb, dev);
}

/* ── The loading screen starts: compile the remembered shaders now ──
 *
 * sub_0001B740 starts the loading-screen thread (0x0001B720, priority 14):
 * first behind the legal screen, while the game loads for seconds. Without
 * program binaries (the Switch's Mesa 20.1) the renderer compiled its whole
 * program list before the first logo: 4.8 s of black (2026-10-18). Now it
 * waits for this and compiles a program a frame behind a static screen
 * (nv2a_gl_prewarm_start; RECOMP_PROG_PREWARM=boot for the old way). */
extern void nv2a_gl_prewarm_start(void);
extern void sub_0001B740_gen(void);
void sub_0001B740(void)
{
    nv2a_gl_prewarm_start();
    sub_0001B740_gen();
}

/* ── The loading screen keeps its 4:3 shape ──
 *
 * sub_0001B500 is the loading-screen thread's loop (thread 0x0001B720,
 * started only from the boot sequence at 0x00020387): the legal screen.
 * With "legal" in MC3_FILL_SKIP it is shown at its own 4:3 shape, the fill
 * held while the loop runs. Not the default: the loop also runs behind
 * other loading screens and ends before some of them do, so they started
 * at 4:3 and switched to 16:9 halfway (2026-10-20). */
extern void nv2a_gl_fill_hold(int delta) __attribute__((weak));
extern void sub_0001B500_gen(void);
static RECOMP_TLS int t_ls_thread;          /* this is the loading-screen thread */
void sub_0001B500(void)
{
    const char *e = getenv("MC3_FILL_SKIP");
    int hold = nv2a_gl_fill_hold && e && strstr(e, "legal");   /* opt-in: see above */
    if (hold)
        nv2a_gl_fill_hold(1);
    t_ls_thread = 1;
    sub_0001B500_gen();
    t_ls_thread = 0;
    if (hold)
        nv2a_gl_fill_hold(-1);
}

/* ── The loading screen's loop, timed (MC3_LS_TRACE=1) ──
 *
 * Each turn of sub_0001B500: the Flash movie advances a fixed 1/30 s
 * (sub_00122DB0, 0x3D08882F), is drawn and presented (sub_002A3270), then
 * Sleep(24) (sub_0001739C). Logs every call on the loading thread with
 * its time, and presents from any other thread. It showed the loop turning
 * evenly every 33 ms while the circles stuttered (2026-10-24): the cause
 * was the renderer, which ignored the 2D blit that carries each frame into
 * the next back buffer (xboxrecomp, nv2a_pb_exec.c). */
static int ls_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_LS_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}
static double ls_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
const char *xbox_gil_my_label(void);
static void ls_log(const char *what, double t0)
{
    double t1 = ls_now();
    const char *l = xbox_gil_my_label();
    fprintf(stderr, "[LS] @%.3f %s %s %.1f ms\n", t0 / 1e3, t_ls_thread ? "ls" : l ? l : "?",
            what, t1 - t0);
}
static void ls_call(void (*fn)(void), const char *what, int any_thread)
{
    double t0;
    if (!ls_trace_on() || !(t_ls_thread || any_thread)) {
        fn();
        return;
    }
    t0 = ls_now();
    fn();
    ls_log(what, t0);
}
extern void sub_00122DB0_gen(void);
void sub_00122DB0(void) { ls_call(sub_00122DB0_gen, "advance", 0); }
extern void sub_002A3270_gen(void);
void sub_002A3270(void) { ls_call(sub_002A3270_gen, "present", 1); }
/* ── The heartbeat thread sleeps instead of spinning (MC3_HEARTBEAT_MS) ──
 *
 * sub_00196230 (its own thread) is a one-second watchdog: enter a critical
 * section, compare the CPU clock with the last beat (ticks / 733 MHz >=
 * 1.0 s: sub_00044B90), leave, Sleep(0) at 0x001962AC, again. On the
 * Switch each turn took the guest lock from the main thread: 4,200 turns
 * a second, the main thread waiting behind it 3,400 times a second at
 * ~50 us a hand-over -- most of its 30% waiting in the menus (19.5 fps,
 * 2026-10-21 log). A beat checked every few milliseconds is as good.
 * This turns that one Sleep(0) into Sleep(MC3_HEARTBEAT_MS), default 8;
 * 0 restores the game's own. */
static uint32_t heartbeat_ms(void)
{
    static int ms = -1;
    if (ms < 0) {
        const char *e = getenv("MC3_HEARTBEAT_MS");
        ms = e ? atoi(e) : 8;
        if (ms < 0) ms = 0;
    }
    return (uint32_t)ms;
}

extern void sub_0001739C_gen(void);
void sub_0001739C(void)
{
    if (MEM32(g_esp) == 0x001962B3u && MEM32(g_esp + 4) == 0) {   /* the heartbeat's Sleep(0) */
        static int said;
        MEM32(g_esp + 4) = heartbeat_ms();
        if (!said++) {
            const char *l = xbox_gil_my_label();
            fprintf(stderr, "[MC3] heartbeat thread (%s): Sleep(0) -> Sleep(%u) (MC3_HEARTBEAT_MS)\n",
                    l ? l : "?", heartbeat_ms());
        }
    }
    ls_call(sub_0001739C_gen, "sleep", 0);
}

/* The way out: once the game sets the stop flag (+0x2004 of the loop's
 * object), each turn sends the Flash movie to "StartOfTransitionOut"
 * (sub_001221F0, this = movie, label, 1) and leaves when sub_00122360
 * says it has reached "EndOfTransitionOut" (returns 1). Logged with their
 * arguments and results on the loading thread. A load that ends while the
 * intro still plays (fast storage: 0.5 s) gets a transition-out with its
 * shapes still off stage, so the screen cuts away with the spinner up; at
 * a DVD's pace (RECOMP_IO_MBPS=8 RECOMP_IO_SEEK_MS=80) it plays out. */
static void ls_label(void (*fn)(void), const char *what)
{
    uint32_t movie = g_ecx, label = MEM32(g_esp + 4), arg2 = MEM32(g_esp + 8);
    char name[40];
    uint32_t i;
    fn();
    if (!ls_trace_on() || !t_ls_thread)
        return;
    for (i = 0; i + 1 < sizeof name && label && MEM8(label + i); i++)
        name[i] = (char)MEM8(label + i);
    name[i] = 0;
    fprintf(stderr, "[LS] @%.3f ls %s(movie %08X, \"%s\", %u) -> %u\n", ls_now() / 1e3,
            what, movie, name, arg2, g_eax);
}
extern void sub_001221F0_gen(void);
void sub_001221F0(void) { ls_label(sub_001221F0_gen, "goto"); }
extern void sub_00122360_gen(void);
void sub_00122360(void) { ls_label(sub_00122360_gen, "reached"); }

/* ── Save-game deletion, traced (MC3_SAVE_TRACE=1) ──
 *
 * Deleting a profile says "deleted" but leaves it on disk, and no file call
 * reaches the kernel (2026-10-18). sub_0002A5D0(name) is the game's delete:
 * it calls XDeleteSaveGame (sub_002F4E4E) with "<letter>:\\" only when its
 * device's drive letter ([this+8]) is set. This logs which way it goes and
 * what XDeleteSaveGame returned. */
static int save_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_SAVE_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}

static void save_trace_name(char *out, size_t cap, uint32_t va)
{
    size_t i = 0;
    for (; va && i + 1 < cap; i++) {        /* wide string, ASCII shown */
        uint16_t c = MEM16(va + 2 * i);
        if (!c) break;
        out[i] = c < 0x80 ? (char)c : '?';
    }
    out[i] = 0;
}

extern void sub_0002A5D0_gen(void);
void sub_0002A5D0(void)
{
    uint32_t self = g_ecx, name = MEM32(g_esp + 4), ret = MEM32(g_esp);
    char n[64];
    if (save_trace_on()) {
        save_trace_name(n, sizeof n, name);
        fprintf(stderr, "[SAVE] delete \"%s\" device %08X letter '%c' (0x%02X), from %08X\n",
                n, self, MEM8(self + 8) >= 0x20 ? MEM8(self + 8) : '?', MEM8(self + 8), ret);
    }
    sub_0002A5D0_gen();
    if (save_trace_on())
        fprintf(stderr, "[SAVE] delete -> %u\n", g_eax);
}

extern void sub_002F4E4E_gen(void);
void sub_002F4E4E(void)
{
    uint32_t root = MEM32(g_esp + 4), name = MEM32(g_esp + 8);
    char n[64], r[8] = {0};
    int i;
    if (save_trace_on()) {
        save_trace_name(n, sizeof n, name);
        for (i = 0; i < 7 && root && MEM8(root + i); i++)
            r[i] = (char)MEM8(root + i);
        fprintf(stderr, "[SAVE] XDeleteSaveGame(\"%s\", \"%s\")\n", r, n);
    }
    sub_002F4E4E_gen();
    if (save_trace_on())
        fprintf(stderr, "[SAVE] XDeleteSaveGame -> %u\n", g_eax);
}

/* ── "Is pad n a steering wheel?", asked once per controller ──
 *
 * sub_0007D7A0 (thiscall, the game's pad object) returns
 * XInputGetCapabilities(handle).SubType == 0x10 (XINPUT_DEVSUBTYPE_GC_WHEEL),
 * and the game asks every frame. XInputGetCapabilities is a synchronous USB
 * control transfer (XPP 0x0040BB0F, waiting at 0x0040F0BB with a 50 ms
 * timeout): ~1-2 ms on the Xbox, ~19 ms on the emulated OHCI on the Switch,
 * where the transfer waits for the controller thread's 4 ms pass and the
 * completion interrupt for the guest lock. Two per frame took 450 ms of
 * every second of the main thread in a race (10-12 fps, 2026-10-20). The
 * answer cannot change while the same handle is open, so it is kept per
 * handle ([pad+0x34]; [pad+0x1C] == 4 is "open"). MC3_PAD_CAPS_CACHE=0
 * asks every time, as the game does. */
extern void sub_0007D7A0_gen(void);
void sub_0007D7A0(void)
{
    static uint32_t handle_of[8], answer_of[8];
    static unsigned next;
    static int on = -1;
    uint32_t pad = g_ecx, handle = MEM32(pad + 0x34);
    int open = MEM32(pad + 0x1C) == 4 && handle;
    unsigned i;
    if (on < 0) {
        const char *e = getenv("MC3_PAD_CAPS_CACHE");
        on = !(e && e[0] == '0');
    }
    if (on && open)
        for (i = 0; i < 8; i++)
            if (handle_of[i] == handle) {
                g_eax = answer_of[i];
                g_esp += 4;                      /* ret: thiscall, no arguments */
                return;
            }
    sub_0007D7A0_gen();
    if (on && open) {
        handle_of[next % 8] = handle;
        answer_of[next % 8] = g_eax & 0xFFu;
        next++;
    }
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── Career race results: listeners checked before they are called ──
 *
 * mcPgCareer::ReportRaceResult() (sub_0024F8C0, named by its own messages)
 * hands the result to sub_00251CD0, which calls every listener in a list
 * of 12-byte entries {u16 id, u32 key, object*} at this+0x78..+0x7C
 * (vtable slot 0x28; the entry at index this+0x94 first, -1 = none);
 * sub_00251D40 does the same through slot 0x2C. Starting one career race
 * on the Switch, and after ~8 minutes on the Mac, the list held a stale
 * entry: first a null method (the runtime skipped the call), then an
 * object at garbage (0xCDCDCD08 on the Mac, 0x535F4D7C on the Switch), and
 * the game crashed (2026-10-21; same call chain on both). Which object
 * leaves its entry behind is not known yet. Until it is, entries whose
 * object, vtable or method do not point where they must (object in RAM,
 * vtable in .rdata, method in code) are taken out of the list before the
 * call and logged with what they held. MC3_LISTENER_CHECK=0 turns it off. */
static int listener_ram(uint32_t va)
{
    return !(va & 3) && ((va >= 0x00010000u && va < 0x04000000u)
                         || (va >= 0x80000000u && va < 0x84000000u));
}

static int listener_valid(uint32_t obj, unsigned slot)
{
    uint32_t vt, fn;
    if (!listener_ram(obj))
        return 0;
    vt = MEM32(obj);
    if (vt < 0x00411440u || vt >= 0x00471A80u || (vt & 3))   /* .rdata */
        return 0;
    fn = MEM32(vt + slot);
    return fn >= 0x00012000u && fn < 0x00411000u;               /* code */
}

static void listener_prune(uint32_t self, unsigned slot, const char *who)
{
    static int on = -1, logged;
    uint32_t begin, end, rd, wr, idx, new_idx = 0xFFFFFFFFu, i;
    if (on < 0) {
        const char *e = getenv("MC3_LISTENER_CHECK");
        on = !(e && e[0] == '0');
    }
    if (!on || !listener_ram(self))
        return;
    begin = MEM32(self + 0x78);
    end = MEM32(self + 0x7C);
    idx = MEM32(self + 0x94);
    if (!listener_ram(begin) || end < begin || end - begin > 12u * 4096 || (end - begin) % 12) {
        if (logged++ < 20)
            fprintf(stderr, "[LISTEN] %s: list of %08X is not a list (begin %08X end %08X): left alone\n",
                    who, self, begin, end);
        return;
    }
    for (rd = wr = begin, i = 0; rd < end; rd += 12, i++) {
        uint32_t obj = MEM32(rd + 8);
        if (!listener_valid(obj, slot)) {
            if (logged++ < 20)
                fprintf(stderr, "[LISTEN] %s: entry %u of %08X dropped: id %04X key %08X object %08X "
                                "vtable %08X%s\n", who, i, self, MEM16(rd), MEM32(rd + 4), obj,
                        listener_ram(obj) ? MEM32(obj) : 0, i == idx ? " (the current one)" : "");
            continue;
        }
        if (i == idx)
            new_idx = (wr - begin) / 12;
        if (wr != rd) {
            MEM32(wr) = MEM32(rd);
            MEM32(wr + 4) = MEM32(rd + 4);
            MEM32(wr + 8) = MEM32(rd + 8);
        }
        wr += 12;
    }
    if (wr != end) {
        MEM32(self + 0x7C) = wr;
        if (idx != 0xFFFFFFFFu)
            MEM32(self + 0x94) = new_idx;
    }
}

/* The broadcast itself, done safely. The crash came back with every entry
 * valid before the call (2026-10-21, no [LISTEN] line; the loop pointer
 * esi inside the list, a garbage object read): a listener's callback
 * changes the list while the original loop walks it, so the loop reads an
 * entry that was moved or freed. On the Xbox freed memory keeps its old
 * contents and the game gets away with it. So: the current entry first
 * (this+0x94), as the original, then every other entry from a copy taken
 * before the calls, each called only if it is still in the list and valid
 * at that moment. thiscall, one argument, callee pops it (ret 4). */
static void listener_call(uint32_t obj, unsigned slot, uint32_t arg)
{
    uint32_t fn_va = MEM32(MEM32(obj) + slot);
    recomp_func_t fn = recomp_lookup(fn_va);
    if (!fn)
        fn = recomp_lookup_manual(fn_va);
    if (!fn) {
        fprintf(stderr, "[LISTEN] method %08X of %08X not in dispatch: skipped\n", fn_va, obj);
        return;
    }
    {
        static int trace = -1;
        if (trace < 0) {
            const char *e = getenv("MC3_LISTEN_TRACE");
            trace = e && e[0] == '1';
        }
        if (trace)
            fprintf(stderr, "[LISTEN]   -> object %08X vtable %08X method %08X (arg %u)\n",
                    obj, MEM32(obj), fn_va, arg);
    }
    {
        /* The callee must pop its return address and argument (ret 4) and
         * keep ebx, esi, edi, ebp, as MSVC's thiscall does. A translated
         * method that does not is a lifter bug; the original loop kept its
         * position in esi and crashed on one. Say which method, and put the
         * stack back where the call left it. */
        uint32_t esp0 = g_esp, bx = g_ebx, si = g_esi, di = g_edi, bp = g_ebp;
        static int logged;
        g_esp -= 4; MEM32(g_esp) = arg;
        g_esp -= 4; MEM32(g_esp) = 0;          /* return address */
        g_ecx = obj;
        fn();                                  /* ret 4: pops both */
        if (g_esp != esp0 || g_ebx != bx || g_esi != si || g_edi != di || g_ebp != bp) {
            if (logged++ < 20)
                fprintf(stderr, "[LISTEN] method %08X of object %08X (vtable %08X +%02X) broke the "
                                "calling convention:%s%s%s%s%s esp %+d\n", fn_va, obj, MEM32(obj), slot,
                        g_ebx != bx ? " ebx" : "", g_esi != si ? " esi" : "", g_edi != di ? " edi" : "",
                        g_ebp != bp ? " ebp" : "", "", (int)(g_esp - esp0));
            g_esp = esp0;
        }
    }
}

static int listener_live(uint32_t self, uint32_t obj, uint16_t id, uint32_t key)
{
    uint32_t e, begin = MEM32(self + 0x78), end = MEM32(self + 0x7C);
    if (!listener_ram(begin) || end < begin || end - begin > 12u * 4096)
        return 0;
    for (e = begin; e + 12 <= end; e += 12)
        if (MEM32(e + 8) == obj && MEM16(e) == id && MEM32(e + 4) == key)
            return 1;
    return 0;
}

#define LISTEN_MAX 512
static void listener_broadcast(uint32_t self, unsigned slot, const char *who)
{
    static int changed_logged;
    uint32_t begin, end, idx, n, i, arg = MEM32(g_esp + 4);
    uint32_t cur_key = 0, saved_ebx = g_ebx, saved_esi = g_esi, saved_edi = g_edi, saved_ebp = g_ebp;
    uint16_t cur_id = 0;
    struct { uint16_t id; uint32_t key, obj; } snap[LISTEN_MAX];

    listener_prune(self, slot, who);           /* drop what is invalid already */
    begin = MEM32(self + 0x78);
    end = MEM32(self + 0x7C);
    idx = MEM32(self + 0x94);
    n = (listener_ram(begin) && end >= begin) ? (end - begin) / 12 : 0;
    if (n > LISTEN_MAX)
        n = LISTEN_MAX;
    for (i = 0; i < n; i++) {
        uint32_t e = begin + 12 * i;
        snap[i].id = MEM16(e); snap[i].key = MEM32(e + 4); snap[i].obj = MEM32(e + 8);
    }
    if (idx != 0xFFFFFFFFu) {
        if (idx < n) {
            cur_id = snap[idx].id; cur_key = snap[idx].key;
            if (listener_valid(snap[idx].obj, slot))
                listener_call(snap[idx].obj, slot, arg);
        } else if (changed_logged++ < 20) {
            fprintf(stderr, "[LISTEN] %s: current entry %u of %08X is past the list (%u entries): skipped\n",
                    who, idx, self, n);
        }
    }
    for (i = 0; i < n; i++) {
        if (snap[i].id == cur_id && snap[i].key == cur_key)
            continue;                          /* the current one: done above */
        if (!listener_live(self, snap[i].obj, snap[i].id, snap[i].key)) {
            if (changed_logged++ < 20)
                fprintf(stderr, "[LISTEN] %s: entry %u of %08X (object %08X) left the list during "
                                "the broadcast: not called\n", who, i, self, snap[i].obj);
            continue;
        }
        if (listener_valid(snap[i].obj, slot))
            listener_call(snap[i].obj, slot, arg);
    }
    /* Listeners added during the broadcast hear it too, as the original
     * loop reached them (it runs to the list's live end): starting a race
     * registers the race's own listeners from a callback, and without this
     * the race stopped at "PREPARE TO RACE" (2026-10-21). Rounds until no
     * new entry appears, each called once. */
    {
        int round;
        for (round = 0; round < 8 && n < LISTEN_MAX; round++) {
            uint32_t e, b = MEM32(self + 0x78), en = MEM32(self + 0x7C), added = 0;
            if (!listener_ram(b) || en < b || en - b > 12u * 4096)
                break;
            for (e = b; e + 12 <= en && n < LISTEN_MAX; e += 12) {
                uint16_t id = MEM16(e);
                uint32_t key = MEM32(e + 4), obj = MEM32(e + 8), k;
                int seen = id == cur_id && key == cur_key;
                for (k = 0; k < n && !seen; k++)
                    seen = snap[k].obj == obj && snap[k].id == id && snap[k].key == key;
                if (seen)
                    continue;
                snap[n].id = id; snap[n].key = key; snap[n].obj = obj;
                n++;
                added++;
                if (listener_valid(obj, slot))
                    listener_call(obj, slot, arg);
            }
            if (!added)
                break;
        }
    }
    {
        static int trace = -1;
        static long count;
        if (trace < 0) {
            const char *e = getenv("MC3_LISTEN_TRACE");
            trace = e && e[0] == '1';
        }
        if (trace && count++ < 200)
            fprintf(stderr, "[LISTEN] %s %08X(arg %u): %u listener(s), current %d, list now %u\n",
                    who, self, arg, n, (int)idx,
                    (MEM32(self + 0x7C) - MEM32(self + 0x78)) / 12);
    }
    g_ebx = saved_ebx; g_esi = saved_esi; g_edi = saved_edi; g_ebp = saved_ebp;
    g_esp += 8;                                /* ret 4: return address and the argument */
}

static int listener_fix_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_LISTENER_CHECK");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* Written out, not by macro: tools/recomp/manual_scan.py reads the text. */
extern void sub_00251CD0_gen(void);
void sub_00251CD0(void)
{
    if (!listener_fix_on()) { sub_00251CD0_gen(); return; }
    listener_broadcast(g_ecx, 0x28, "race result");
}

extern void sub_00251D40_gen(void);
void sub_00251D40(void)
{
    if (!listener_fix_on()) { sub_00251D40_gen(); return; }
    listener_broadcast(g_ecx, 0x2C, "race event");
}

/* ── The game's state machine, traced (MC3_STATE_TRACE=1) ──
 *
 * The object behind the main loop's dispatch (DUB 0x0051886C, Remix
 * 0x00557ED0) keeps the state at +4 (1..7, the main loop's switch); its
 * vtable +4 (sub_00020AE0) requests a screen, +8 (sub_00020B60) runs a pass and
 * applies requests. Logs every request with its caller and every change of
 * state -- the DUB Edition goes to state 4 for the boot movies, the Remix
 * stayed in 1 (2026-10-24). */
static int state_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_STATE_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}
extern void sub_00020AE0_gen(void);
void sub_00020AE0(void)
{
    if (state_trace_on())
        fprintf(stderr, "[STATE] request %u (state %u) from %08X\n", MEM32(g_esp + 4),
                MEM32(g_ecx + 4), MEM32(g_esp));
    sub_00020AE0_gen();
}
extern void sub_00020B60_gen(void);
void sub_00020B60(void)
{
    uint32_t obj = g_ecx, before = MEM32(obj + 4);
    sub_00020B60_gen();
    if (state_trace_on() && MEM32(obj + 4) != before)
        fprintf(stderr, "[STATE] %u -> %u\n", before, MEM32(obj + 4));
}

/* ── DirectSound's DSP command wait: its doorbell found at run time ──
 *
 * sub_0038DAA3 (DSOUND) hands the audio DSP a command block and waits for the
 * DSP to clear the command word at block + 0x810. No DSP is emulated: the
 * runtime answers words registered with it (apu_dsp_ack_add). The block is
 * [[[this+8]+0x10]], the object's command buffer, and moves with the
 * title's allocations -- the Remix's sat at 0x83B10000 when told the TV
 * is widescreen and at 0x83A5C000 when not (2026-10-25), so a fixed
 * address hung one of the two and zeroed a stray word in the other. */
extern void apu_dsp_ack_add(uint32_t va);
extern void sub_0038DAA3_gen(void);
void sub_0038DAA3(void)
{
    uint32_t dev = MEM32(g_ecx + 8);
    uint32_t buf = dev ? MEM32(dev + 0x10) : 0;
    uint32_t block = buf ? MEM32(buf) : 0;
    if (block)
        apu_dsp_ack_add(block + 0x810);
    sub_0038DAA3_gen();
}
