// net_crowd: the server records every player at 30 Hz (--crowd-record); a window replays it at any camera (--crowd-replay)

#define NC_REC_MAGIC 0x4352434eu   // "NCRC"
#define NC_REC_FRAME 0x5246434eu   // "NCFR"
#define NC_REC_VERSION 1u
#define NC_REC_ID_BITS 17u         // ghost ids: max_ghosts must fit
#define NC_REC_POS_BITS 19u        // mm from the plaza's corner: 448,000 < 2^19
#define NC_REC_FACING_BITS 9u      // the facing's angle, 512 steps
#define NC_REC_EVERY 2u            // ticks between frames: 30 Hz at the 60 Hz tick
#define NC_REC_ORIGIN_MM 224000    // NC_PLAZA_HALF in mm

typedef struct NcRecHeader {
  u32 magic, version, tick_us, capacity;
  f32 map_half; // the server map's square, so a replay computes the same cells
  u32 max_ghosts; // every id is below it
} NcRecHeader;

typedef struct NcRecFrame {
  u32 magic;
  u32 tick;
  u32 count;
  u32 second; // the server report this frame is the map of (0: none)
} NcRecFrame;

// 12 bytes a player: the look's seed, then id, x, z and facing packed in 64 bits
typedef struct NcRecPlayer {
  u32 seed;
  u32 lo, hi;
} NcRecPlayer;

static_assert(sizeof(NcRecPlayer) == 12, "a recorded player is 12 bytes");
static_assert(NC_REC_ID_BITS + 2 * NC_REC_POS_BITS + NC_REC_FACING_BITS == 64, "the packed fields fill 64 bits");

force_inline NcRecPlayer nc_rec_pack(u32 id, u32 seed, v3 pos, v2 facing) {
  u64 x = (u64)(u32)((i32)net_q_f32(pos.x, NC_GRID) + NC_REC_ORIGIN_MM);
  u64 z = (u64)(u32)((i32)net_q_f32(pos.z, NC_GRID) + NC_REC_ORIGIN_MM);
  f32 a = m_atan2f(facing.x, facing.y) / (2.0f * M_PI) + 0.5f;
  u64 f = (u64)((u32)(a * (f32)(1u << NC_REC_FACING_BITS) + 0.5f) & ((1u << NC_REC_FACING_BITS) - 1));
  u64 bits = (u64)id | x << NC_REC_ID_BITS | z << (NC_REC_ID_BITS + NC_REC_POS_BITS) |
             f << (NC_REC_ID_BITS + 2 * NC_REC_POS_BITS);
  return (NcRecPlayer){.seed = seed, .lo = (u32)bits, .hi = (u32)(bits >> 32)};
}

force_inline u32 nc_rec_id(NcRecPlayer p) { return p.lo & ((1u << NC_REC_ID_BITS) - 1); }

force_inline v3 nc_rec_pos(NcRecPlayer p) {
  u64 bits = (u64)p.hi << 32 | p.lo;
  u32 m = (1u << NC_REC_POS_BITS) - 1;
  i32 x = (i32)((bits >> NC_REC_ID_BITS) & m) - NC_REC_ORIGIN_MM;
  i32 z = (i32)((bits >> (NC_REC_ID_BITS + NC_REC_POS_BITS)) & m) - NC_REC_ORIGIN_MM;
  return (v3){net_dq_f32((u32)x, NC_GRID), 0.0f, net_dq_f32((u32)z, NC_GRID)};
}

force_inline f32 nc_rec_yaw(NcRecPlayer p) {
  u64 bits = (u64)p.hi << 32 | p.lo;
  u32 f = (u32)(bits >> (NC_REC_ID_BITS + 2 * NC_REC_POS_BITS));
  return ((f32)f / (f32)(1u << NC_REC_FACING_BITS) - 0.5f) * 2.0f * M_PI;
}

typedef struct NcRecorder {
  OsAppendFile *file;
  u8 *buf; // a frame's header and players
  u32 max_players;
  NetTick last;
  b32 any;
  u64 bytes;
  u32 frames;
  u32 *write_us; // [NC_REPORT_FRAMES] this second's frames: gather and write
  u32 write_count;
} NcRecorder;

