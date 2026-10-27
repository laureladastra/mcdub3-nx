/*
 * switch_nx.c -- Nintendo Switch (Horizon / libnx) boot glue.
 *
 *   log       nxlink to the PC when launched with `nxlink -s`, otherwise
 *             sdmc:/switch/<MC3_SD_DIR>/<MC3_APP>_log.txt (the run before
 *             it: <MC3_APP>_log_prev.txt)
 *   settings  sdmc:/switch/<MC3_SD_DIR>/<MC3_APP>_env.txt, KEY=VALUE per line,
 *             '#' starts a comment; applied with setenv before boot, so every
 *             RECOMP_* / MC3_* switch works on the console without a rebuild
 *   crashes   CPU exception handler: host PC as an ELF offset (for
 *             aarch64-none-elf-addr2line) plus the guest state
 *   errors    switch_error_dialog(): the system error applet
 *
 * Kept apart from main.c because <switch.h> and the runtime's Win32 vocabulary
 * collide. Only built for the Switch.
 *
 * The log goes through memory: a line-buffered SD file made every line a
 * card write, and the runtime logs thousands at boot (file opens, reads,
 * NV2A) -- MC3 sat on a black screen for ~15 s before its first frame
 * (2026-10-16). Now stdout and stderr write into a "log:" device, a 4 MB
 * ring in RAM, and a writer thread empties it onto the card in blocks every
 * 200 ms (sooner when half full). A crash or exit drains it synchronously.
 * (The approach nfsu2-sw measured; this is our own implementation.)
 */
#include <switch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdalign.h>
#include <sys/iosupport.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MC3_APP
#  define MC3_APP "mcdub3"
#endif
#ifndef MC3_SD_DIR
#  define MC3_SD_DIR "mcdub3-nx"
#endif
#define APP_DIR "sdmc:/switch/" MC3_SD_DIR

void print_guest_state(void);   /* main.c */
extern char __start__[];        /* libnx: the ELF's load address */

static int s_nxlink_fd = -1;
static int s_sockets;

/* ------------------------------------------------------------------ */
/* Log                                                                 */
/* ------------------------------------------------------------------ */

/* The RAM ring: written by any thread (memcpy under a mutex), emptied by
 * the writer thread. head - tail bytes are waiting; both only grow. */
#define LOG_RING (4u << 20)
static char       s_ring[LOG_RING];
static u64        s_head, s_tail;
static Mutex      s_ring_lock;          /* head, and the ring contents */
static Mutex      s_file_lock;          /* the SD file and tail */
static CondVar    s_ring_cv;
static int        s_log_fd = -1;
static Thread     s_writer;
static volatile int s_writer_on;

/* Write what is waiting to the card. Under s_file_lock only: s_head is read
 * under the ring lock, the copy out happens without it (the writer is the
 * only one advancing s_tail, and producers never pass it). */
static void log_drain(void)
{
    u64 head, tail;
    mutexLock(&s_file_lock);
    mutexLock(&s_ring_lock);
    head = s_head;
    mutexUnlock(&s_ring_lock);
    tail = s_tail;
    while (tail < head) {
        u64 at = tail % LOG_RING, n = head - tail;
        if (n > LOG_RING - at) n = LOG_RING - at;
        if (s_log_fd >= 0 && write(s_log_fd, s_ring + at, (size_t)n) < 0)
            break;
        tail += n;
    }
    mutexLock(&s_ring_lock);
    s_tail = tail;
    condvarWakeAll(&s_ring_cv);         /* room for a producer that waits */
    mutexUnlock(&s_ring_lock);
    mutexUnlock(&s_file_lock);
}

static void log_writer(void *arg)
{
    (void)arg;
    while (s_writer_on) {
        mutexLock(&s_ring_lock);
        if (s_head - s_tail < LOG_RING / 2)
            condvarWaitTimeout(&s_ring_cv, &s_ring_lock, 200000000ull);   /* 200 ms */
        mutexUnlock(&s_ring_lock);
        log_drain();
    }
}

static void ring_put(const char *ptr, size_t len)
{
    size_t done = 0;
    while (done < len) {
        size_t n;
        mutexLock(&s_ring_lock);
        if (s_head - s_tail >= LOG_RING) {    /* full: empty it here */
            mutexUnlock(&s_ring_lock);
            log_drain();
            continue;
        }
        n = LOG_RING - (size_t)(s_head - s_tail);
        if (n > len - done) n = len - done;
        {
            u64 at = s_head % LOG_RING;
            size_t first = n < LOG_RING - at ? n : (size_t)(LOG_RING - at);
            memcpy(s_ring + at, ptr + done, first);
            memcpy(s_ring, ptr + done + first, n - first);
        }
        s_head += n;
        if (s_head - s_tail >= LOG_RING / 2)
            condvarWakeOne(&s_ring_cv);
        mutexUnlock(&s_ring_lock);
        done += n;
    }
}

