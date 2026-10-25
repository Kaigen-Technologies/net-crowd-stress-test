// Headless fast simulation: a game's own simulation step at a fixed dt, as fast as the CPU allows, over seeds.
//
// Usage: every lane calls sim_run(&run, &desc, alloc) with the same SimRun; `step` calls sim_run_beat on moments.
//
// Does NOT: render, read OS input, scale time (every frame is exactly dt), or judge the report against a reference.
#pragma once

#include "lib/blob_asset.h"
#include "lib/memory.h"
#include "lib/reflect.h"
#include "lib/string.h"
#include "lib/typedefs.h"

#define SIM_RUN_MAX_BEATS 64
#define SIM_RUN_MAX_SEEDS 64

typedef enum SimRunStatus {
  SIM_RUN_CONTINUE = 0,
  SIM_RUN_DONE, // the game reached its end; the seed stops here
} SimRunStatus;

typedef struct SimRun SimRun;

typedef struct SimRunDesc {
  CString name;
  f32 dt;               // game seconds per frame; 0 -> 1/60
  f32 max_seconds;      // game seconds before a seed stops unfinished; > 0
  const u64 *seeds;
  u32 seed_count;       // 1..SIM_RUN_MAX_SEEDS
  f32 progress_seconds; // logs a progress line every this many game seconds; 0 = none
  void *user;
  // main lane: the game for this seed, or NULL when it cannot be built (the seed is reported unfinished)
  void *(*begin)(SimRun *run, u64 seed, void *user);
  // main lane, optional: false while the game still loads; those frames step with dt 0 and count no game time
  b32 (*ready)(void *game, void *user);
  f32 ready_timeout_seconds; // wall seconds a seed may load before it is reported unfinished; 0 -> 60
  // all lanes, lock step: one frame of exactly dt; the main lane's result decides
  SimRunStatus (*step)(SimRun *run, void *game, f32 dt, void *user);
  // main lane: frees the game
  void (*end)(SimRun *run, void *game, void *user);
  // main lane, optional: the game state folded into one value, sampled once per game second for determinism checks
  u64 (*digest)(void *game, void *user);
} SimRunDesc;

// shared by every lane of a run; the caller owns it (local_shared in a test) and never touches its fields mid-run
struct SimRun {
  const SimRunDesc *desc;
  Allocator *alloc;
  void *game;
  u32 seed_index;
  u64 frame;
  f64 time;        // game seconds into this seed
  b32 vote[2];     // the main lane's decisions, alternating so it never overwrites one a lane has yet to read
  u32 beat_count;
  String *beat_names; // SIM_RUN_MAX_BEATS
  f32 *beat_time;     // [seed * SIM_RUN_MAX_BEATS + beat], game seconds; negative = not reached
  f32 *beat_wall;     // the same, wall seconds since the seed began
  u64 *digest;        // per seed: the last sample
  u64 *digest_trail;  // per seed: every sample folded together, so any divergence along the way shows
  f32 *game_seconds;
  f32 *wall_seconds;
  f32 *load_seconds;
  u64 *frames;
  b32 *finished;
  u64 wall_start;
  f64 next_digest;
  f64 next_progress;
  struct SimRunReportBlobAsset *report;
  u32 report_size;
};

HZ_STRUCT_TYPEDEF(SimRunBeatStat) {
  StringBlob name;
  BlobArray(f32) seconds; // per seed, game seconds; negative = never reached
  BlobArray(f32) wall;    // per seed, wall seconds; negative = never reached
  u32 reached;            // seeds that reached it
  f32 median;             // game seconds over the seeds that reached it; negative when none did
  f32 min;
  f32 max;
};

HZ_STRUCT_TYPEDEF(SimRunSeedStat) {
  u64 seed;
  b32 finished;      // SIM_RUN_DONE before max_seconds
  f32 game_seconds;
  f32 load_seconds;  // wall seconds before `ready`, not part of wall_seconds
  f32 wall_seconds;
  f32 speed;         // game seconds per wall second
  u64 frames;
  u64 digest;        // the last sample; 0 without a digest callback
  u64 digest_trail;  // every sample folded together
};

HZ_STRUCT_TYPEDEF(SimRunReportBlobAsset) {
  BlobAssetHeader header;
  StringBlob name;
  f32 dt;
  f32 max_seconds;
  BlobArray(SimRunSeedStat) seeds;
  BlobArray(SimRunBeatStat) beats; // in the order first seen
};

// all lanes, lock step: every seed in turn; the report (from `alloc`, the same pointer on every lane), NULL on a bad desc
SimRunReportBlobAsset *sim_run(SimRun *run, const SimRunDesc *desc, Allocator *alloc);

// main lane, inside `step`: `name` happened; the first call per seed stamps the current game time
void sim_run_beat(SimRun *run, String name);

// main lane: the report as log lines (one per beat, one per seed)
void sim_run_log(const SimRunReportBlobAsset *r);

// main lane: the report as a .hzt text asset at `path`; false when it cannot be written
b32 sim_run_write(const SimRunReportBlobAsset *r, CString path);

// main lane: the stat of beat `name`, or NULL
const SimRunBeatStat *sim_run_find_beat(const SimRunReportBlobAsset *r, String name);
