#include "sim_run.h"
#include "gameplay/sim_run/sim_run.gen.h"
#include "lib/hash.h"
#include "lib/hztext.h"
#include "lib/string_builder.h"
#include "lib/thread_context.h"
#include "os/os.h"

#define SIM_RUN_DEFAULT_DT (1.0f / 60.0f)

hz_internal f32 sim_run_wall_since(u64 start) { return (f32)NS_TO_SECS(os_time_diff(os_time_now(), start)); }

hz_internal b32 sim_run_desc_valid(const SimRunDesc *d) {
  if (!d || !d->begin || !d->step || !d->end || !d->seeds) {
    log_error("sim_run: begin, step, end and seeds are required");
    return false;
  }
  if (d->seed_count == 0 || d->seed_count > SIM_RUN_MAX_SEEDS) {
    log_error("sim_run: seed_count % is outside 1..%", fmt_u32(d->seed_count), fmt_u32(SIM_RUN_MAX_SEEDS));
    return false;
  }
  if (!(d->max_seconds > 0.0f) || d->dt < 0.0f) {
    log_error("sim_run: max_seconds must be > 0 and dt >= 0");
    return false;
  }
  return true;
}

void sim_run_beat(SimRun *run, String name) {
  // the beat table is written by one lane
  assert(is_main_thread());
  u32 b = 0;
  while (b < run->beat_count && !str_equal(run->beat_names[b], name))
    b++;
  if (b == run->beat_count) {
    if (run->beat_count == SIM_RUN_MAX_BEATS) {
      log_error("sim_run: more than % distinct beats, '%' dropped", fmt_u32(SIM_RUN_MAX_BEATS), fmt_str(name));
      return;
    }
    run->beat_names[b] = str_from_cstr_with_len_alloc(name.value, name.len, run->alloc);
    run->beat_count++;
  }
  u32 slot = run->seed_index * SIM_RUN_MAX_BEATS + b;
  if (run->beat_time[slot] < 0.0f) {
    run->beat_time[slot] = (f32)run->time;
    run->beat_wall[slot] = sim_run_wall_since(run->wall_start);
  }
}

// main lane: the seeds and beats as one blob, from `alloc`
hz_internal void sim_run_build_report(SimRun *run) {
  const SimRunDesc *d = run->desc;
  u32 n = d->seed_count;
  BlobAssetBuilder b = blob_builder_begin_hint(KB(4) + run->beat_count * (n * 8 + 64) + n * 64);
  SimRunReportBlobAsset *r = blob_builder_alloc(&b, SimRunReportBlobAsset);
  r->dt = d->dt > 0.0f ? d->dt : SIM_RUN_DEFAULT_DT;
  r->max_seconds = d->max_seconds;
  CString name = d->name ? d->name : "sim_run";
  blob_builder_string(&b, &r->name, name, cstr_len(name));
  SimRunSeedStat *seeds = blob_builder_array(&b, &r->seeds, SimRunSeedStat, n);
  for (u32 s = 0; s < n; s++)
    seeds[s] = (SimRunSeedStat){.seed = d->seeds[s],
                                .finished = run->finished[s],
                                .game_seconds = run->game_seconds[s],
                                .load_seconds = run->load_seconds[s],
                                .wall_seconds = run->wall_seconds[s],
                                .speed = run->wall_seconds[s] > 0.0f ? run->game_seconds[s] / run->wall_seconds[s] : 0.0f,
                                .frames = run->frames[s],
                                .digest = run->digest[s],
                                .digest_trail = run->digest_trail[s]};
  SimRunBeatStat *beats = blob_builder_array(&b, &r->beats, SimRunBeatStat, run->beat_count);
  tctx_temp_allocator_scope(ta, b.temp.arena) {
    f32 *sorted = ALLOC_ARRAY(&ta.allocator, f32, n);
    for (u32 i = 0; i < run->beat_count; i++) {
      SimRunBeatStat *st = &beats[i];
      blob_builder_string(&b, &st->name, run->beat_names[i].value, run->beat_names[i].len);
      f32 *secs = blob_builder_array(&b, &st->seconds, f32, n);
      f32 *wall = blob_builder_array(&b, &st->wall, f32, n);
      u32 reached = 0;
      for (u32 s = 0; s < n; s++) {
        f32 t = run->beat_time[s * SIM_RUN_MAX_BEATS + i];
        secs[s] = t;
        wall[s] = run->beat_wall[s * SIM_RUN_MAX_BEATS + i];
        if (t < 0.0f)
          continue;
        // insertion into the sorted prefix: n is at most SIM_RUN_MAX_SEEDS
        u32 j = reached++;
        while (j > 0 && sorted[j - 1] > t) {
          sorted[j] = sorted[j - 1];
          j--;
        }
        sorted[j] = t;
      }
      st->reached = reached;
      st->median = reached == 0 ? -1.0f
                                : (reached & 1u ? sorted[reached / 2] : 0.5f * (sorted[reached / 2 - 1] + sorted[reached / 2]));
      st->min = reached ? sorted[0] : -1.0f;
      st->max = reached ? sorted[reached - 1] : -1.0f;
    }
  }
  blob_builder_fill_header(&b, &r->header, &SimRunReportBlobAsset_typeinfo);
  BlobBuilderResult(SimRunReportBlobAsset) out = {0};
  blob_builder_finish(&b, SimRunReportBlobAsset, run->alloc, &out);
  run->report = out.data;
  run->report_size = out.size;
}

