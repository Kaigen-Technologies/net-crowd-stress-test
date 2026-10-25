---
title: Sim Run
description: how do I check a gameplay or tuning change in seconds instead of playing it — a headless run of the game's own simulation step at a fixed dt over seeds, with beats (game minutes things happen at), wall time and a determinism digest; sim_run, SimRunDesc, sim_run_beat, SimRunReportBlobAsset
resource: src/gameplay/sim_run/sim_run.h
tags: [gameplay, testing, simulation, pacing, determinism, headless]
updated: 2026-10-15
---

# What it provides

- `sim_run(&run, &desc, alloc)`: every lane, lock step. For each seed: `begin` builds the game, `ready` (optional)
  gates loading frames, `step` runs one frame of exactly `dt` until it returns `SIM_RUN_DONE` or `max_seconds` of
  game time pass, `end` frees it. Returns a `SimRunReportBlobAsset` (the same pointer on every lane).
- `sim_run_beat(run, name)`: from `step`, main lane. The first call per seed stamps the game time at the END of the
  current frame (`run->time` is advanced before `step`). Beats are named moments: "reached the door", "boss down".
- The report, per beat: the game seconds on every seed, `reached`, `median`/`min`/`max`, and the wall seconds; per
  seed: `finished`, `game_seconds`, `load_seconds`, `wall_seconds`, `speed` (game s per wall s), `frames`, `digest`.
- `digest` (optional) is sampled once per game second and folded into `digest_trail`: two runs of one seed agree on
  the trail only if they agreed at every sampled second, not just at the end.
- `sim_run_log` (one line per beat and per seed), `sim_run_write` (the report as `.hzt`), `sim_run_find_beat`.
- Loading frames from `ready` step with `dt` 0, count no frames and no game time, and are reported apart
  (`load_seconds`); a seed not ready after `ready_timeout_seconds` (0 = 60 wall s) is reported unfinished.

# The contract a game meets

`sim_run` drives a simulation step; the game has to have one. Split the frame in two:

1. **The simulation step**: everything that changes game state. Callable without a window, a GPU or audio: the
   world, physics, characters, AI, progression, saves. It takes its controls as data (a `PlayerControls`-shaped
   struct or resource), never from `InputSystem`, so a bot can drive it.
2. **Presentation**: renderer, UI, audio, avatars, particles. The step only queues what presentation consumes (a
   sound request, a "this chunk's mesh changed" flag), and the app's frame drains it after the step.

The app then calls the step from its frame (OS input -> controls -> step -> presentation), and a test calls the
same step from `sim_run`. One step, two callers: a change to it is a change to both. The starting template
(`templates/starting-template`, `game_sim.h`) is the worked example of the split.

# Example usage

```c
// tests/my_tests.c: a bot walks to a point; the beat says when it got there
hz_internal void *my_begin(SimRun *run, u64 seed, void *user) {
  MyBot *bot = ALLOC(run->alloc, MyBot);
  bot->sim = game_sim_create(seed);
  return bot;
}
hz_internal b32 my_ready(void *game, void *user) { return game_sim_ready(((MyBot *)game)->sim); }
hz_internal SimRunStatus my_step(SimRun *run, void *game, f32 dt, void *user) {
  MyBot *bot = game;
  local_shared PlayerControls controls;
  local_shared SimRunStatus status;
  if (is_main_thread()) {
    controls = my_bot_think(bot);  // the bot reads state, writes controls
    if (my_bot_arrived(bot))
      sim_run_beat(run, STR_FROM_CSTR("arrived"));
    status = my_bot_arrived(bot) ? SIM_RUN_DONE : SIM_RUN_CONTINUE;
  }
  lane_sync();                      // every lane steps with the main lane's controls
  game_sim_frame(bot->sim, &controls, dt);
  return status;
}

void test_my_route(void) {
  local_shared SimRun run;          // shared by the lanes, owned by the caller: no globals
  local_shared SimRunDesc desc;
  /* main lane: arena, seeds, desc = {.dt = 1.0f / 60.0f, .max_seconds = 60, .begin = ..., .ready = ...,
     .step = ..., .end = ..., .digest = ...} */
  lane_sync();
  SimRunReportBlobAsset *r = sim_run(&run, &desc, &alloc);
  // main lane, after a lane_sync: read and assert on r
}
```

Run it from the project: `hz\hzbuild test run --filter my_route` (the project's `hzproject.hzt` needs
`test_roots 1 tests` and a `test_app_register` in `tests/`). For speed: `hz\hzbuild test run --release --lanes 1
--filter my_route`.

# Gotchas

- **A time scale is not a fixed step.** Running the app at 10x scales dt and changes what a frame does (bigger
  physics steps, missed thresholds). `sim_run` keeps dt exact and simply runs more frames per wall second.
- **dt changes per-frame logic.** Anything that runs once per frame rather than per fixed step (a cooldown counted
  in frames, a "one hit per frame" rule, a damp) behaves differently at 1/60 and 1/144. To compare with a recorded
  playthrough, run at the frame rate it was recorded at; to design, run at the target rate.
- **Fewer lanes are faster for a small world.** Each frame pays lane syncs; a world with a few hundred entities has
  little to split. Measured on a ~20 game-minute run: 1 lane debug was about 2x faster than 8 lanes, and
  `--release --lanes 1` about 15x faster than the 8-lane debug default (~590x real time). Beats were identical
  across lane counts and configs. Measure your own game before choosing.
- **Determinism is a test, not an assumption.** Run one seed twice and compare `digest_trail`. Unordered work
  (lane-dependent order of float sums, per-lane RNG, wall time) breaks it; a digest catches it at the first second
  it diverges.
- **Presentation stays out of the step.** A renderer, audio or UI call inside the step fails or crashes headless,
  and makes the step's cost depend on the GPU. Queue a request; drain it in the app.
- **A headless world still needs its resources.** Blocks look up resources the app normally provides: the character
  avatar system and the scene loader need an `AssetSystem` (`ecs_assets_init(w, NULL)`), the camera needs an
  `AppMemory` (any canvas size). Destroy the `AssetSystem` yourself after the world: the world holds a reference.
- **Scenes load over frames.** `scene_load_file` returns before the entities exist. Gate with `ready` (frames step
  at dt 0) rather than counting frames, so a slow disk never moves a beat.
- **`mesh_builder_finish` with a NULL renderer** builds the colliders only; a simulation that generates meshes
  keeps its collision in the step and the upload in presentation ([mesh-builder.md](mesh-builder.md)).
- **`step` needs no syncs of its own.** The driver agrees on stop/continue through its own votes; a step that
  works only on the main lane and returns `SIM_RUN_CONTINUE` elsewhere is fine. Only the main lane's return
  value counts.
- **Assert after the run's last `lane_sync`.** `test_assert_*` on the main lane before the other lanes are past the
  run leaves them in a sync the main lane never reaches.

# Does NOT

Render, read OS input, scale time, write a game's bot, or judge a report against a reference: the test asserts.
