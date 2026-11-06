// Input journal: every run is recorded so it can be replayed, in the same files and with the same behaviour as Eden's input journal on the PC
// (chocokage/PokeDinner-a-Eglantine-Fork docs/nx2, src/core/input_journal.cpp), so a recording plays on either machine. Each run is a session
// <root>/<YYYYMMDD-HHMMSS>_<title ID>/ of segments seg_NNN: seg_000 from boot, and a new one each time the game loads its save again (a full read of a
// save file at least 60 frames after a commit). A segment holds meta.txt (key=value), input.txt (player 1's non-neutral frames:
// "<at> <KEY_A;KEY_B;...|NONE> <lx>;<ly> <rx>;<ry>"), events.txt (save reads "<at> R <offset> <size> <file>" and commits
// "<at> C <hash> <name=hash|...>", the sync points a replay aligns on) and save/, the save as the segment began.
// Replay (DM_REPLAY=<segment folder>): the segment's save is swapped in (the user's is put back at exit, or after a crash at the next start), the
// recorded input is given to player 1 frame by frame, held while the game has not reached the next recorded save read or commit, and the player takes
// over by pressing a button or pushing a stick past halfway: the replayed save is kept and recording goes on in a new session that replays to here.
// Same seed, boot date and guest clock in both runs (dm_journal_init). Off unless DM_INPUT_JOURNAL=1 or DM_REPLAY is set (NX Play sets them); the
// EDEN_* names are accepted too. docs/development.md "Recording and replaying a run".
#include "hle.h"
#include "journal.h"
#include <copyfile.h>
#include <dirent.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>
#include <removefile.h>
#include <stdatomic.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

enum { MAX_SESSIONS = 10, LOAD_SEGMENTS_KEPT = 2, FLUSH_FRAMES = 30, RELOAD_GUARD_FRAMES = 60, MAX_HOLD_FRAMES = 1800, ANCHOR_LOOKAHEAD = 16 };
static const uint64_t MAX_SNAPSHOT_BYTES = 256ull << 20, RECORDED_BUTTONS = 0xffff;
enum { STICK_HALF = 0x4000 };
// The TAS script's names and bit order (Eden input_common/drivers/tas_input.cpp), which are NpadButton's bits 0..15
static const char *const BUTTON_NAMES[16] = {"KEY_A", "KEY_B", "KEY_X", "KEY_Y", "KEY_LSTICK", "KEY_RSTICK", "KEY_L", "KEY_R",
                                             "KEY_ZL", "KEY_ZR", "KEY_PLUS", "KEY_MINUS", "KEY_DLEFT", "KEY_DUP", "KEY_DRIGHT", "KEY_DDOWN"};

typedef struct { uint64_t buttons; int32_t lx, ly, rx, ry; } sample_t;
typedef struct { uint64_t at; sample_t s; } line_t;
typedef struct { uint64_t at; int commit; int64_t offset, size; uint64_t hash; char *file; } event_t;
typedef struct { event_t *v; size_t n, cap; } events_t;

static struct {
    pthread_mutex_t mu;
    uint64_t title_id, frame, segment_index, segment_start, last_flush_frame, last_commit_frame;
    int recording, replaying, replay_running, has_initial_snapshot, committed_since_segment, handed_over, unflushed, latch, frame_clock;
    int seed_set, boot_set; uint32_t rng_seed; int64_t boot_time;
    char session_dir[PATH_MAX], current_segment[PATH_MAX];
    FILE *input, *events;
    char replay_dir[PATH_MAX], replay_trigger[PATH_MAX], replay_save_dir[PATH_MAX], replay_backup[PATH_MAX];
    line_t *samples; size_t nsamples, capsamples;
    events_t reads, commits;
    uint64_t replay_start_frame, replay_begin, replay_length;
    size_t next_read, next_commit;
    int replay_from_load, replay_player_input;
    sample_t takeover_prev;
    uint64_t hold_frames, total_held, total_skipped, anchors_hit, anchors_missed, commits_same, commits_diff;
} J = {.mu = PTHREAD_MUTEX_INITIALIZER, .replay_start_frame = ~0ull};
static _Atomic int g_on, g_swapped;   // recording or replaying; a replay has the user's save swapped out
static void on_update(void) { atomic_store(&g_on, J.recording || J.replaying); atomic_store(&g_swapped, J.replay_save_dir[0] != 0); }