// main lane: --crowd-record <path>
hz_internal b32 nc_record_open(NcRecorder *r, Allocator *a, const char *path, u32 capacity, u32 max_ghosts, f32 map_half) {
  if (max_ghosts > (1u << NC_REC_ID_BITS)) {
    log_error("nc record: % ghosts do not fit the record's % id bits", fmt_u32(max_ghosts), fmt_u32(NC_REC_ID_BITS));
    return false;
  }
  r->file = os_append_open(path);
  if (!r->file) {
    log_error("nc record: cannot open %", fmt_cstr(path));
    return false;
  }
  r->max_players = capacity;
  r->buf = ALLOC_ARRAY(a, u8, sizeof(NcRecFrame) + (u64)capacity * sizeof(NcRecPlayer));
  r->write_us = ALLOC_ARRAY(a, u32, NC_REPORT_FRAMES);
  NcRecHeader h = {.magic = NC_REC_MAGIC, .version = NC_REC_VERSION, .tick_us = NC_TICK_US, .capacity = capacity,
                   .map_half = map_half, .max_ghosts = max_ghosts};
  if (!os_append_write(r->file, &h, sizeof h))
    return false;
  r->bytes = sizeof h;
  log_info("nc record: % at % Hz, % B a player", fmt_cstr(path), fmt_u32(60 / NC_REC_EVERY), fmt_u32((u32)sizeof(NcRecPlayer)));
  return true;
}

// main lane, after the frame's ticks: a frame every NC_REC_EVERY ticks, and on a report's frame the map's own state
hz_internal void nc_record_frame(NcRecorder *r, EcsWorld *w, NetTick tick, u32 second) {
  if (!r->file || (!second && r->any && net_tick_since(tick, r->last) < (i32)NC_REC_EVERY))
    return;
  u64 at = (u64)os_ticks_to_us(os_time_now());
  NcRecFrame *fr = (NcRecFrame *)r->buf;
  NcRecPlayer *p = (NcRecPlayer *)(r->buf + sizeof(NcRecFrame));
  u32 n = 0;
  ecs_query_foreach(ecs_query(w, EcsIn(NcBody), EcsIn(NcLook), EcsIn(NetGhost), EcsWith(NcInput)), it) {
    const NcBody *b = ecs_iter_get_component(&it, NcBody, 0);
    const NcLook *l = ecs_iter_get_component(&it, NcLook, 1);
    const NetGhost *g = ecs_iter_get_component(&it, NetGhost, 2);
    for (i32 k = 0; k < it.count && n < r->max_players; k++)
      p[n++] = nc_rec_pack(g[k].id, l[k].name_seed, b[k].pos, b[k].facing);
  }
  *fr = (NcRecFrame){.magic = NC_REC_FRAME, .tick = tick, .count = n, .second = second};
  u32 len = (u32)sizeof(NcRecFrame) + n * (u32)sizeof(NcRecPlayer);
  if (!os_append_write(r->file, r->buf, len)) {
    log_error("nc record: write failed after % frames; recording stops", fmt_u32(r->frames));
    os_append_close(r->file);
    r->file = NULL;
    return;
  }
  r->bytes += len;
  r->frames++;
  r->last = tick;
  r->any = true;
  if (r->write_count < NC_REPORT_FRAMES)
    r->write_us[r->write_count++] = (u32)((u64)os_ticks_to_us(os_time_now()) - at);
}

// main lane, with the server report: this second's frames and their cost
hz_internal void nc_record_report(NcRecorder *r) {
  if (!r->file)
    return;
  u32 worst = 0, sum = 0;
  for (u32 i = 0; i < r->write_count; i++) {
    worst = MAX(worst, r->write_us[i]);
    sum += r->write_us[i];
  }
  log_info("nc record: % frames this second, gather+write us mean % max %; % MB so far", fmt_u32(r->write_count),
           fmt_u32(r->write_count ? sum / r->write_count : 0), fmt_u32(worst), fmt_f64_p((f64)r->bytes / (1024.0 * 1024.0), 1));
  r->write_count = 0;
}

// ---- the replay ----