// main lane: one digest sample, folded into the seed's trail
hz_internal void sim_run_sample_digest(SimRun *run) {
  if (!run->desc->digest)
    return;
  u64 h = run->desc->digest(run->game, run->desc->user);
  u32 s = run->seed_index;
  run->digest[s] = h;
  run->digest_trail[s] = fnv1a_hash_64_continue(run->digest_trail[s] ? run->digest_trail[s] : 0xcbf29ce484222325ull,
                                                (const char *)&h, (u32)sizeof(h));
}

// all lanes: the main lane's `value`, the same on every lane. Slots alternate: the main lane writes one only after every
// lane passed the sync that followed its last read, so a game step without syncs of its own cannot race it
hz_internal b32 sim_run_agree(SimRun *run, u32 *round, b32 value) {
  u32 slot = (*round)++ & 1u;
  if (is_main_thread())
    run->vote[slot] = value;
  lane_sync();
  return run->vote[slot];
}

SimRunReportBlobAsset *sim_run(SimRun *run, const SimRunDesc *desc, Allocator *alloc) {
  if (is_main_thread()) {
    *run = (SimRun){.alloc = alloc};
    if (sim_run_desc_valid(desc)) {
      u32 n = desc->seed_count;
      run->desc = desc;
      run->beat_names = ALLOC_ARRAY(alloc, String, SIM_RUN_MAX_BEATS);
      run->beat_time = ALLOC_ARRAY(alloc, f32, n * SIM_RUN_MAX_BEATS);
      run->beat_wall = ALLOC_ARRAY(alloc, f32, n * SIM_RUN_MAX_BEATS);
      for (u32 i = 0; i < n * SIM_RUN_MAX_BEATS; i++)
        run->beat_time[i] = run->beat_wall[i] = -1.0f;
      run->digest = ALLOC_ARRAY(alloc, u64, n);
      run->digest_trail = ALLOC_ARRAY(alloc, u64, n);
      run->game_seconds = ALLOC_ARRAY(alloc, f32, n);
      run->wall_seconds = ALLOC_ARRAY(alloc, f32, n);
      run->load_seconds = ALLOC_ARRAY(alloc, f32, n);
      run->frames = ALLOC_ARRAY(alloc, u64, n);
      run->finished = ALLOC_ARRAY(alloc, b32, n);
    }
  }
  lane_sync(); // every lane sees the same desc (NULL on a bad one) and tables
  if (!run->desc)
    return NULL;

  f32 dt = desc->dt > 0.0f ? desc->dt : SIM_RUN_DEFAULT_DT;
  u32 round = 0; // advances identically on every lane
  for (u32 s = 0; s < desc->seed_count; s++) {
    if (is_main_thread()) {
      run->seed_index = s;
      run->frame = 0;
      run->time = 0.0;
      run->next_digest = 1.0;
      run->next_progress = desc->progress_seconds > 0.0f ? desc->progress_seconds : -1.0;
      run->wall_start = os_time_now();
      run->game = desc->begin(run, desc->seeds[s], desc->user);
      if (!run->game)
        log_error("sim_run %: seed % could not begin", fmt_cstr(desc->name ? desc->name : ""), fmt_u64(desc->seeds[s]));
    }
    lane_sync(); // publishes the seed's game to every lane
    if (!run->game)
      continue;

    // loading frames: zero game time, so a slow disk never moves a beat
    if (desc->ready) {
      f32 timeout = desc->ready_timeout_seconds > 0.0f ? desc->ready_timeout_seconds : 60.0f;
      b32 loaded = false;
      for (;;) {
        b32 stop = false;
        if (is_main_thread()) {
          loaded = desc->ready(run->game, desc->user);
          stop = loaded || sim_run_wall_since(run->wall_start) > timeout;
          if (!loaded && stop)
            log_error("sim_run %: seed % not ready after % wall s", fmt_cstr(desc->name ? desc->name : ""), fmt_u64(desc->seeds[s]),
                      fmt_f32_p(timeout, 0));
        }
        if (sim_run_agree(run, &round, stop))
          break;
        desc->step(run, run->game, 0.0f, desc->user);
      }
      if (is_main_thread()) {
        run->load_seconds[s] = sim_run_wall_since(run->wall_start);
        run->wall_start = os_time_now();
      }
      // a seed that never loaded runs no frames
      if (sim_run_agree(run, &round, !loaded)) {
        if (is_main_thread()) {
          desc->end(run, run->game, desc->user);
          run->game = NULL;
        }
        lane_sync();
        continue;
      }
    }

    for (;;) {
      if (is_main_thread()) {
        // advanced first: a beat raised during the frame carries the time at its end
        run->frame++;
        run->time += (f64)dt;
      }
      SimRunStatus status = desc->step(run, run->game, dt, desc->user);
      if (is_main_thread()) {
        if (run->time >= run->next_digest) {
          sim_run_sample_digest(run);
          run->next_digest += 1.0;
        }
        if (run->next_progress > 0.0 && run->time >= run->next_progress) {
          log_info("sim_run %: seed % at % game min, % wall s", fmt_cstr(desc->name ? desc->name : ""), fmt_u64(desc->seeds[s]),
                   fmt_f32_p((f32)(run->time / 60.0), 1), fmt_f32_p(sim_run_wall_since(run->wall_start), 1));
          run->next_progress += (f64)desc->progress_seconds;
        }
        run->finished[s] = status == SIM_RUN_DONE;
      }
      b32 stop = is_main_thread() && (status == SIM_RUN_DONE || run->time >= (f64)desc->max_seconds);
      if (sim_run_agree(run, &round, stop))
        break;
    }

    if (is_main_thread()) {
      sim_run_sample_digest(run);
      run->game_seconds[s] = (f32)run->time;
      run->wall_seconds[s] = sim_run_wall_since(run->wall_start);
      run->frames[s] = run->frame;
      desc->end(run, run->game, desc->user);
      run->game = NULL;
    }
    lane_sync();
  }

  if (is_main_thread())
    sim_run_build_report(run);
  lane_sync(); // every lane returns the same report
  return run->report;
}