// ---------- CityHash64 (Geoff Pike and Jyrki Alakuijala, Google, MIT licence), as Eden's common/cityhash.cpp ----------
static const uint64_t K0 = 0xc3a5c85c97cb3127ull, K1 = 0xb492b66fbe98f273ull, K2 = 0x9ae16a3b2f90404full;
static uint64_t fetch64(const char *p) { uint64_t r; memcpy(&r, p, 8); return r; }
static uint32_t fetch32(const char *p) { uint32_t r; memcpy(&r, p, 4); return r; }
static uint64_t rot(uint64_t v, int s) { return s == 0 ? v : (v >> s) | (v << (64 - s)); }
static uint64_t shift_mix(uint64_t v) { return v ^ (v >> 47); }
static uint64_t hash16m(uint64_t u, uint64_t v, uint64_t mul) {
    uint64_t a = (u ^ v) * mul; a ^= a >> 47;
    uint64_t b = (v ^ a) * mul; b ^= b >> 47; return b * mul;
}
static uint64_t hash16(uint64_t u, uint64_t v) { return hash16m(u, v, 0x9ddfea08eb382d69ull); }   // Hash128to64
static uint64_t len0to16(const char *s, size_t len) {
    if (len >= 8) {
        uint64_t mul = K2 + len * 2, a = fetch64(s) + K2, b = fetch64(s + len - 8);
        uint64_t c = rot(b, 37) * mul + a, d = (rot(a, 25) + b) * mul;
        return hash16m(c, d, mul);
    }
    if (len >= 4) { uint64_t mul = K2 + len * 2, a = fetch32(s); return hash16m(len + (a << 3), fetch32(s + len - 4), mul); }
    if (len > 0) {
        uint8_t a = (uint8_t)s[0], b = (uint8_t)s[len >> 1], c = (uint8_t)s[len - 1];
        uint32_t y = (uint32_t)a + ((uint32_t)b << 8), z = (uint32_t)len + ((uint32_t)c << 2);
        return shift_mix(y * K2 ^ z * K0) * K2;
    }
    return K2;
}
static uint64_t len17to32(const char *s, size_t len) {
    uint64_t mul = K2 + len * 2, a = fetch64(s) * K1, b = fetch64(s + 8), c = fetch64(s + len - 8) * mul, d = fetch64(s + len - 16) * K2;
    return hash16m(rot(a + b, 43) + rot(c, 30) + d, a + rot(b + K2, 18) + c, mul);
}
typedef struct { uint64_t first, second; } pair64;
static pair64 weak32(uint64_t w, uint64_t x, uint64_t y, uint64_t z, uint64_t a, uint64_t b) {
    a += w; b = rot(b + a + z, 21);
    uint64_t c = a; a += x; a += y; b += rot(a, 44);
    return (pair64){a + z, b + c};
}
static pair64 weak32s(const char *s, uint64_t a, uint64_t b) { return weak32(fetch64(s), fetch64(s + 8), fetch64(s + 16), fetch64(s + 24), a, b); }
static uint64_t len33to64(const char *s, size_t len) {
    uint64_t mul = K2 + len * 2, a = fetch64(s) * K2, b = fetch64(s + 8), c = fetch64(s + len - 24), d = fetch64(s + len - 32);
    uint64_t e = fetch64(s + 16) * K2, f = fetch64(s + 24) * 9, g = fetch64(s + len - 8), h = fetch64(s + len - 16) * mul;
    uint64_t u = rot(a + g, 43) + (rot(b, 30) + c) * 9, v = ((a + g) ^ d) + f + 1, w = __builtin_bswap64((u + v) * mul) + h;
    uint64_t x = rot(e + f, 42) + c, y = (__builtin_bswap64((v + w) * mul) + g) * mul, z = e + f + c;
    a = __builtin_bswap64((x + z) * mul + y) + b;
    b = shift_mix((z + a) * mul + d + h) * mul;
    return b + x;
}
uint64_t dm_cityhash64(const char *s, size_t len) {
    if (len <= 32) return len <= 16 ? len0to16(s, len) : len17to32(s, len);
    if (len <= 64) return len33to64(s, len);
    uint64_t x = fetch64(s + len - 40), y = fetch64(s + len - 16) + fetch64(s + len - 56), z = hash16(fetch64(s + len - 48) + len, fetch64(s + len - 24));
    pair64 v = weak32s(s + len - 64, len, z), w = weak32s(s + len - 32, y + K1, x);
    x = x * K1 + fetch64(s);
    len = (len - 1) & ~(size_t)63;
    do {
        x = rot(x + y + v.first + fetch64(s + 8), 37) * K1;
        y = rot(y + v.second + fetch64(s + 48), 42) * K1;
        x ^= w.second;
        y += v.first + fetch64(s + 40);
        z = rot(z + w.first, 33) * K1;
        v = weak32s(s, v.second * K1, x + w.first);
        w = weak32s(s + 32, z + w.second, y + fetch64(s + 16));
        uint64_t t = z; z = x; x = t;
        s += 64; len -= 64;
    } while (len != 0);
    return hash16(hash16(v.first, w.first) + shift_mix(y) * K1 + z, hash16(v.second, w.second) + x);
}
uint64_t dm_cityhash64_seeds(const char *s, size_t len, uint64_t seed0, uint64_t seed1) { return hash16(dm_cityhash64(s, len) - seed0, seed1); }

