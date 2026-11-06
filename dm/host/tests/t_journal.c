// The input journal (hle_journal.c, docs/nx2): CityHash64 as Eden's, a recorded run's files, its replay to the end and on with the player's input,
// a take-over, the save put back after an interrupted replay, and a PC recording's CRLF meta
#include "../hle_journal.c"

// hle_hid.c / hle_os.c, as the journal sees them
static uint64_t t_btn; static int32_t t_axes[4];
static int t_ov_on; static uint64_t t_ov_btn; static int32_t t_ov_axes[4];
static int t_rebased, t_seeded; static uint32_t t_seed;
void dm_hid_live_sample(uint64_t *b, int32_t a[4]) { *b = t_btn; memcpy(a, t_axes, sizeof t_axes); }
void dm_hid_override(int on, uint64_t b, const int32_t a[4]) { t_ov_on = on; t_ov_btn = b; memcpy(t_ov_axes, a, sizeof t_ov_axes); }
void dm_guest_clock_rebase(void) { t_rebased = 1; }
void dm_frame_clock_advance(uint64_t p) { (void)p; }
uint64_t dm_guest_frame_period_ns(void) { return 16666667; }
void dm_entropy_seed(uint32_t s) { t_seeded = 1; t_seed = s; }

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char g_tmp[PATH_MAX], g_save[PATH_MAX];
static void put(const char *path, const char *text) { FILE *f = fopen(path, "w"); fputs(text, f); fclose(f); }
static int has(const char *path, const char *text) { size_t n; char *s = read_all(path, &n); int r = s && strstr(s, text); free(s); return r; }
static int file_is(const char *path, const char *text) { size_t n; char *s = read_all(path, &n); int r = s && !strcmp(s, text); free(s); return r; }
static void press(uint64_t b, int32_t lx) { t_btn = b; t_axes[0] = lx; }
static void frames(int n) { for (int i = 0; i < n; i++) dm_journal_frame(); }
static void reset(void) {   // a fresh host process
    dm_journal_stop();
    J.title_id = 0; J.frame = 0; J.handed_over = 0; J.session_dir[0] = J.current_segment[0] = 0;
    t_ov_on = 0; press(0, 0); unsetenv("DM_REPLAY");
}
static void newest_session(char *out, size_t n) {   // the newest <timestamp>_<title> folder in the replay root
    DIR *d = opendir(g_tmp); struct dirent *e; char best[256] = "";
    while ((e = readdir(d))) if (e->d_name[0] != '.' && strchr(e->d_name, '_') && strcmp(e->d_name, best) > 0) strlcpy(best, e->d_name, sizeof best);
    closedir(d); snprintf(out, n, "%s/%s", g_tmp, best);
}

static void cityhash(void) {   // Google's CityHash64 1.1 (pypi cityhash), the version Eden's common/cityhash.cpp is
    const char *fox = "The quick brown fox jumps over the lazy dog";
    char x33[34], fox3[130]; memset(x33, 'x', 33); x33[33] = 0; snprintf(fox3, sizeof fox3, "%s%s%s", fox, fox, fox);
    CHECK(dm_cityhash64("", 0) == 0x9ae16a3b2f90404full, "empty");
    CHECK(dm_cityhash64("a", 1) == 0xb3454265b6df75e3ull, "1 byte");
    CHECK(dm_cityhash64("hello world", 11) == 0x588fb7478bd6b01bull, "11 bytes");
    CHECK(dm_cityhash64("0123456789abcdef0", 17) == 0x6deb36b4a021a909ull, "17 bytes");
    CHECK(dm_cityhash64(x33, 33) == 0x3efce6b6a0cfe809ull, "33 bytes");
    CHECK(dm_cityhash64(fox3, 129) == 0x3d47fac12f9d663eull, "129 bytes");
    CHECK(dm_cityhash64_seeds("abc", 3, 1, 2) == 0x9f37d4106a646448ull, "with seeds");
}