/* Each line starts with the seconds since start-up, "[  4.512] ", so a log
 * says where boot time goes (MC3's black screen before the first logo,
 * 2026-10-18). MC3_LOG_TIME=0 leaves lines bare. Line starts are tracked
 * across threads; a line two threads interleave gets one stamp. */
static u64          s_t0;
static volatile int s_bol = 1;

static ssize_t log_write_r(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    static int on = -1;
    size_t i = 0;
    (void)r; (void)fd;
    if (on < 0) {
        const char *e = getenv("MC3_LOG_TIME");
        on = !(e && e[0] == '0');
    }
    if (!on) {
        ring_put(ptr, len);
        return (ssize_t)len;
    }
    while (i < len) {
        const char *nl = memchr(ptr + i, '\n', len - i);
        size_t n = nl ? (size_t)(nl - (ptr + i)) + 1 : len - i;
        if (s_bol) {
            char stamp[24];
            u64 ns = armTicksToNs(armGetSystemTick() - s_t0);
            int k = snprintf(stamp, sizeof stamp, "[%3u.%03u] ",
                             (unsigned)(ns / 1000000000ull), (unsigned)(ns / 1000000ull % 1000));
            ring_put(stamp, (size_t)k);
        }
        ring_put(ptr + i, n);
        s_bol = nl != NULL;
        i += n;
    }
    return (ssize_t)len;
}

static int log_open_r(struct _reent *r, void *file, const char *path, int flags, int mode)
{
    (void)r; (void)file; (void)path; (void)flags; (void)mode;
    return 0;
}

static int log_close_r(struct _reent *r, void *fd)
{
    (void)r; (void)fd;
    return 0;
}

static const devoptab_t s_log_dev = {
    .name       = "log",
    .structSize = sizeof(int),
    .open_r     = log_open_r,
    .close_r    = log_close_r,
    .write_r    = log_write_r,
};

static void log_open(void)
{
    if (R_SUCCEEDED(socketInitializeDefault())) {
        s_sockets = 1;
        s_nxlink_fd = nxlinkStdio();       /* -1 unless launched by nxlink */
    }
    if (s_nxlink_fd >= 0)
        return;

    s_t0 = armGetSystemTick();
    mkdir(APP_DIR, 0777);
    /* The last run's log survives one more start: a hang reported after
     * the game was started again (2026-10-24) was gone otherwise. */
    remove(APP_DIR "/" MC3_APP "_log_prev.txt");
    rename(APP_DIR "/" MC3_APP "_log.txt", APP_DIR "/" MC3_APP "_log_prev.txt");
    s_log_fd = open(APP_DIR "/" MC3_APP "_log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (s_log_fd < 0)
        return;
    mutexInit(&s_ring_lock);
    mutexInit(&s_file_lock);
    condvarInit(&s_ring_cv);
    if (AddDevice(&s_log_dev) < 0 || !freopen("log:", "w", stdout)
            || !freopen("log:", "w", stderr)) {
        /* No device: the old way, a line-buffered file. */
        close(s_log_fd);
        s_log_fd = -1;
        if (freopen(APP_DIR "/" MC3_APP "_log.txt", "w", stdout))
            dup2(fileno(stdout), fileno(stderr));
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IOLBF, 0);
        return;
    }
    /* Unbuffered into the device: each printf is one memcpy into the ring. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    s_writer_on = 1;
    /* Core 2, low priority: off the game's cores, never ahead of them --
     * except in a measuring run (MC3_PERF): a hang in a race (2026-10-25)
     * left no line at all, not even the perf thread's, so the log has to
     * keep going while something spins above priority 59 (the interrupt
     * and APU threads run at 0x2C). The writer sleeps between 200 ms
     * blocks, so 0x28 costs the game little. */
    if (R_FAILED(threadCreate(&s_writer, log_writer, NULL, NULL, 0x4000,
                              getenv("MC3_PERF") ? 0x28 : 0x3B, 2))
            || R_FAILED(threadStart(&s_writer)))
        s_writer_on = 0;                /* still works: full ring drains inline */
}

/* The calling thread above the interrupt and APU threads (0x2C), for the
 * diagnostics that must keep running through a hang (MC3_PERF's report). */
void switch_raise_thread(void)
{
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x28);
}

/* The crash path: the faulting thread may hold a log lock, so no locks --
 * write what the ring holds, best effort, and let the process die. */