// ---------- Environment, paths, files ----------
static const char *env2(const char *dm, const char *eden) { const char *v = getenv(dm); return v ? v : getenv(eden); }
static int env_zero(const char *dm, const char *eden) { const char *v = env2(dm, eden); return v && !strcmp(v, "0"); }
static int env_on(const char *dm, const char *eden) { const char *v = env2(dm, eden); return v && *v && strcmp(v, "0"); }
static const char *replay_env(void) { const char *v = env2("DM_REPLAY", "EDEN_REPLAY"); return v && *v ? v : NULL; }
// <log folder>/replay: ~/Library/Logs/nxnative/replay as Eden's <log folder>/replay; DM_REPLAY_ROOT moves it (tests)
static const char *root_dir(void) {
    static char r[PATH_MAX];
    if (!r[0]) { const char *e = getenv("DM_REPLAY_ROOT"), *h = getenv("HOME");
        if (e && *e) strlcpy(r, e, sizeof r); else snprintf(r, sizeof r, "%s/Library/Logs/nxnative/replay", h ? h : "/tmp"); }
    return r;
}
static void backups_dir(char *out, size_t n) {   // beside replay/, so pruning sessions does not touch it
    char p[PATH_MAX]; strlcpy(p, root_dir(), sizeof p);
    char *sl = strrchr(p, '/'); if (sl) *sl = 0;
    snprintf(out, n, "%s/replay_save_backups", p);
}
static void marker_path(char *out, size_t n) { snprintf(out, n, "%s/pending_save_restore.txt", root_dir()); }
static void timestamp(char *out, size_t n) { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(out, n, "%Y%m%d-%H%M%S", &tm); }
static int exists(const char *p) { return !access(p, F_OK); }
static void mkdirs(const char *path) {
    char p[PATH_MAX]; strlcpy(p, path, sizeof p);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
static void rm_all(const char *p) { if (exists(p)) removefile(p, NULL, REMOVEFILE_RECURSIVE); }
static uint64_t dir_size(const char *dir) {
    char *const paths[] = {(char *)dir, NULL}; uint64_t total = 0;
    FTS *f = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL); if (!f) return 0;
    for (FTSENT *e; (e = fts_read(f));) if (e->fts_info == FTS_F) total += (uint64_t)e->fts_statp->st_size;
    fts_close(f); return total;
}
// A directory tree: an APFS clone when it can (the save and the log folder on one volume), else a copy with the attributes
static int copy_dir(const char *from, const char *to) {
    char parent[PATH_MAX]; strlcpy(parent, to, sizeof parent); char *sl = strrchr(parent, '/'); if (sl) { *sl = 0; mkdirs(parent); }
    if (!exists(to) && !clonefile(from, to, 0)) return 1;
    if (!copyfile(from, to, NULL, COPYFILE_ALL | COPYFILE_RECURSIVE)) return 1;
    LOG("[RED] input journal: copying %s to %s failed (errno %d)", from, to, errno);
    return 0;
}
static char *read_all(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY); if (fd < 0) return NULL;
    struct stat st; if (fstat(fd, &st)) { close(fd); return NULL; }
    size_t n = (size_t)st.st_size, got = 0; char *b = malloc(n + 1);
    while (b && got < n) { ssize_t r = read(fd, b + got, n - got); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; got += (size_t)r; }
    close(fd); if (b) b[got] = 0; *len = got; return b;
}
static void chomp(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0; }   // a PC recording's meta.txt has CRLF lines
static int read_meta(const char *segment, const char *key, char *out, size_t n) {   // the first key= line
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/meta.txt", segment);
    FILE *f = fopen(p, "r"); out[0] = 0; if (!f) return 0;
    char line[4096]; size_t kl = strlen(key); int found = 0;
    while (fgets(line, sizeof line, f)) { chomp(line);
        if (strlen(line) > kl && !strncmp(line, key, kl) && line[kl] == '=') { strlcpy(out, line + kl + 1, n); found = 1; break; } }
    fclose(f); return found;
}
static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
// Whether a session or save backup folder (<timestamp>_<title ID>[_N]) belongs to title_id
static int names_game(const char *name, uint64_t title_id) {
    const char *u = strchr(name, '_'); if (!u) return 0;
    char tid[17]; snprintf(tid, sizeof tid, "%016llX", (unsigned long long)title_id);
    return !strncasecmp(u + 1, tid, 16) && (u[17] == '\0' || u[17] == '_');
}
// Keeps the newest `keep` directories of parent whose names start with prefix (names begin with a sortable timestamp or a zero-padded index),
// only counting one game's when game is set; always_keep is never counted or removed
static void keep_newest_of(const char *parent, const char *prefix, size_t keep, const char *always_keep, const uint64_t *game) {
    DIR *d = opendir(parent); if (!d) return;
    char **names = NULL; size_t n = 0, cap = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strncmp(e->d_name, prefix, strlen(prefix)) || (always_keep && !strcmp(e->d_name, always_keep)) || (game && !names_game(e->d_name, *game))) continue;
        char p[PATH_MAX]; struct stat st; snprintf(p, sizeof p, "%s/%s", parent, e->d_name);
        if (stat(p, &st) || !S_ISDIR(st.st_mode)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; char **nn = realloc(names, cap * sizeof *nn); if (!nn) break; names = nn; }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n > keep) {
        qsort(names, n, sizeof *names, name_cmp);
        for (size_t i = 0; i + keep < n; i++) { char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", parent, names[i]); rm_all(p); }
    }
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}
static void keep_newest(const char *parent, const char *prefix, size_t keep, const char *always_keep) { keep_newest_of(parent, prefix, keep, always_keep, NULL); }
// Sessions and save backups: the newest MAX_SESSIONS of each game
static void keep_newest_game(const char *parent) { keep_newest_of(parent, "", MAX_SESSIONS, NULL, &J.title_id); }

// ---------- Save hashes: CityHash64 of each file's content, keyed by its path in the save ('/' separators), in byte order ----------
typedef struct { char *name; uint64_t hash; } fhash_t;
static int fhash_cmp(const void *a, const void *b) { return strcmp(((const fhash_t *)a)->name, ((const fhash_t *)b)->name); }
static fhash_t *hash_files(const char *dir, size_t *count) {
    char *const paths[] = {(char *)dir, NULL}; fhash_t *v = NULL; size_t n = 0, cap = 0, dl = strlen(dir);
    FTS *f = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    for (FTSENT *e; f && (e = fts_read(f));) {
        if (e->fts_info != FTS_F || strlen(e->fts_path) <= dl + 1) continue;
        size_t len; char *data = read_all(e->fts_path, &len); if (!data) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; fhash_t *nv = realloc(v, cap * sizeof *nv); if (!nv) { free(data); break; } v = nv; }
        v[n].name = strdup(e->fts_path + dl + 1); v[n].hash = dm_cityhash64(data, len); n++;
        free(data);
    }
    if (f) fts_close(f);
    if (n) qsort(v, n, sizeof *v, fhash_cmp);
    *count = n; return v;
}
static uint64_t combine_hashes(const fhash_t *v, size_t n) {
    uint64_t c = 0; for (size_t i = 0; i < n; i++) c = dm_cityhash64_seeds(v[i].name, strlen(v[i].name), c, v[i].hash); return c;
}
static char *format_hashes(const fhash_t *v, size_t n) {   // name=hash|name=hash
    size_t cap = 1; for (size_t i = 0; i < n; i++) cap += strlen(v[i].name) + 18;
    char *out = malloc(cap); size_t l = 0; out[0] = 0;
    for (size_t i = 0; i < n; i++) l += (size_t)snprintf(out + l, cap - l, "%s%s=%016llx", i ? "|" : "", v[i].name, (unsigned long long)v[i].hash);
    return out;
}
static void free_hashes(fhash_t *v, size_t n) { for (size_t i = 0; i < n; i++) free(v[i].name); free(v); }

