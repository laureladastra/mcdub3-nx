/*
 * android_host.c -- Android boot glue (ADR 0010), the counterpart of
 * switch_nx.c on the Switch.
 *
 *   folder    the app's own external files folder, through SDL:
 *             /storage/emulated/0/Android/data/<app>/files -- what a user
 *             can reach by USB or a file manager; the working directory
 *   disc      the first holding default.xbe of <folder>/game-<edition>/,
 *             <folder>/game/, then on internal storage and every SD card
 *             <MC3_SD_DIR>/ and switch/<MC3_SD_DIR>/ (the Switch build's
 *             place: the same card under switchroot), game-<edition>/
 *             before game/, then game-<any edition>/; the start screen's
 *             choice (android/LauncherActivity.java), else MC3_GAME_DIR,
 *             overrides. Outside <folder> it takes "All files access"
 *   saves     <folder>/save/ (MC3_SAVE_DIR overrides)
 *   settings  <MC3_APP>_env.txt in <folder>, then in <MC3_SD_DIR>/ on
 *             internal storage (a file manager can reach that one), KEY=VALUE
 *             per line, '#' starts a comment; applied with setenv before
 *             boot, as on the Switch
 *   log       <folder>/<MC3_APP>_log.txt (the run before it: _log_prev.txt),
 *             and every line to logcat under the tag <MC3_APP> -- Android
 *             sends stdout and stderr nowhere otherwise
 */
#ifdef __ANDROID__
#include <SDL.h>
#include <android/log.h>
#include <dirent.h>
#include <jni.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MC3_APP
#  define MC3_APP "mcdub3"
#endif
#ifndef MC3_PACKAGE_EDITION
#  define MC3_PACKAGE_EDITION "usa"
#endif

static char s_dir[512];
static int  s_pipe[2] = { -1, -1 };
static FILE *s_log;

/* stdout and stderr, line by line, into the log file and logcat. */
static void *log_pump(void *arg)
{
    char buf[4096], line[4096];
    size_t have = 0;
    ssize_t n;
    (void)arg;
    while ((n = read(s_pipe[0], buf, sizeof buf)) > 0) {
        ssize_t i;
        if (s_log) {
            fwrite(buf, 1, (size_t)n, s_log);
            fflush(s_log);
        }
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n' || have == sizeof line - 1) {
                line[have] = '\0';
                __android_log_write(ANDROID_LOG_INFO, MC3_APP, line);
                have = 0;
                if (buf[i] == '\n')
                    continue;
            }
            line[have++] = buf[i];
        }
    }
    return NULL;
}

static void log_open(void)
{
    char path[600], prev[600];
    pthread_t th;
    snprintf(path, sizeof path, "%s/" MC3_APP "_log.txt", s_dir);
    snprintf(prev, sizeof prev, "%s/" MC3_APP "_log_prev.txt", s_dir);
    rename(path, prev);                      /* the run before, kept */
    s_log = fopen(path, "w");
    if (pipe(s_pipe) != 0)
        return;
    dup2(s_pipe[1], STDOUT_FILENO);
    dup2(s_pipe[1], STDERR_FILENO);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pthread_create(&th, NULL, log_pump, NULL) == 0)
        pthread_detach(th);
}

static void env_file_apply(const char *dir)
{
    char path[600], line[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/" MC3_APP "_env.txt", dir);
    if (!(f = fopen(path, "r")))
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
        printf("[android] %s=%s\n", key, eq + 1);
    }
    fclose(f);
}

/* The folder everything lives in (after android_boot). */
const char *android_app_dir(void)
{
    return s_dir;
}

/* MainActivity's static helpers: "All files access" (Android 11+), which a
 * disc outside the app's own folder needs. */
static int activity_call(const char *name)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    jclass cls;
    jmethodID m;
    int r = 0;
    if (!env || !act)
        return 0;
    cls = (*env)->GetObjectClass(env, act);
    m = cls ? (*env)->GetStaticMethodID(env, cls, name, "()Z") : NULL;
    if (m)
        r = (*env)->CallStaticBooleanMethod(env, cls, m) ? 1 : 0;
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (cls)
        (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return r;
}

static int has_disc(const char *dir)
{
    char path[700];
    snprintf(path, sizeof path, "%s/default.xbe", dir);
    return access(path, R_OK) == 0;
}

/* The disc: the app's folder, then MC3_SD_DIR/ (a folder of its own on the
 * device's storage) and switch/MC3_SD_DIR/ (where the Switch build keeps it:
 * the same SD card under switchroot) on internal storage and every SD card;
 * game-<edition>/ before game/ in each. */