// a player as one frame saw it, by ghost id
typedef struct NcReplaySeen {
  u32 seed;
  u32 stamp; // the frame number it was in
  v3 pos;
  f32 yaw;
} NcReplaySeen;

typedef struct NcReplayBuf {
  NcRecFrame fr;
  u32 no;             // the frame's number, from 1
  u32 *ids;           // [capacity] in file order
  NcReplaySeen *seen; // [max_ghosts]
} NcReplayBuf;

#define NC_REPLAY_CHUNK MB(8) // a streamed read; the buffer holds four, at least two frames

typedef struct NcReplay {
  const char *path;
  u64 offset, size; // offset: the file's bytes consumed
  NcRecHeader h;
  // the file streamed in chunks: data[lo, hi) are the bytes from `offset`; one read in flight lands at data[hi]
  u8 *data;
  u32 cap, lo, hi;
  u64 read_at; // the file offset of data[hi]
  OsFileOp *op;
  b32 verified; // GPU-less: every frame read
  NcReplayBuf *a, *b; // the frames either side of the shown time
  NcRecPlayer *buf;   // [capacity] a frame as read
  EcsEntity *ent;     // [tiles * max_ghosts] the body drawn for an id in a tile
  u32 *ent_seed;      // [tiles * max_ghosts] whose body it is
  u32 frames;
  NetTick first_tick;
  b32 ended;
  u32 copies; // --replay-copies K: the crowd tiled K x K, for the renderer's capacity only; logged in every report
  u32 bodies;
} NcReplay;

// the read in flight landed (or wait for it); then the next chunk starts, compacting the buffer first
hz_internal b32 nc_replay_pump(NcReplay *r, b32 wait) {
  if (r->op) {
    OsFileReadState st = os_check_read_file(r->op);
    while (wait && st == OS_FILE_READ_STATE_IN_PROGRESS)
      st = os_wait_read_file(r->op, 1000);
    if (st == OS_FILE_READ_STATE_IN_PROGRESS)
      return true;
    i32 got = os_get_file_range_bytes(r->op);
    os_release_file_op(r->op);
    r->op = NULL;
    if (st != OS_FILE_READ_STATE_COMPLETED || got <= 0) {
      log_error("nc replay: reading % failed at byte %", fmt_cstr(r->path), fmt_u64(r->read_at));
      return false;
    }
    r->hi += (u32)got;
    r->read_at += (u64)got;
  }
  if (r->read_at >= r->size)
    return true;
  if (r->cap - r->hi < NC_REPLAY_CHUNK) {
    mem_move(r->data, r->data + r->lo, r->hi - r->lo);
    r->hi -= r->lo;
    r->lo = 0;
  }
  u32 len = (u32)MIN((u64)MIN(NC_REPLAY_CHUNK, r->cap - r->hi), r->size - r->read_at);
  if (len)
    r->op = os_start_read_file_range(r->path, r->read_at, len, r->data + r->hi);
  return len == 0 || r->op != NULL;
}

// `len` bytes from the consumed offset in the buffer, waiting on the stream; false at the end or on a read error
hz_internal b32 nc_replay_have(NcReplay *r, u32 len) {
  while (r->hi - r->lo < len) {
    if (!r->op && r->read_at >= r->size)
      return false;
    if (!nc_replay_pump(r, true))
      return false;
  }
  return true;
}