const SimRunBeatStat *sim_run_find_beat(const SimRunReportBlobAsset *r, String name) {
  Array(SimRunBeatStat) beats = blob_array_get(SimRunBeatStat, (BlobPtr *)&r->beats);
  for (u32 i = 0; i < beats.len; i++) {
    StringBlob *sb = &beats.items[i].name;
    if (str_equal((String){.value = string_blob_get(sb), .len = sb->len}, name))
      return &beats.items[i];
  }
  return NULL;
}

void sim_run_log(const SimRunReportBlobAsset *r) {
  Array(SimRunSeedStat) seeds = blob_array_get(SimRunSeedStat, (BlobPtr *)&r->seeds);
  Array(SimRunBeatStat) beats = blob_array_get(SimRunBeatStat, (BlobPtr *)&r->beats);
  log_info("sim_run %: % seeds at dt % s, up to % game min", fmt_cstr(string_blob_get((StringBlob *)&r->name)), fmt_u32(seeds.len),
           fmt_f32_p(r->dt, 5), fmt_f32_p(r->max_seconds / 60.0f, 1));
  arr_foreach_ptr(beats, SimRunBeatStat, b) {
    log_info("  beat %: median % min (min %, max %), reached by %/%", fmt_cstr(string_blob_get(&b->name)),
             fmt_f32_p(b->median / 60.0f, 2), fmt_f32_p(b->min / 60.0f, 2), fmt_f32_p(b->max / 60.0f, 2), fmt_u32(b->reached),
             fmt_u32(seeds.len));
  }
  arr_foreach_ptr(seeds, SimRunSeedStat, s) {
    log_info("  seed %: % game min in % wall s (%x real time, % frames), %", fmt_u64(s->seed), fmt_f32_p(s->game_seconds / 60.0f, 2),
             fmt_f32_p(s->wall_seconds, 2), fmt_f32_p(s->speed, 1), fmt_u64(s->frames),
             fmt_cstr(s->finished ? "finished" : "stopped at the budget"));
  }
}

b32 sim_run_write(const SimRunReportBlobAsset *r, CString path) {
  b32 ok = false;
  tctx_temp_allocator_scope(ta, NULL) {
    StringBuilder sb = sb_create(r->header.asset_size * 4 + KB(4), &ta.allocator);
    hzt_write_file(&sb, &SimRunReportBlobAsset_typeinfo, (void *)r, ASSET_VERSION);
    ok = os_write_file(path, (u8 *)sb_get(&sb), sb_length(&sb));
  }
  if (!ok)
    log_error("sim_run: could not write %", fmt_cstr(path));
  return ok;
}