// ---------- Text formats ----------
static void format_buttons(uint64_t b, char *out, size_t n) {
    out[0] = 0; for (int i = 0; i < 16; i++) if (b >> i & 1) { strlcat(out, BUTTON_NAMES[i], n); strlcat(out, ";", n); }
    if (!out[0]) strlcpy(out, "NONE", n);
}
static uint64_t parse_buttons(const char *t) {
    uint64_t b = 0;
    while (*t) {
        const char *end = strchr(t, ';'); size_t l = end ? (size_t)(end - t) : strlen(t);
        for (int i = 0; i < 16; i++) if (strlen(BUTTON_NAMES[i]) == l && !strncmp(BUTTON_NAMES[i], t, l)) b |= 1ull << i;
        if (!end) break; t = end + 1;
    }
    return b;
}
static int neutral(const sample_t *s) { return !s->buttons && !s->lx && !s->ly && !s->rx && !s->ry; }
static int same_sample(const sample_t *a, const sample_t *b) { return a->buttons == b->buttons && a->lx == b->lx && a->ly == b->ly && a->rx == b->rx && a->ry == b->ry; }
static void write_sample(uint64_t frame, const sample_t *s) {
    if (!J.input) return;
    char b[256]; format_buttons(s->buttons, b, sizeof b);
    fprintf(J.input, "%llu %s %d;%d %d;%d\n", (unsigned long long)(frame - J.segment_start), b, s->lx, s->ly, s->rx, s->ry);
    J.unflushed = 1;
}
static void write_event(const event_t *e) {
    if (!J.events) return;
    if (e->commit) fprintf(J.events, "%llu C %016llx %s\n", (unsigned long long)e->at, (unsigned long long)e->hash, e->file);
    else fprintf(J.events, "%llu R %lld %lld %s\n", (unsigned long long)e->at, (long long)e->offset, (long long)e->size, e->file);
    J.unflushed = 1;
}
static void flush_files(void) {
    if (J.input) fflush(J.input);
    if (J.events) fflush(J.events);
    J.last_flush_frame = J.frame; J.unflushed = 0;
}
static void events_push(events_t *v, const event_t *e) {
    if (v->n == v->cap) { size_t cap = v->cap ? v->cap * 2 : 32; event_t *nv = realloc(v->v, cap * sizeof *nv); if (!nv) return; v->v = nv; v->cap = cap; }
    v->v[v->n++] = *e;
}
static void events_clear(events_t *v) { for (size_t i = 0; i < v->n; i++) free(v->v[i].file); v->n = 0; }
static void replay_clear(void) { J.nsamples = 0; events_clear(&J.reads); events_clear(&J.commits); }

// ---------- Input ----------
static sample_t live_sample(void) {
    uint64_t b; int32_t a[4]; dm_hid_live_sample(&b, a);
    return (sample_t){b & RECORDED_BUTTONS, a[0], a[1], a[2], a[3]};
}
static void apply_input(const sample_t *s) { int32_t a[4] = {s->lx, s->ly, s->rx, s->ry}; dm_hid_override(1, s->buttons, a); }
static void clear_input(void) { dm_hid_override(0, 0, (const int32_t[4]){0}); }
// The player takes over by pressing a button or pushing a stick past halfway; input already held when the replay started does not count
static int took_over(const sample_t *now, const sample_t *before) {
    #define PUSHED(f) (abs(now->f) >= STICK_HALF && abs(before->f) < STICK_HALF)
    return (now->buttons & ~before->buttons) != 0 || PUSHED(lx) || PUSHED(ly) || PUSHED(rx) || PUSHED(ry);
    #undef PUSHED
}

// ---------- Saves ----------
static int restore_user_save(const char *save_dir, const char *backup) {   // the user's save back over a replay's copy
    rm_all(save_dir);
    if (backup[0]) { if (!copy_dir(backup, save_dir)) return 0; rm_all(backup); }
    LOG("input journal: put the original save back into %s", save_dir);
    return 1;
}
static void restore_pending_save(void) {   // a replay that did not exit cleanly left the user's save swapped out
    char m[PATH_MAX]; marker_path(m, sizeof m);
    FILE *f = fopen(m, "r"); if (!f) return;
    char save_dir[PATH_MAX] = "", backup[PATH_MAX] = "";
    if (!fgets(save_dir, sizeof save_dir, f)) save_dir[0] = 0;
    if (!fgets(backup, sizeof backup, f)) backup[0] = 0;
    fclose(f); chomp(save_dir); chomp(backup);
    if (!save_dir[0]) return;
    if (backup[0] && !exists(backup)) { LOG("[RED] input journal: the save backup %s from an interrupted replay is missing", backup); return; }
    LOG("[RED] input journal: the last replay did not exit cleanly, restoring the save");
    if (restore_user_save(save_dir, backup)) unlink(m);
}
// A recording's save_dir is a path on the machine that made it (a PC's Eden folder, another Mac's): the replay's save goes to this machine's save for
// the game, DM_SAVE, or its device save (<DM_SAVE>_device) when the recorded one was that
static void local_save_dir(const char *recorded, char *out, size_t n) {
    const char *d = getenv("DM_SAVE");
    if (!d || !*d) { strlcpy(out, recorded, n); return; }
    strlcpy(out, d, n); size_t l = strlen(out); while (l > 1 && out[l - 1] == '/') out[--l] = 0;
    size_t rl = strlen(recorded);
    if (rl > 7 && !strcmp(recorded + rl - 7, "_device")) strlcat(out, "_device", n);
}