// the next frame into `into`; false at the end. A map frame logs its cells' hash, to compare with the server's
hz_internal b32 nc_replay_next(NcReplay *r, NcReplayBuf *into) {
  NcRecFrame fr;
  if (!nc_replay_have(r, sizeof fr))
    return false;
  mem_cpy(&fr, r->data + r->lo, sizeof fr);
  u32 len = (u32)sizeof fr + fr.count * (u32)sizeof(NcRecPlayer);
  if (fr.magic != NC_REC_FRAME || fr.count > r->h.capacity || !nc_replay_have(r, len))
    return false;
  mem_cpy(r->buf, r->data + r->lo + sizeof fr, fr.count * sizeof(NcRecPlayer));
  r->lo += len;
  r->offset += len;
  nc_replay_pump(r, false);
  into->fr = fr;
  into->no = ++r->frames;
  u64 hash = 0;
  for (u32 i = 0; i < fr.count; i++) {
    NcRecPlayer p = r->buf[i];
    u32 id = nc_rec_id(p);
    if (id >= r->h.max_ghosts) {
      log_error("nc replay: frame % holds ghost id % past the recording's % ghosts", fmt_u32(into->no), fmt_u32(id),
                fmt_u32(r->h.max_ghosts));
      return false;
    }
    NcReplaySeen *s = &into->seen[id];
    *s = (NcReplaySeen){.seed = p.seed, .stamp = into->no, .pos = nc_rec_pos(p), .yaw = nc_rec_yaw(p)};
    into->ids[i] = id;
    if (fr.second)
      hash = nc_map_hash_add(hash, nc_map_cell(s->pos, r->h.map_half));
  }
  if (fr.second)
    log_info("nc replay: second % tick % dots % map hash %", fmt_u32(fr.second), fmt_u32(fr.tick), fmt_u32(fr.count),
             fmt_hex(hash));
  return true;
}

hz_internal NcReplayBuf *nc_replay_buf(Allocator *a, u32 capacity, u32 max_ghosts) {
  NcReplayBuf *b = ALLOC(a, NcReplayBuf);
  b->ids = ALLOC_ARRAY(a, u32, capacity);
  b->seen = ALLOC_ARRAY(a, NcReplaySeen, max_ghosts);
  return b;
}

// main lane: --crowd-replay <path>; GPU-less, it reads to the end and logs the map frames' hashes
hz_internal b32 nc_replay_open(NcReplay *r, Allocator *a, const char *path) {
  r->path = path;
  r->size = os_file_size(path);
  r->cap = 4 * NC_REPLAY_CHUNK;
  r->data = ALLOC_ARRAY(a, u8, r->cap);
  b32 head = nc_replay_pump(r, false) && nc_replay_have(r, sizeof r->h);
  if (head)
    mem_cpy(&r->h, r->data, sizeof r->h);
  if (!head || r->h.magic != NC_REC_MAGIC || r->h.version != NC_REC_VERSION || r->h.max_ghosts > (1u << NC_REC_ID_BITS) ||
      r->h.capacity > r->h.max_ghosts) {
    log_error("nc replay: % is not a net_crowd recording", fmt_cstr(path));
    return false;
  }
  if (!nc_arg_u32("--replay-copies", 1, &r->copies) || !r->copies) {
    log_error("nc replay: --replay-copies takes a number");
    return false;
  }
  r->offset = sizeof r->h;
  r->lo = sizeof r->h;
  r->buf = ALLOC_ARRAY(a, NcRecPlayer, r->h.capacity);
  r->a = nc_replay_buf(a, r->h.capacity, r->h.max_ghosts);
  r->b = nc_replay_buf(a, r->h.capacity, r->h.max_ghosts);
  u64 slots = (u64)r->copies * r->copies * r->h.max_ghosts;
  r->ent = ALLOC_ARRAY(a, EcsEntity, slots);
  r->ent_seed = ALLOC_ARRAY(a, u32, slots);
  log_info("nc replay: % (% MB, capacity %, map half % m, % x % copies)", fmt_cstr(path),
           fmt_f64_p((f64)r->size / (1024.0 * 1024.0), 1), fmt_u32(r->h.capacity), fmt_f32_p(r->h.map_half, 1),
           fmt_u32(r->copies), fmt_u32(r->copies));
  if (!nc_replay_next(r, r->a))
    return false;
  // drawn, it starts at the first player; the GPU-less check reads every frame
  while (hz_has_gpu() && !r->a->fr.count)
    if (!nc_replay_next(r, r->a))
      return false;
  r->first_tick = r->a->fr.tick;
  r->ended = !nc_replay_next(r, r->b);
  return true;
}

// GPU-less, main lane, a frame at a time so no frame stalls the lanes: read on, logging every map frame's hash
hz_internal void nc_replay_verify(NcReplay *r) {
  if (r->verified)
    return;
  for (u32 k = 0; k < 256 && !r->verified; k++)
    r->verified = !nc_replay_next(r, r->a);
  if (r->verified)
    log_info("nc replay: % frames, % of % MB read", fmt_u32(r->frames), fmt_f64_p((f64)r->offset / (1024.0 * 1024.0), 1),
             fmt_f64_p((f64)r->size / (1024.0 * 1024.0), 1));
}