static void record(char *session, size_t n) {
    setenv("DM_INPUT_JOURNAL", "1", 1);
    dm_journal_init(); dm_journal_start();
    CHECK(dm_journal_active() && t_rebased && t_seeded, "recording: journal on, clock rebased, entropy seeded");
    int64_t bt; CHECK(dm_journal_boot_time(&bt) && bt > 1700000000, "recording: a boot date");
    newest_session(session, n);
    dm_journal_save_opened(g_save);
    frames(2);                       // frames 1, 2: nothing held, no lines
    press(1, 0); frames(1);          // frame 3: A
    CHECK(t_ov_on && t_ov_btn == 1, "the latch gives the game what is recorded");
    press(1 | 1 << 13, 20000); frames(1);   // frame 4: A + DUp, stick right
    press(0, 0); frames(1);
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/data.bin", g_save);
    dm_journal_save_read(g_save, "/data.bin", 0, 4, 4);   // the boot's read: no load segment before a commit
    frames(5);                       // frame 10
    put(p, "two!"); dm_journal_save_committed(g_save);
    frames(70);                      // frame 80: past the reload guard
    dm_journal_save_read(g_save, "/data.bin", 0, 4, 4);   // the player loads the save: segment 1
    press(1 << 1, 0); frames(1);     // frame 81 = load segment frame 1: B
    press(0, 0); frames(3);
    reset();
    put(p, "user");                  // the user's save as it is now
}

static void check_recording(const char *session) {
    char p[PATH_MAX], tid[32]; snprintf(tid, sizeof tid, "title_id=%016llX\n", (unsigned long long)DM_GAME_PROGRAM_ID);
    snprintf(p, sizeof p, "%s/seg_000/meta.txt", session);
    CHECK(has(p, tid) && has(p, "segment=0\nstart=boot\nstart_frame=0\nstarted=") && has(p, "rng_seed=") && has(p, "boot_time=") && has(p, "save_dir="), "seg_000 meta");
    snprintf(p, sizeof p, "%s/seg_000/input.txt", session);
    CHECK(file_is(p, "3 KEY_A; 0;0 0;0\n4 KEY_A;KEY_DUP; 20000;0 0;0\n"), "seg_000 input lines");
    snprintf(p, sizeof p, "%s/seg_000/save/data.bin", session);
    CHECK(file_is(p, "one!"), "seg_000 save: as the game found it");
    snprintf(p, sizeof p, "%s/seg_000/events.txt", session);
    char want[256]; snprintf(want, sizeof want, "5 R 0 4 /data.bin\n10 C %016llx data.bin=%016llx\n",
        (unsigned long long)dm_cityhash64_seeds("data.bin", 8, 0, dm_cityhash64("two!", 4)), (unsigned long long)dm_cityhash64("two!", 4));
    CHECK(file_is(p, want), "seg_000 events");
    snprintf(p, sizeof p, "%s/seg_001/meta.txt", session);
    CHECK(has(p, "segment=1\nstart=load\nstart_frame=80\n") && has(p, "save_dir="), "seg_001: a load segment");
    snprintf(p, sizeof p, "%s/seg_001/input.txt", session);
    CHECK(file_is(p, "1 KEY_B; 0;0 0;0\n"), "seg_001 input");
    snprintf(p, sizeof p, "%s/seg_001/events.txt", session);
    CHECK(file_is(p, "0 R 0 4 /data.bin\n"), "seg_001 events");
    snprintf(p, sizeof p, "%s/seg_001/save/data.bin", session);
    CHECK(file_is(p, "two!"), "seg_001 save");
}

