/* host_main.c -- the process's main thread, kept for the window system.
 *
 * macOS lets only the main thread touch windows and events: SDL's video
 * start-up from the renderer's thread ended the first native run in an
 * NSInternalInconsistencyException ("setting the main menu on a non-main
 * thread", 2026-10-21). A recompiled title, though, runs its own main
 * thread for as long as it plays. So on macOS the title moves to a thread
 * of its own (with a main thread's stack size) and the real main thread
 * becomes a service loop: it runs what other threads hand it
 * (xbox_host_main_call: window and context creation, controller set-up)
 * and pumps the window system's events, which SDL then queues for any
 * thread to take (SDL_PeepEvents). The same on Windows (below). Elsewhere
 * both calls are direct: the title runs on the main thread as before.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "host_main.h"

/* Windows likewise: a window's messages reach only the thread that created
 * it. Created on the boot thread and presented from the title's, the window
 * never answered and Windows marked it "Not Responding" (2026-10-23). */
#if (defined(__APPLE__) || defined(_WIN32) || defined(__ANDROID__)) && !defined(XBOXRECOMP_NO_SDL)
#include <SDL.h>

typedef struct HostCall {
    void (*fn)(void *);
    void *arg;
    int done;
    struct HostCall *next;
} HostCall;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv = PTHREAD_COND_INITIALIZER;      /* new call or exit */
static pthread_cond_t  s_done_cv = PTHREAD_COND_INITIALIZER;
static HostCall       *s_head, *s_tail;
static pthread_t       s_main;
static int             s_serving, s_exit, s_exit_code;
static void          (*s_tick)(void);

int xbox_host_on_main(void)
{
    return !s_serving || pthread_equal(pthread_self(), s_main);
}

void xbox_host_main_call(void (*fn)(void *), void *arg)
{
    HostCall c;
    if (xbox_host_on_main()) {
        fn(arg);
        return;
    }
    c.fn = fn; c.arg = arg; c.done = 0; c.next = NULL;
    pthread_mutex_lock(&s_lock);
    if (s_tail) s_tail->next = &c; else s_head = &c;
    s_tail = &c;
    pthread_cond_signal(&s_cv);
    while (!c.done)
        pthread_cond_wait(&s_done_cv, &s_lock);
    pthread_mutex_unlock(&s_lock);
}

void xbox_host_main_tick(void (*fn)(void)) { s_tick = fn; }

static int SDLCALL host_event_trace(void *u, SDL_Event *e)
{
    (void)u;
    if (e->type != SDL_MOUSEMOTION && e->type != SDL_JOYAXISMOTION
        && e->type != SDL_CONTROLLERAXISMOTION && e->type != SDL_POLLSENTINEL)
        fprintf(stderr, "  [HOST] event 0x%X\n", e->type);
    return 1;
}

typedef struct { int (*fn)(int, char **); int argc; char **argv; } GuestStart;

static void *guest_main(void *p)
{
    GuestStart *g = p;
    int rc = g->fn(g->argc, g->argv);
    pthread_mutex_lock(&s_lock);
    s_exit = 1;
    s_exit_code = rc;
    pthread_cond_signal(&s_cv);
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

int xbox_host_main_run(int (*fn)(int, char **), int argc, char **argv)
{
    static GuestStart g;
    pthread_attr_t attr;
    pthread_t th;
    g.fn = fn; g.argc = argc; g.argv = argv;
    s_main = pthread_self();
    s_serving = 1;
    pthread_attr_init(&attr);
    /* A main thread's stack: lifted code nests deeply in C. 512 KB is
     * macOS's default for other threads; Linux gives main 8 MB. */
    pthread_attr_setstacksize(&attr, (size_t)64 << 20);
    if (pthread_create(&th, &attr, guest_main, &g) != 0) {
        s_serving = 0;
        return fn(argc, argv);
    }
    pthread_attr_destroy(&attr);
    for (;;) {
        struct timespec ts;
        HostCall *c;
        pthread_mutex_lock(&s_lock);
        if (!s_head && !s_exit) {
            /* Events at ~200 Hz: input latency well under a frame. */
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 5 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&s_cv, &s_lock, &ts);
        }
        if (s_exit) {
            pthread_mutex_unlock(&s_lock);
            break;
        }
        while ((c = s_head) != NULL) {
            s_head = c->next;
            if (!s_head) s_tail = NULL;
            pthread_mutex_unlock(&s_lock);
            c->fn(c->arg);
            pthread_mutex_lock(&s_lock);
            c->done = 1;
            pthread_cond_broadcast(&s_done_cv);
        }
        pthread_mutex_unlock(&s_lock);
        if (SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) {
            static int watch;
            if (!watch && getenv("RECOMP_HOST_EVENT_TRACE")) {
                watch = 1;
                SDL_AddEventWatch(host_event_trace, NULL);
            }
            SDL_PumpEvents();
            /* Close button, Cmd+Q, Dock > Quit. Also seen by the renderer
             * at its next frame; here so a quit during a load (no frames)
             * still works. */
            if (SDL_HasEvent(SDL_QUIT)) {
                fprintf(stderr, "  [HOST] quit requested\n");
                fflush(stderr);
                exit(0);
            }
        }
        if (s_tick)
            s_tick();
    }
    return s_exit_code;
}

#else   /* Linux, the Switch: the title keeps the main thread */

int  xbox_host_on_main(void) { return 1; }
void xbox_host_main_call(void (*fn)(void *), void *arg) { fn(arg); }
void xbox_host_main_tick(void (*fn)(void)) { (void)fn; }
int  xbox_host_main_run(int (*fn)(int, char **), int argc, char **argv)
{
    return fn(argc, argv);
}

#endif