// a tile's offset when the crowd is tiled for the capacity measurement
force_inline v3 nc_replay_tile(const NcReplay *r, u32 c) {
  f32 side = 2.0f * r->h.map_half + 4.0f, mid = 0.5f * (f32)(r->copies - 1);
  return (v3){((f32)(c % r->copies) - mid) * side, 0.0f, ((f32)(c / r->copies) - mid) * side};
}

force_inline f32 nc_replay_frame_s(const NcReplay *r, const NcReplayBuf *b) {
  return (f32)((f64)net_tick_since(b->fr.tick, r->first_tick) * (f64)r->h.tick_us / 1e6);
}

// main lane, before the frame's flush: the recording at `t` seconds, each body between the two frames around it
hz_internal void nc_replay_frame(NcReplay *r, EcsWorld *w, f32 t) {
  u32 tiles = r->copies * r->copies;
  while (!r->ended && nc_replay_frame_s(r, r->b) <= t) {
    NcReplayBuf *old = r->a;
    r->a = r->b;
    r->b = old;
    // bodies of players the new older frame lacks go; the old frame's ids are still intact here
    for (u32 i = 0; i < old->fr.count; i++) {
      u32 id = old->ids[i];
      const NcReplaySeen *s = &r->a->seen[id];
      if (s->stamp == r->a->no && s->seed == old->seen[id].seed)
        continue;
      for (u32 c = 0; c < tiles; c++) {
        EcsEntity *e = &r->ent[(u64)c * r->h.max_ghosts + id];
        if (ecs_entity_is_alive(w, *e))
          ecs_entity_destroy(w, *e);
        *e = (EcsEntity){0};
      }
    }
    r->ended = !nc_replay_next(r, r->b);
  }
  f32 ta = nc_replay_frame_s(r, r->a), tb = nc_replay_frame_s(r, r->b);
  f32 alpha = r->ended || tb <= ta ? 0.0f : m_clampf((t - ta) / (tb - ta), 0.0f, 1.0f);
  u32 bodies = 0;
  for (u32 i = 0; i < r->a->fr.count; i++) {
    u32 id = r->a->ids[i];
    const NcReplaySeen *sa = &r->a->seen[id], *sb = &r->b->seen[id];
    b32 both = !r->ended && sb->stamp == r->b->no && sb->seed == sa->seed;
    v3 pos = both ? v3_lerp(sa->pos, sb->pos, alpha) : sa->pos;
    f32 dyaw = both ? sb->yaw - sa->yaw : 0.0f;
    dyaw = dyaw > M_PI ? dyaw - 2.0f * M_PI : dyaw < -M_PI ? dyaw + 2.0f * M_PI : dyaw;
    f32 yaw = sa->yaw + dyaw * alpha;
    v2 facing = {m_sinf(yaw), m_cosf(yaw)};
    for (u32 c = 0; c < tiles; c++) {
      u64 k = (u64)c * r->h.max_ghosts + id;
      v3 at = v3_add(pos, nc_replay_tile(r, c));
      if (!ecs_entity_is_alive(w, r->ent[k]) || r->ent_seed[k] != sa->seed) {
        if (ecs_entity_is_alive(w, r->ent[k]))
          ecs_entity_destroy(w, r->ent[k]);
        u32 h = sa->seed;
        r->ent[k] = ecs_entity_create(w);
        r->ent_seed[k] = h;
        ecs_component_set(w, r->ent[k], NcLook, {.name_seed = h, .colour = (h >> 8) % NC_COLOURS, .body = (h >> 16) & 1});
        ecs_component_set(w, r->ent[k], NcBody, {.pos = at, .facing = facing});
      } else {
        *ecs_component_get_mut(w, r->ent[k], NcBody) = (NcBody){.pos = at, .facing = facing};
      }
      bodies++;
    }
  }
  r->bodies = bodies;
}