static void replay_to_end(const char *session) {
    char seg[PATH_MAX], p[PATH_MAX], m[PATH_MAX]; snprintf(seg, sizeof seg, "%s/seg_000", session); marker_path(m, sizeof m);
    snprintf(p, sizeof p, "%s/data.bin", g_save);
    setenv("DM_REPLAY", seg, 1); t_seeded = 0;
    dm_journal_init();
    char mv[64]; read_meta(seg, "rng_seed", mv, sizeof mv);
    CHECK(t_seeded && t_seed == (uint32_t)strtoul(mv, NULL, 16), "replay: the recording's seed");
    dm_journal_start();
    CHECK(dm_journal_save_swapped() && exists(m) && file_is(p, "one!"), "replay: the segment's save in place, the marker written");
    sleep(1);   // a session folder named after a later second than the recording's
    uint64_t seen[6] = {0};
    for (int f = 1; f <= 5; f++) { dm_journal_frame(); seen[f] = t_ov_on ? t_ov_btn : ~0ull; }
    CHECK(seen[1] == 0 && seen[2] == 0 && seen[3] == 1 && seen[4] == (1 | 1 << 13) && seen[5] == 0, "replay: the recorded input on its frames");
    CHECK(t_ov_axes[0] == 0, "stick released");
    dm_journal_save_read(g_save, "/data.bin", 0, 4, 4);
    frames(5);
    put(p, "two!"); dm_journal_save_committed(g_save);
    CHECK(J.commits_same == 1 && J.anchors_hit == 2, "replay: the read and the commit on time, the save identical");
    frames(2);
    CHECK(!J.replaying && J.handed_over && J.recording && !t_ov_btn, "replay finished: the player has control (the latch now gives the live input), recording on");
    CHECK(!dm_journal_save_swapped() && !exists(m) && file_is(p, "two!"), "replay finished: the replayed save stays");
    char cont[PATH_MAX]; newest_session(cont, sizeof cont); snprintf(p, sizeof p, "%s/seg_000/meta.txt", cont);
    CHECK(strcmp(cont, session) && has(p, "continued_from=") && has(p, "continued_at=11\n") && has(p, "start=boot"), "continued recording: meta");
    snprintf(p, sizeof p, "%s/seg_000/input.txt", cont);
    CHECK(file_is(p, "3 KEY_A; 0;0 0;0\n4 KEY_A;KEY_DUP; 20000;0 0;0\n"), "continued recording: the replayed lines");
    char broot[PATH_MAX]; backups_dir(broot, sizeof broot);
    DIR *d = opendir(broot); int nb = 0; struct dirent *e; while (d && (e = readdir(d))) nb += e->d_name[0] != '.'; if (d) closedir(d);
    CHECK(nb == 1, "the user's save from before is in replay_save_backups");
    reset();
}

static void take_over(const char *session) {
    char seg[PATH_MAX], p[PATH_MAX], m[PATH_MAX]; snprintf(seg, sizeof seg, "%s/seg_000", session); marker_path(m, sizeof m);
    snprintf(p, sizeof p, "%s/data.bin", g_save); put(p, "mine");
    setenv("DM_REPLAY", seg, 1); setenv("DM_INPUT_JOURNAL", "0", 1);
    press(1 << 2, 0);   // X held before the replay starts: not a take-over
    dm_journal_init(); dm_journal_start(); frames(3);
    CHECK(J.replaying && t_ov_on && t_ov_btn == 1, "held input does not take over");
    press(1 << 2 | 1 << 3, 0); frames(1);   // Y pressed
    CHECK(!J.replaying && !t_ov_on && !J.recording, "a press takes over (no recording on with DM_INPUT_JOURNAL=0)");
    CHECK(!exists(m) && file_is(p, "one!"), "take-over: the replayed save stays");
    reset();
}

static void interrupted(const char *session) {
    char seg[PATH_MAX], p[PATH_MAX], m[PATH_MAX]; snprintf(seg, sizeof seg, "%s/seg_001", session); marker_path(m, sizeof m);
    snprintf(p, sizeof p, "%s/data.bin", g_save); put(p, "mine");
    setenv("DM_REPLAY", seg, 1);
    dm_journal_init(); dm_journal_start();
    CHECK(J.replay_from_load && exists(m) && file_is(p, "two!"), "load segment: its save in place");
    frames(5);
    CHECK(!J.replay_running && !t_ov_on, "load segment: waits for the save load");
    press(1, 0); frames(1); press(0, 0);
    dm_journal_save_read(g_save, "/data.bin", 0, 4, 4);
    CHECK(J.replay_running, "load segment: starts on the load after player input");
    // the host dies here: nothing puts the save back
    J.replaying = J.replay_running = 0; J.replay_save_dir[0] = 0; on_update(); unsetenv("DM_REPLAY");
    dm_journal_init(); dm_journal_start();
    CHECK(!exists(m) && file_is(p, "mine"), "the next start puts the user's save back");
    reset();
}

