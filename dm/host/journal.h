// Input journal (hle_journal.c): every run recorded so it can be replayed, in the same files and with the same behaviour as Eden's input journal on the PC
// (chocokage/PokeDinner-a-Eglantine-Fork docs/nx2). docs/development.md "Recording and replaying a run" describes the switches.
#pragma once
#include <stddef.h>
#include <stdint.h>

void dm_journal_init(void);       // first thing in the host: pins the RNG seed and the boot date, rebases the guest clock (before guest code runs)
void dm_journal_start(void);      // before the game mounts its save: puts back a save an interrupted replay swapped out, then records or prepares a replay
void dm_journal_stop(void);       // at exit (atexit, graceful exit): closes the files, puts the user's save back after a replay that was not taken over
void dm_journal_frame(void);      // once per guest present (dm_guest_frame_tick), before the game's next frame reads its input
int dm_journal_active(void);      // recording or replaying
int dm_journal_save_swapped(void);   // a replay has the user's save swapped out (no cloud sync then)
int dm_journal_boot_time(int64_t *out);   // the pinned boot date (POSIX seconds) while recording or replaying
// The game's own save (hle_fs.c): dir is the committed save directory, file the path inside the save as the game opened it ("/save.bin")
void dm_journal_save_opened(const char *dir);
void dm_journal_save_read(const char *dir, const char *file, int64_t off, int64_t size, int64_t file_size);
void dm_journal_save_committed(const char *dir);

// hle_os.c: the guest clock and entropy the journal pins
void dm_guest_clock_rebase(void);             // the guest clock starts at 0 now, as on a freshly booted console
void dm_frame_clock_advance(uint64_t period_ns);   // the frame clock: guest time per frame is the frame's period
uint64_t dm_guest_frame_period_ns(void);      // how long the game shows its latest frame (NVN; 0 = unknown)
void dm_entropy_seed(uint32_t seed);          // guest entropy from a fixed seed

// hle_hid.c: player 1's live input and the input the game is given instead
void dm_hid_live_sample(uint64_t *buttons, int32_t axes[4]);
void dm_hid_override(int on, uint64_t buttons, const int32_t axes[4]);

// CityHash64 (Google, MIT), byte-compatible with Eden's common/cityhash.cpp
uint64_t dm_cityhash64(const char *s, size_t len);
uint64_t dm_cityhash64_seeds(const char *s, size_t len, uint64_t seed0, uint64_t seed1);