// ---------- Segments ----------
static void close_segment(void) {
    if (J.input) { fclose(J.input); J.input = NULL; }
    if (J.events) { fclose(J.events); J.events = NULL; }
}
static FILE *open_in(const char *dir, const char *name, const char *mode) { char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, name); return fopen(p, mode); }
static void begin_segment(const char *save_dir) {
    close_segment();
    snprintf(J.current_segment, sizeof J.current_segment, "%s/seg_%03llu", J.session_dir, (unsigned long long)J.segment_index);
    mkdirs(J.current_segment);
    J.segment_start = J.frame; J.committed_since_segment = 0;
    if (save_dir) {
        char to[PATH_MAX]; snprintf(to, sizeof to, "%s/save", J.current_segment);
        if (dir_size(save_dir) <= MAX_SNAPSHOT_BYTES) copy_dir(save_dir, to);
        else LOG("input journal: the save is larger than %llu MiB, not copied", (unsigned long long)(MAX_SNAPSHOT_BYTES >> 20));
    }
    char ts[32]; timestamp(ts, sizeof ts);
    FILE *m = open_in(J.current_segment, "meta.txt", "w");
    if (m) {
        fprintf(m, "title_id=%016llX\nsegment=%llu\nstart=%s\nstart_frame=%llu\nstarted=%s\n", (unsigned long long)J.title_id, (unsigned long long)J.segment_index,
                save_dir ? "load" : "boot", (unsigned long long)J.segment_start, ts);
        if (J.seed_set) fprintf(m, "rng_seed=%08X\n", J.rng_seed);
        if (J.boot_set) fprintf(m, "boot_time=%lld\n", (long long)J.boot_time);
        if (save_dir) fprintf(m, "save_dir=%s\n", save_dir);
        fclose(m);
    }
    J.input = open_in(J.current_segment, "input.txt", "wb");
    J.events = open_in(J.current_segment, "events.txt", "wb");
    J.last_flush_frame = J.frame; J.unflushed = 0;
    J.segment_index++;
    keep_newest(J.session_dir, "seg_", LOAD_SEGMENTS_KEPT, "seg_000");
}