static void pc_meta(void) {
    char seg[PATH_MAX], p[PATH_MAX]; snprintf(seg, sizeof seg, "%s/pc/seg_000", g_tmp); mkdirs(seg);
    snprintf(p, sizeof p, "%s/meta.txt", seg);
    put(p, "title_id=0100000000010000\r\nsegment=0\r\nstart=boot\r\nstart_frame=0\r\nrng_seed=0BADF00D\r\nboot_time=1700000000\r\nsave_dir=C:\\eden\\nand\\user\\save\\0\\X\r\n");
    snprintf(p, sizeof p, "%s/input.txt", seg); put(p, "1 KEY_ZL;KEY_PLUS; -32767;1 0;0\r\n");
    snprintf(p, sizeof p, "%s/events.txt", seg); put(p, "");
    setenv("DM_REPLAY", seg, 1); t_seeded = 0;
    dm_journal_init(); int64_t bt = 0;
    CHECK(t_seed == 0x0BADF00D && dm_journal_boot_time(&bt) && bt == 1700000000, "PC meta: seed and boot date through CRLF");
    dm_journal_start(); frames(1);
    CHECK(t_ov_on && t_ov_btn == (1 << 8 | 1 << 10) && t_ov_axes[0] == -32767 && t_ov_axes[1] == 1, "PC input line");
    reset();
}

static void per_game(void) {   // sessions and save backups: the newest 10 of each game, another game's untouched
    char dir[PATH_MAX], p[PATH_MAX]; snprintf(dir, sizeof dir, "%s/../keep", g_tmp); mkdirs(dir);
    for (int i = 0; i < 12; i++) { snprintf(p, sizeof p, "%s/20260101-0000%02d_0100F2C0115B6000", dir, i); mkdirs(p); }
    snprintf(p, sizeof p, "%s/20260101-000011_0100F2C0115B6000_2", dir); mkdirs(p);
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/20250101-0000%02d_0100f2c0115b7000", dir, i); mkdirs(p); }
    uint64_t was = J.title_id; J.title_id = 0x0100F2C0115B6000ull; keep_newest_game(dir); J.title_id = was;
    int a = 0, b = 0; DIR *d = opendir(dir); struct dirent *e;
    while ((e = readdir(d))) { if (strstr(e->d_name, "6000")) a++; if (strstr(e->d_name, "7000")) b++; }
    closedir(d);
    snprintf(p, sizeof p, "%s/20260101-000002_0100F2C0115B6000", dir);
    CHECK(a == 10 && b == 3 && !exists(p), "per game: 10 of this game kept (%d), the other's 3 untouched (%d)", a, b);
}

int main(void) {
    snprintf(g_tmp, sizeof g_tmp, "/tmp/t_journal.%d/replay", getpid()); mkdirs(g_tmp);
    snprintf(g_save, sizeof g_save, "/tmp/t_journal.%d/save", getpid()); mkdirs(g_save);
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/data.bin", g_save); put(p, "one!");
    setenv("DM_REPLAY_ROOT", g_tmp, 1); setenv("DM_SAVE", g_save, 1); unsetenv("DM_DETCLOCK");
    cityhash();
    char session[PATH_MAX]; record(session, sizeof session);
    check_recording(session);
    replay_to_end(session);
    take_over(session);
    interrupted(session);
    pc_meta();
    per_game();
    char top[PATH_MAX]; snprintf(top, sizeof top, "/tmp/t_journal.%d", getpid()); if (!fails) rm_all(top);
    printf("input journal: %d / %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