static int find_disc(char *out, size_t cap)
{
    static const char *const sub[] = {
        MC3_SD_DIR "/game-" MC3_PACKAGE_EDITION, MC3_SD_DIR "/game",
        "switch/" MC3_SD_DIR "/game-" MC3_PACKAGE_EDITION, "switch/" MC3_SD_DIR "/game",
    };
    char roots[16][300];
    int nroots = 0, r, k;
    DIR *d;
    snprintf(out, cap, "%s/game-" MC3_PACKAGE_EDITION, s_dir);
    if (has_disc(out))
        return 1;
    snprintf(out, cap, "%s/game", s_dir);
    if (has_disc(out))
        return 1;
    snprintf(roots[nroots++], sizeof roots[0], "/storage/emulated/0");
    if ((d = opendir("/storage")) != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && nroots < 16) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "self")
                || !strcmp(e->d_name, "emulated"))
                continue;
            snprintf(roots[nroots++], sizeof roots[0], "/storage/%s", e->d_name);
        }
        closedir(d);
    }
    for (r = 0; r < nroots; r++)
        for (k = 0; k < (int)(sizeof sub / sizeof sub[0]); k++) {
            snprintf(out, cap, "%s/%s", roots[r], sub[k]);
            if (has_disc(out))
                return 1;
        }
    /* Another edition's disc (game-eur/ for the usa package, ...): the boot
     * check takes every edition this lift accepts (xbe_editions.h). */
    {
        char dirs[3 * 16][330];
        int nd = 0, i;
        snprintf(dirs[nd++], sizeof dirs[0], "%s", s_dir);
        for (r = 0; r < nroots && nd + 2 <= (int)(sizeof dirs / sizeof dirs[0]); r++) {
            snprintf(dirs[nd++], sizeof dirs[0], "%s/" MC3_SD_DIR, roots[r]);
            snprintf(dirs[nd++], sizeof dirs[0], "%s/switch/" MC3_SD_DIR, roots[r]);
        }
        for (i = 0; i < nd; i++) {
            struct dirent *e;
            if (!(d = opendir(dirs[i])))
                continue;
            while ((e = readdir(d)) != NULL)
                if (!strncmp(e->d_name, "game-", 5)) {
                    snprintf(out, cap, "%s/%s", dirs[i], e->d_name);
                    if (has_disc(out)) {
                        closedir(d);
                        return 1;
                    }
                }
            closedir(d);
        }
    }
    return 0;
}

void android_boot(void)
{
    const char *d = SDL_AndroidGetExternalStoragePath();
    char path[700];
    if (!d || !d[0])
        d = SDL_AndroidGetInternalStoragePath();
    snprintf(s_dir, sizeof s_dir, "%s", d ? d : ".");
    mkdir(s_dir, 0775);
    if (chdir(s_dir) != 0)
        __android_log_print(ANDROID_LOG_WARN, MC3_APP, "chdir %s: %s", s_dir, strerror(errno));
    log_open();
    printf("[android] " MC3_APP " starting in %s\n", s_dir);
    /* Back (the gesture, a pad's or keyboard's) is the Xbox's Back, not
     * "leave the game". */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    env_file_apply(s_dir);
    env_file_apply("/storage/emulated/0/" MC3_SD_DIR);   /* reachable by a file manager */
    {   /* the start screen's choice (android/LauncherActivity.java) wins */
        const char *l = getenv("MC3_LAUNCHER_GAME_DIR");
        if (l && *l)
            setenv("MC3_GAME_DIR", l, 1);
        if ((l = getenv("MC3_LAUNCHER_TOUCH_PAD")) != NULL && *l)
            setenv("RECOMP_TOUCH_PAD", l, 1);  /* auto, 1 always, 0 never */
    }
    for (;;) {
        const char *g = getenv("MC3_GAME_DIR");
        static const SDL_MessageBoxButtonData buttons[] = {
            { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Retry" },
            { 0, 2, "Allow storage access" },
            { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" },
        };
        SDL_MessageBoxData box;
        char msg[1600];
        int access_ok, hit = -1;
        if (g ? has_disc(g) : find_disc(path, sizeof path)) {
            if (!g)
                setenv("MC3_GAME_DIR", path, 1);
            printf("[android] disc: %s\n", getenv("MC3_GAME_DIR"));
            break;
        }
        /* No disc: say where it may go, rather than closing without a word. */
        access_ok = activity_call("hasAllFilesAccess");
        printf("[android] no disc found (%s; all files access %s)\n",
               g ? g : "searched", access_ok ? "on" : "off");
        snprintf(msg, sizeof msg,
                 "No game disc found.\n\n"
                 "Put the extracted disc (the folder holding default.xbe) in one of:\n\n"
                 "  " MC3_SD_DIR "/game-" MC3_PACKAGE_EDITION "/  on the device's storage or SD card\n"
                 "  switch/" MC3_SD_DIR "/game-" MC3_PACKAGE_EDITION "/  (as the Switch build has it)\n"
                 "  %s/game-" MC3_PACKAGE_EDITION "/\n\n"
                 "or name its folder in " MC3_SD_DIR "/" MC3_APP "_env.txt: MC3_GAME_DIR=/path/to/disc%s%s%s",
                 s_dir, g ? "\n\nMC3_GAME_DIR is set to " : "", g ? g : "",
                 access_ok ? "" : "\n\nOutside the app's folder the game needs \"All files access\".");
        memset(&box, 0, sizeof box);
        box.flags = SDL_MESSAGEBOX_ERROR;
        box.title = "Midnight Club 3";
        box.message = msg;
        box.numbuttons = access_ok ? 2 : 3;
        box.buttons = access_ok ? (const SDL_MessageBoxButtonData[]){ buttons[0], buttons[2] }
                                : buttons;
        if (SDL_ShowMessageBox(&box, &hit) != 0 || hit == 0)
            exit(0);
        if (hit == 2)
            activity_call("openAllFilesAccess");
    }
    if (!getenv("MC3_SAVE_DIR")) {
        snprintf(path, sizeof path, "%s/save", s_dir);
        mkdir(path, 0775);
        setenv("MC3_SAVE_DIR", path, 1);
    }
}
#endif