// ---------- Replay ----------
static void start_replay_clock(uint64_t begin) { J.replay_running = 1; J.replay_begin = begin; J.hold_frames = 0; }
static void anchor_replay(events_t *ev, size_t *next, size_t index) {
    if (index > *next) { J.total_skipped += index - *next; LOG("input journal: replay: %zu recorded save events did not happen", index - *next); }
    int64_t drift = (int64_t)(J.frame - J.replay_begin) - (int64_t)ev->v[index].at;
    if (drift) LOG("input journal: replay: resynced by %lld frames at guest frame %llu", (long long)-drift, (unsigned long long)J.frame);
    J.replay_begin = J.frame - ev->v[index].at;
    *next = index + 1; J.hold_frames = 0; J.anchors_hit++;
}
static void finish_replay(const char *reason) {
    clear_input();
    J.replaying = 0; J.replay_running = 0; on_update();
    LOG("input journal: replay %s at guest frame %llu: %llu sync points hit, %llu missed, %llu skipped, held %llu frames; commits: %llu identical, %llu differ",
        reason, (unsigned long long)J.frame, (unsigned long long)J.anchors_hit, (unsigned long long)J.anchors_missed, (unsigned long long)J.total_skipped,
        (unsigned long long)J.total_held, (unsigned long long)J.commits_same, (unsigned long long)J.commits_diff);
}
static void keep_replay_save(void) {   // after a take-over: the replayed save stays, the user's from before goes to replay_save_backups
    if (!J.replay_save_dir[0]) return;
    if (J.replay_backup[0]) {
        char broot[PATH_MAX], kept[PATH_MAX], ts[32]; backups_dir(broot, sizeof broot); timestamp(ts, sizeof ts);
        snprintf(kept, sizeof kept, "%s/%s_%016llX", broot, ts, (unsigned long long)J.title_id);
        for (int k = 2; exists(kept) && k < 100; k++) snprintf(kept, sizeof kept, "%s/%s_%016llX_%d", broot, ts, (unsigned long long)J.title_id, k);   // two in one second
        mkdirs(broot);
        if (rename(J.replay_backup, kept)) LOG("input journal: could not move %s (errno %d)", J.replay_backup, errno);
        else { strlcpy(J.replay_backup, kept, sizeof J.replay_backup); keep_newest_game(broot); }
    }
    char m[PATH_MAX]; marker_path(m, sizeof m); unlink(m);
    LOG("input journal: keeping the replayed save; the one from before is in %s", J.replay_backup);
    J.replay_save_dir[0] = J.replay_backup[0] = 0; on_update();
}
// Recording on from a handed-over replay: seg_000 of a new session holds the replayed segment's save and input up to `upto`, so it replays to the
// same point and then on with the player's input
static void continue_recording(uint64_t upto) {
    if (env_zero("DM_INPUT_JOURNAL", "EDEN_INPUT_JOURNAL") || !J.title_id) return;
    char ts[32]; timestamp(ts, sizeof ts);
    snprintf(J.session_dir, sizeof J.session_dir, "%s/%s_%016llX", root_dir(), ts, (unsigned long long)J.title_id);
    snprintf(J.current_segment, sizeof J.current_segment, "%s/seg_000", J.session_dir);
    mkdirs(J.current_segment);
    if (!exists(J.current_segment)) { LOG("[RED] input journal: cannot create %s", J.session_dir); return; }
    char from[PATH_MAX], to[PATH_MAX]; snprintf(from, sizeof from, "%s/save", J.replay_dir); snprintf(to, sizeof to, "%s/save", J.current_segment);
    if (exists(from)) copy_dir(from, to);
    {
        snprintf(from, sizeof from, "%s/meta.txt", J.replay_dir);
        size_t len = 0; char *meta = read_all(from, &len);
        FILE *m = open_in(J.current_segment, "meta.txt", "w");
        if (m) { if (meta) fwrite(meta, 1, len, m); fprintf(m, "continued_from=%s\ncontinued_at=%llu\n", J.replay_dir, (unsigned long long)upto); fclose(m); }
        free(meta);
    }
    J.input = open_in(J.current_segment, "input.txt", "wb");
    J.events = open_in(J.current_segment, "events.txt", "wb");
    J.segment_start = J.replay_begin;
    for (size_t i = 0; i < J.nsamples; i++) if (J.samples[i].at < upto) write_sample(J.segment_start + J.samples[i].at, &J.samples[i].s);
    FILE *ev = open_in(J.replay_dir, "events.txt", "r");
    char line[8192];
    while (ev && fgets(line, sizeof line, ev)) { chomp(line); if (strtoull(line, NULL, 10) < upto && J.events) fprintf(J.events, "%s\n", line); }
    if (ev) fclose(ev);
    J.segment_index = 1; J.has_initial_snapshot = 1; J.committed_since_segment = 0; J.recording = 1; on_update();
    flush_files();
    keep_newest_game(root_dir());
    LOG("input journal: the player has control; recording on in %s", J.session_dir);
}
static void hand_over(const char *reason) {
    uint64_t upto = J.frame - J.replay_begin;
    finish_replay(reason);
    J.handed_over = 1;
    keep_replay_save();
    continue_recording(upto);
}
static void prepare_replay(const char *segment) {
    char meta[PATH_MAX]; snprintf(meta, sizeof meta, "%s/meta.txt", segment);
    FILE *in = open_in(segment, "input.txt", "r");
    if (!exists(meta) || !in) { if (in) fclose(in); LOG("[RED] input journal: %s is not a recorded segment", segment); return; }
    strlcpy(J.replay_dir, segment, sizeof J.replay_dir);
    replay_clear();
    J.replay_start_frame = ~0ull; J.replay_player_input = 0; J.replay_running = 0; J.replay_begin = 0;
    J.hold_frames = J.total_held = J.total_skipped = 0;
    J.anchors_hit = J.anchors_missed = J.commits_same = J.commits_diff = 0;
    char line[8192];
    while (fgets(line, sizeof line, in)) {
        unsigned long long at; char b[512], l[64], r[64];
        if (sscanf(line, "%llu %511s %63s %63s", &at, b, l, r) != 4) continue;
        sample_t s = {parse_buttons(b), 0, 0, 0, 0};
        sscanf(l, "%d;%d", &s.lx, &s.ly); sscanf(r, "%d;%d", &s.rx, &s.ry);
        if (J.nsamples == J.capsamples) { size_t cap = J.capsamples ? J.capsamples * 2 : 1024; line_t *nv = realloc(J.samples, cap * sizeof *nv); if (!nv) break; J.samples = nv; J.capsamples = cap; }
        J.samples[J.nsamples++] = (line_t){at, s};
    }
    fclose(in);
    J.replay_length = J.nsamples ? J.samples[J.nsamples - 1].at + 1 : 0;
    FILE *ev = open_in(segment, "events.txt", "r");
    while (ev && fgets(line, sizeof line, ev)) {
        chomp(line);
        event_t e = {0}; char *p = line, *q;
        e.at = strtoull(p, &q, 10); if (q == p) continue; p = q; while (*p == ' ' || *p == '\t') p++;
        char kind = *p; if (!kind || (p[1] && p[1] != ' ' && p[1] != '\t')) continue; p++;
        if (kind == 'C') {
            while (*p == ' ' || *p == '\t') p++;
            e.commit = 1; e.hash = strtoull(p, &q, 16); p = q; while (*p && *p != ' ' && *p != '\t') p++;
            while (*p == ' ' || *p == '\t') p++;
            e.file = strdup(p); events_push(&J.commits, &e);
        } else if (kind == 'R') {
            e.offset = strtoll(p, &q, 10); if (q == p) continue; p = q;
            e.size = strtoll(p, &q, 10); if (q == p) continue; p = q;
            while (*p == ' ' || *p == '\t') p++;
            e.file = strdup(p); events_push(&J.reads, &e);
        } else continue;
        if (e.at + 1 > J.replay_length) J.replay_length = e.at + 1;
    }
    if (ev) fclose(ev);
    J.next_read = J.next_commit = 0;

    // The segment's save in place (keeping a copy of the user's) before the game mounts it
    char recorded[PATH_MAX], save_dir[PATH_MAX], snapshot[PATH_MAX];
    read_meta(segment, "save_dir", recorded, sizeof recorded);
    snprintf(snapshot, sizeof snapshot, "%s/save", segment);
    if (recorded[0] && exists(snapshot)) {
        local_save_dir(recorded, save_dir, sizeof save_dir);
        J.replay_backup[0] = 0;
        if (exists(save_dir)) {
            char ts[32]; timestamp(ts, sizeof ts);
            snprintf(J.replay_backup, sizeof J.replay_backup, "%s/save_before_replay_%s", segment, ts);
            if (!copy_dir(save_dir, J.replay_backup)) { LOG("[RED] input journal: could not back up %s, not restoring the segment", save_dir); J.replay_backup[0] = 0; return; }
        }
        mkdirs(root_dir());
        char m[PATH_MAX]; marker_path(m, sizeof m);
        FILE *mf = fopen(m, "w");
        int ok = mf && fprintf(mf, "%s\n%s\n", save_dir, J.replay_backup) > 0;
        if (mf && fclose(mf)) ok = 0;
        if (!ok) { LOG("[RED] input journal: could not write %s, not restoring the segment", m); rm_all(J.replay_backup); J.replay_backup[0] = 0; return; }
        rm_all(save_dir);
        copy_dir(snapshot, save_dir);
        strlcpy(J.replay_save_dir, save_dir, sizeof J.replay_save_dir);
        LOG("input journal: restored the segment save into %s (yours is put back on exit)", save_dir);
    }

    char start[32], start_frame[32];
    read_meta(segment, "start", start, sizeof start);
    int from_boot = start[0] ? !strcmp(start, "boot") : (read_meta(segment, "start_frame", start_frame, sizeof start_frame), !strcmp(start_frame, "0"));
    J.replay_from_load = !strcmp(start, "load");
    const char *sf = env2("DM_REPLAY_START_FRAME", "EDEN_REPLAY_START_FRAME");
    if (sf && *sf) J.replay_start_frame = strtoull(sf, NULL, 10);
    else if (from_boot) J.replay_start_frame = 0;
    snprintf(J.replay_trigger, sizeof J.replay_trigger, "%s/start", segment);
    unlink(J.replay_trigger);
    J.replaying = 1; on_update();
    LOG("input journal: replaying %s (%llu frames, %zu input lines, %zu save reads, %zu commits)%s", segment, (unsigned long long)J.replay_length, J.nsamples,
        J.reads.n, J.commits.n, from_boot ? " from boot" : "; load the save in the game (or create the file 'start' in that folder)");
}
static void step_replay(void) {
    if (!J.replay_running) {
        J.takeover_prev = live_sample();
        J.replay_player_input |= !neutral(&J.takeover_prev);
        if (J.frame < J.replay_start_frame && !exists(J.replay_trigger)) return;
        start_replay_clock(J.frame - 1);   // recorded frame numbers count from 1 after the segment start
        while (J.next_read < J.reads.n && J.reads.v[J.next_read].at == 0 && J.replay_from_load) J.next_read++;   // the load's reads already happened
        LOG("input journal: replay started at guest frame %llu", (unsigned long long)J.frame);
    }
    sample_t now = live_sample();
    int took = took_over(&now, &J.takeover_prev);
    J.takeover_prev = now;
    if (took) { hand_over("taken over by the player"); return; }
    uint64_t at = J.frame - J.replay_begin;
    // Hold the input while the game has not yet reached the next recorded save read or commit
    const event_t *pending = J.next_read < J.reads.n ? &J.reads.v[J.next_read] : NULL;
    if (J.next_commit < J.commits.n && (!pending || J.commits.v[J.next_commit].at < pending->at)) pending = &J.commits.v[J.next_commit];
    if (pending && pending->at < at) {
        if (J.hold_frames < MAX_HOLD_FRAMES) { J.hold_frames++; J.total_held++; J.replay_begin += at - pending->at; at = pending->at; }
        else {
            J.anchors_missed++;
            LOG("input journal: replay: gave up waiting for the recorded save %s %s", pending->commit ? "commit" : "read of", pending->file);
            if (pending->commit) J.next_commit++; else J.next_read++;
            J.hold_frames = 0;
        }
    }
    if (at >= J.replay_length) { hand_over("finished"); return; }
    size_t lo = 0, hi = J.nsamples;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (J.samples[mid].at < at) lo = mid + 1; else hi = mid; }
    sample_t s = {0};
    if (lo < J.nsamples && J.samples[lo].at == at) s = J.samples[lo].s;
    apply_input(&s);
}