static void log_flush_crash(void)
{
    u64 tail = s_tail, head = s_head;
    if (s_log_fd < 0)
        return;
    while (tail < head) {
        u64 at = tail % LOG_RING, n = head - tail;
        if (n > LOG_RING - at) n = LOG_RING - at;
        if (write(s_log_fd, s_ring + at, (size_t)n) < 0)
            break;
        tail += n;
    }
}

/* Whatever is in the ring, onto the card now (exit). */
static void log_flush(void)
{
    fflush(stdout);
    fflush(stderr);
    if (s_log_fd >= 0)
        log_drain();
}

/* ------------------------------------------------------------------ */
/* Settings file                                                       */
/* ------------------------------------------------------------------ */

static void env_file_apply(void)
{
    char line[512];
    FILE *f = fopen(APP_DIR "/" MC3_APP "_env.txt", "r");

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *key = line, *eq, *end;

        while (*key == ' ' || *key == '\t')
            key++;
        if (*key == '#' || *key == '\n' || *key == '\r' || !*key)
            continue;
        if (!(eq = strchr(key, '=')))
            continue;
        *eq = '\0';
        end = eq + 1 + strcspn(eq + 1, "\r\n");
        *end = '\0';
        setenv(key, eq + 1, 1);
        printf("[switch] %s=%s\n", key, eq + 1);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* CPU exceptions                                                      */
/* ------------------------------------------------------------------ */

/* libnx runs __libnx_exception_handler on this stack when a thread faults
 * (requires both symbols). Unaligned atomics (0x103) are the classic
 * x86-vs-AArch64 fault only real hardware shows; emulators don't model it. */
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    uintptr_t base = (uintptr_t)__start__;

    fprintf(stderr, "[CRASH] exception 0x%X at pc=0x%llX (elf +0x%llX), "
                    "far=0x%llX, lr elf +0x%llX\n",
            ctx->error_desc,
            (unsigned long long)ctx->pc.x,
            (unsigned long long)(ctx->pc.x - base),
            (unsigned long long)ctx->far.x,
            (unsigned long long)(ctx->lr.x - base));
    fprintf(stderr, "  aarch64-none-elf-addr2line -f -e work/elf/mcdub3-<build>.elf 0x%llX\n",
            (unsigned long long)(ctx->pc.x - base));
    print_guest_state();
    log_flush_crash();
    svcExitProcess();
}

/* ------------------------------------------------------------------ */
/* Entry points used by main.c                                         */
/* ------------------------------------------------------------------ */

void switch_error_dialog(const char *msg)
{
    ErrorApplicationConfig c;

    if (R_SUCCEEDED(errorApplicationCreate(&c, msg, NULL)))
        errorApplicationShow(&c);
}

void switch_boot(void)
{
    log_open();
    printf("[switch] " MC3_APP " starting (%s)\n",
           appletGetAppletType() == AppletType_Application
               ? "title takeover" : "applet mode: expect too little memory");
    env_file_apply();
    /* Keep the console awake through long loads and movies. */
    appletSetMediaPlaybackState(true);
    /* Video capture (the Capture button's 30 s clips): with VideoCapture
     * "Enabled" in the host's NACP (our forwarders', switch/nsp.sh) the
     * application must hand the system its recording buffer, 96 MB, or a
     * clip comes out a second long (2026-10-27). Title takeover only;
     * MC3_RECORDING=0 keeps the memory. */
    {
        const char *e = getenv("MC3_RECORDING");
        bool ok = false;
        Result rc;
        if (appletGetAppletType() != AppletType_Application || (e && e[0] == '0')) {
            printf("[switch] video capture: not set up (%s)\n",
                   e && e[0] == '0' ? "MC3_RECORDING=0" : "applet mode");
        } else if (R_FAILED(rc = appletIsGamePlayRecordingSupported(&ok)) || !ok) {
            printf("[switch] video capture: not supported by the host title (0x%X)\n", rc);
        } else if (R_FAILED(rc = appletInitializeGamePlayRecording())
                   || R_FAILED(rc = appletSetGamePlayRecordingState(true))) {
            printf("[switch] video capture: setup failed (0x%X)\n", rc);
        } else {
            printf("[switch] video capture: on (30 s clips; MC3_RECORDING=0 off)\n");
        }
    }
}

void switch_shutdown(void)
{
    appletSetMediaPlaybackState(false);
    if (s_writer_on) {
        s_writer_on = 0;
        condvarWakeAll(&s_ring_cv);
        threadWaitForExit(&s_writer);
        threadClose(&s_writer);
    }
    log_flush();
    if (s_log_fd >= 0)
        close(s_log_fd);
    if (s_sockets)
        socketExit();
}
