// DM_RECORD=<folder>: every run is a session that can be played again (scripts/replay.py). The run gets <folder>/<YYYYMMDD-HHMMSS>/ with
//   input.keys   player 1's input as DM_REC writes it (hle_hid.c; DM_REC set by the player wins and the session has none),
//   save/        the save as the game mounted it (save_device/ for a device save of its own): the committed directory at the mount, before the game read
//                anything, so a game that saved on its own (an autosave) in an earlier run starts the replay where this run started,
//   meta.tsv     "key<TAB>value" lines: title, build (main's build ID), started, cwd, one "arg" per host argument, one "env" per DM_* variable
//                (the launcher's settings reach the host as these), and at exit "frames" (presented frames) and "ended".
// DM_RECORD_KEEP (default 10) sessions are kept in the folder, the oldest removed at start. Replays are approximate with the real clock: a
// recording is stamped in presented frames, and a load that takes a different number of frames moves the input after it (docs/development.md)
#include "hle.h"
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <removefile.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static char g_dir[PATH_MAX];
static FILE *g_meta;
static uint64_t g_t0;

static int stamp_name(const char *s) {   // YYYYMMDD-HHMMSS, maybe "-<n>" after it
    for (int i = 0; i < 15; i++) if (i == 8 ? s[i] != '-' : (s[i] < '0' || s[i] > '9')) return 0;
    return !s[15] || s[15] == '-';
}
static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void prune(const char *root, int keep) {   // keeps the newest keep - 1, room for this run's
    DIR *d = opendir(root); if (!d) return;
    char *names[512]; int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < 512) if (stamp_name(e->d_name)) names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, name_cmp);
    for (int i = 0; i < n; i++) {
        if (i < n - (keep - 1)) {
            char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", root, names[i]);
            if (removefile(p, NULL, REMOVEFILE_RECURSIVE)) LOG("[RED] DM_RECORD: cannot remove the old session %s (errno %d)", p, errno);
        }
        free(names[i]);
    }
}
static void meta(const char *k, const char *v) { if (g_meta) { fprintf(g_meta, "%s\t%s\n", k, v); fflush(g_meta); } }
static void session_end(void) {
    if (!g_meta) return;
    char v[64]; snprintf(v, sizeof v, "%llu", (unsigned long long)dm_guest_frame()); meta("frames", v);
    snprintf(v, sizeof v, "%.1f s", (now_ns() - g_t0) / 1e9); meta("ended", v);
    fclose(g_meta); g_meta = NULL;
}

void dm_session_start(int argc, char **argv) {
    const char *root = getenv("DM_RECORD"); if (!root || !*root || !strcmp(root, "0")) return;
    const char *k = getenv("DM_RECORD_KEEP"); int keep = k ? atoi(k) : 10; if (keep < 1) keep = 1;
    mkdir(root, 0755);
    prune(root, keep);
    char st[32]; time_t t = time(NULL); strftime(st, sizeof st, "%Y%m%d-%H%M%S", localtime(&t));
    snprintf(g_dir, sizeof g_dir, "%s/%s", root, st);
    for (int i = 2; mkdir(g_dir, 0755) && errno == EEXIST && i < 100; i++) snprintf(g_dir, sizeof g_dir, "%s/%s-%d", root, st, i);
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/meta.tsv", g_dir);
    if (!(g_meta = fopen(p, "w"))) { LOG("[RED] DM_RECORD: cannot write %s (errno %d): this run is not recorded", p, errno); g_dir[0] = 0; return; }
    g_t0 = now_ns();
    meta("title", DM_GAME_ID);
    meta("build", dm_main_build());
    char when[32]; strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&t)); meta("started", when);
    char cwd[PATH_MAX]; if (getcwd(cwd, sizeof cwd)) meta("cwd", cwd);
    for (int i = 0; i < argc; i++) meta("arg", argv[i]);
    for (char **e = environ; *e; e++) if (!strncmp(*e, "DM_", 3)) meta("env", *e);
    if (getenv("DM_REC")) meta("input", "none: DM_REC was set by the player");
    else { snprintf(p, sizeof p, "%s/input.keys", g_dir); setenv("DM_REC", p, 1); }
    atexit(session_end);
    LOG("DM_RECORD: this run is recorded in %s (input, the save at its mount, the settings): scripts/replay.py plays it again", g_dir);
}

// The committed save directory dir, at its transactional mount (hle_fs.c mount_ex): copied once into the session, a clone on the same APFS volume
void dm_session_save(const char *dir) {
    if (!g_dir[0]) return;
    size_t n = strlen(dir); int device = n > 7 && !strcmp(dir + n - 7, "_device");
    char dst[PATH_MAX]; snprintf(dst, sizeof dst, "%s/%s", g_dir, device ? "save_device" : "save");
    if (!access(dst, F_OK)) return;
    if (clonefile(dir, dst, 0) && copyfile(dir, dst, NULL, COPYFILE_ALL | COPYFILE_RECURSIVE)) {
        LOG("[RED] DM_RECORD: cannot copy the save %s into the session (errno %d): a replay starts from an empty save", dir, errno); return; }
    meta(device ? "save_device" : "save", dir);
}