// ---------- Hooks ----------
int dm_journal_active(void) { return atomic_load_explicit(&g_on, memory_order_relaxed); }
int dm_journal_save_swapped(void) { return atomic_load(&g_swapped); }
int dm_journal_boot_time(int64_t *out) { if (!J.boot_set) return 0; *out = J.boot_time; return 1; }

void dm_journal_init(void) {
    pthread_mutex_lock(&J.mu);
    J.seed_set = J.boot_set = 0;
    const char *replay = replay_env();
    if (replay) {
        char v[64];
        if (read_meta(replay, "rng_seed", v, sizeof v) && *v) { J.rng_seed = (uint32_t)strtoul(v, NULL, 16); J.seed_set = 1; }
        if (read_meta(replay, "boot_time", v, sizeof v) && *v) { J.boot_time = strtoll(v, NULL, 10); J.boot_set = 1; }
    } else if (!env_on("DM_INPUT_JOURNAL", "EDEN_INPUT_JOURNAL")) { pthread_mutex_unlock(&J.mu); return; }
    else { J.rng_seed = arc4random(); J.seed_set = 1; J.boot_time = (int64_t)time(NULL); J.boot_set = 1; }
    J.latch = !env_zero("DM_INPUT_JOURNAL_LATCH", "EDEN_INPUT_JOURNAL_LATCH");
    // Guest time per frame fixed while recording or replaying, so time-based movement goes as far per frame in both runs: only for a game locked to its
    // frame rate (an adaptive one would slow down with the host), so it is asked for, not on by default
    J.frame_clock = env_on("DM_INPUT_JOURNAL_FRAME_CLOCK", "EDEN_INPUT_JOURNAL_FRAME_CLOCK");
    if (getenv("DM_DETCLOCK")) { J.frame_clock = 0; LOG("input journal: DM_DETCLOCK keeps its own guest clock and entropy"); }
    else { dm_guest_clock_rebase(); if (J.seed_set) dm_entropy_seed(J.rng_seed); }
    LOG("input journal: guest RNG seed %08X, boot time %lld%s", J.seed_set ? J.rng_seed : 0, (long long)(J.boot_set ? J.boot_time : 0), J.frame_clock ? ", frame clock" : "");
    pthread_mutex_unlock(&J.mu);
}
void dm_journal_start(void) {
    pthread_mutex_lock(&J.mu);
    J.title_id = DM_GAME_PROGRAM_ID;
    J.frame = 0; J.segment_index = 0; J.has_initial_snapshot = 0; J.committed_since_segment = 0; J.handed_over = 0;
    restore_pending_save();
    const char *replay = replay_env();
    if (replay) prepare_replay(replay);
    else if (env_on("DM_INPUT_JOURNAL", "EDEN_INPUT_JOURNAL") && J.title_id) {
        char ts[32]; timestamp(ts, sizeof ts);
        snprintf(J.session_dir, sizeof J.session_dir, "%s/%s_%016llX", root_dir(), ts, (unsigned long long)J.title_id);
        mkdirs(J.session_dir);
        if (!exists(J.session_dir)) LOG("[RED] input journal: cannot create %s: this run is not recorded", J.session_dir);
        else {
            keep_newest_game(root_dir());
            J.recording = 1; on_update();
            begin_segment(NULL);
            LOG("input journal: recording to %s", J.session_dir);
        }
    }
    pthread_mutex_unlock(&J.mu);
    if (dm_journal_active()) atexit(dm_journal_stop);
}
void dm_journal_stop(void) {
    pthread_mutex_lock(&J.mu);
    close_segment();
    if (J.recording || J.replaying || J.replay_running) clear_input();
    J.recording = 0;
    if (J.replay_running) finish_replay("stopped");
    if (J.replay_save_dir[0]) {
        if (restore_user_save(J.replay_save_dir, J.replay_backup)) { char m[PATH_MAX]; marker_path(m, sizeof m); unlink(m); }
        J.replay_save_dir[0] = J.replay_backup[0] = 0;
    }
    J.replaying = 0; J.replay_running = 0; on_update();
    replay_clear();
    pthread_mutex_unlock(&J.mu);
}
void dm_journal_frame(void) {
    if (!dm_journal_active()) return;
    pthread_mutex_lock(&J.mu);
    J.frame++;
    uint64_t period = dm_guest_frame_period_ns();
    if (J.frame_clock && period && (J.recording || J.replaying)) dm_frame_clock_advance(period);
    if (J.replaying) {
        step_replay();
        if (J.replaying || !J.recording) { pthread_mutex_unlock(&J.mu); return; }   // unless the player took over: this frame is recorded below
    }
    if (J.recording) {
        sample_t s = live_sample();
        // Latched for the coming frame, so the game sees exactly what is recorded
        if (J.latch) apply_input(&s);
        if (!neutral(&s)) write_sample(J.frame, &s);
        if (J.unflushed && J.frame - J.last_flush_frame >= FLUSH_FRAMES) flush_files();   // a kill loses at most a second
    }
    pthread_mutex_unlock(&J.mu);
}
void dm_journal_save_opened(const char *dir) {
    if (!dm_journal_active()) return;
    pthread_mutex_lock(&J.mu);
    if (J.recording && !J.has_initial_snapshot && J.current_segment[0]) {
        J.has_initial_snapshot = 1;
        // seg_000 starts at boot; its save is the one the game loads, copied before any write
        char to[PATH_MAX]; snprintf(to, sizeof to, "%s/save", J.current_segment);
        if (dir_size(dir) <= MAX_SNAPSHOT_BYTES) copy_dir(dir, to);
        FILE *m = open_in(J.current_segment, "meta.txt", "a");
        if (m) { fprintf(m, "save_dir=%s\n", dir); fclose(m); }
    }
    pthread_mutex_unlock(&J.mu);
}
void dm_journal_save_committed(const char *dir) {
    if (!dm_journal_active()) return;
    pthread_mutex_lock(&J.mu);
    if (J.replay_running) {
        if (J.next_commit >= J.commits.n) LOG("input journal: replay: unexpected save commit at guest frame %llu", (unsigned long long)J.frame);
        else {
            size_t index = J.next_commit, n; fhash_t *h = hash_files(dir, &n);
            const event_t *expected = &J.commits.v[index];
            if (combine_hashes(h, n) == expected->hash) { J.commits_same++; LOG("input journal: replay: commit %zu at guest frame %llu: save identical", index, (unsigned long long)J.frame); }
            else {
                J.commits_diff++;
                char differing[2048] = "";
                for (size_t i = 0; i < n; i++) {
                    char kv[PATH_MAX + 24]; snprintf(kv, sizeof kv, "%s=%016llx", h[i].name, (unsigned long long)h[i].hash);
                    if (!strstr(expected->file, kv)) { strlcat(differing, h[i].name, sizeof differing); strlcat(differing, " ", sizeof differing); }
                }
                LOG("input journal: replay: commit %zu at guest frame %llu: save DIFFERS (%s)", index, (unsigned long long)J.frame, differing);
                if (dir_size(dir) <= MAX_SNAPSHOT_BYTES) { char to[PATH_MAX]; snprintf(to, sizeof to, "%s/replay_commit_%03zu", J.replay_dir, index); copy_dir(dir, to); }
            }
            free_hashes(h, n);
            anchor_replay(&J.commits, &J.next_commit, index);
        }
    } else if (J.recording) {
        J.has_initial_snapshot = 1; J.committed_since_segment = 1; J.last_commit_frame = J.frame;
        size_t n; fhash_t *h = hash_files(dir, &n);
        event_t e = {.at = J.frame - J.segment_start, .commit = 1, .hash = combine_hashes(h, n), .file = format_hashes(h, n)};
        write_event(&e);
        free(e.file); free_hashes(h, n);
        LOG("input journal: save committed at guest frame %llu", (unsigned long long)J.frame);
    }
    pthread_mutex_unlock(&J.mu);
}
static int same_read(const event_t *e, const char *file, int64_t off, int64_t size) { return !e->commit && e->offset == off && e->size == size && !strcmp(e->file, file); }
void dm_journal_save_read(const char *dir, const char *file, int64_t off, int64_t size, int64_t file_size) {
    if (!dm_journal_active()) return;
    pthread_mutex_lock(&J.mu);
    if (J.replaying) {
        if (!J.replay_running) {
            // A load segment starts when the player loads the save again; the reads the game makes on its own at boot happen before any input
            if (J.replay_from_load && J.replay_player_input && J.reads.n && same_read(&J.reads.v[0], file, off, size)) {
                start_replay_clock(J.frame); J.next_read = 1;
                LOG("input journal: replay started on the save load at guest frame %llu", (unsigned long long)J.frame);
            }
        } else {
            size_t end = J.next_read + ANCHOR_LOOKAHEAD < J.reads.n ? J.next_read + ANCHOR_LOOKAHEAD : J.reads.n, i;
            for (i = J.next_read; i < end; i++) if (same_read(&J.reads.v[i], file, off, size)) { anchor_replay(&J.reads, &J.next_read, i); break; }
            if (i == end) LOG("input journal: replay: unexpected save read %s at guest frame %llu", file, (unsigned long long)J.frame);
        }
    } else if (J.recording) {
        if (J.committed_since_segment && size > 0 && off + size >= file_size && J.frame >= J.last_commit_frame + RELOAD_GUARD_FRAMES) {
            begin_segment(dir);
            LOG("input journal: save %s loaded at guest frame %llu, segment %llu", file, (unsigned long long)J.frame, (unsigned long long)(J.segment_index - 1));
        }
        event_t e = {.at = J.frame - J.segment_start, .offset = off, .size = size, .file = (char *)file};
        write_event(&e);
    }
    pthread_mutex_unlock(&J.mu);
}
