// net_crowd fidelity oracle: a client logs what it holds and draws each frame (--fidelity-log); --oracle diffs it with the recording

#define NC_FL_MAGIC 0x4c46434eu // "NCFL"
#define NC_FL_FRAME 0x4646434eu // "NCFF"
#define NC_FL_VERSION 2u
#define NC_FL_DRAWN 0x80000000u // a logged tick's flag: the body is drawn (its visual spawned and dressed)
#define NC_FL_BANDS 3
#define NC_FL_NEAR 16.0f // metres: the bands the acceptance terms use, 0-16, 16-64, 64+
#define NC_FL_MID 64.0f
#define NC_FL_ERR_BUCKETS 4096 // position error histogram: 1 cm buckets to 40.95 m, the last everything past
#define NC_FL_WARMUP_US 5000000u // the terms start 5 s after the log's first frame; the join's warm-up is reported apart

typedef struct NcFlHeader {
  u32 magic, version, tick_us, pad;
} NcFlHeader;

typedef struct NcFlFrame {
  u32 magic;
  u32 frame;    // from 1
  u32 tick;     // the interpolation tick the client shows the others at
  f32 fraction; // into the next tick
  v3 eye;       // the camera
  v3 look;      // the point it looks at
  u32 own_id;   // this client's avatar (0: none, a spectator)
  u32 own_seed;
  u32 shot;  // --frames-out: this frame's capture is frame_<shot - 1>.png (0: none)
  u32 count; // players held
  u64 wall_us;
  u64 rx_bytes; // snapshot and field bytes received so far
  // what changed since the last frame: NcFlEvent, NcFlTick and NcFlPos records follow, in that order
  u32 events, ticks, positions;
  u32 multi; // NcFlMulti records after the positions (0 in logs before them)
} NcFlFrame;

// held from this frame (val: the look's seed), let go, or drawn changed (val: drawn)
enum { NC_FL_ENTER = 1, NC_FL_LEAVE = 2, NC_FL_DRAW = 3 };
#define NC_FL_KIND_SHIFT 24

typedef struct NcFlEvent {
  u32 id_kind; // id | kind << NC_FL_KIND_SHIFT
  u32 val;
} NcFlEvent;

// a held player's newest state is from a new tick (near or field)
typedef struct NcFlTick {
  u32 id, tick; // id: the client's tier (NetcodeGhostTier) above NC_FL_TIER_SHIFT, 0 in older logs
} NcFlTick;
#define NC_FL_TIER_SHIFT 30u
#define NC_FL_TIER_IDS ((1u << NC_FL_TIER_SHIFT) - 1u)

// a held player's drawn position: within NC_FL_POS_NEAR of the view every frame, else 1 frame in NC_FL_POS_EVERY by id
typedef struct NcFlPos {
  u32 lo, hi; // the recording's packing: id, x, z, facing
} NcFlPos;

// a held player whose tick changed took more than one state since the last frame, in the same tier
typedef struct NcFlMulti {
  u32 id, received;
} NcFlMulti;

#define NC_FL_MULTI_MAX 64u // states a player can take between two frames (a tick each at most)

#define NC_FL_POS_NEAR 24.0f
#define NC_FL_POS_EVERY 16u

static_assert(sizeof(NcFlEvent) == 8 && sizeof(NcFlTick) == 8 && sizeof(NcFlPos) == 8 && sizeof(NcFlMulti) == 8,
              "log records are 8 bytes");

force_inline u32 nc_fl_frame_len(const NcFlFrame *fr) {
  return (u32)sizeof *fr + (fr->events + fr->ticks + fr->positions + fr->multi) * 8u;
}

typedef struct NcFidelityLog {
  OsAppendFile *file;
  u32 frames;
  u64 bytes;
  // what the last frame held, by id: the frame that listed it, its seed and its tick (with NC_FL_DRAWN)
  u32 *seen, *seed, *tick;
  u32 *rx;  // and its received count and tier
  u8 *tier;
  u32 *ids; // [NC_FL_IDS] the last frame's ids
  u32 n;
} NcFidelityLog;

#define NC_FL_IDS (1u << NC_REC_ID_BITS)

// main lane: --fidelity-log <path>
hz_internal b32 nc_fidelity_open(NcFidelityLog *l, Allocator *a, const char *path) {
  l->file = os_append_open(path);
  NcFlHeader h = {.magic = NC_FL_MAGIC, .version = NC_FL_VERSION, .tick_us = NC_TICK_US};
  if (!l->file || !os_append_write(l->file, &h, sizeof h)) {
    log_error("nc fidelity: cannot write %", fmt_cstr(path));
    return false;
  }
  l->seen = ALLOC_ARRAY(a, u32, NC_FL_IDS);
  l->seed = ALLOC_ARRAY(a, u32, NC_FL_IDS);
  l->tick = ALLOC_ARRAY(a, u32, NC_FL_IDS);
  l->rx = ALLOC_ARRAY(a, u32, NC_FL_IDS);
  l->tier = ALLOC_ARRAY(a, u8, NC_FL_IDS);
  l->ids = ALLOC_ARRAY(a, u32, NC_FL_IDS);
  l->bytes = sizeof h;
  log_info("nc fidelity: logging each frame's changes to what this client holds and draws to %", fmt_cstr(path));
  return true;
}

// main lane, after the frame's visuals and camera: what this client holds and draws now, as changes from the last frame
hz_internal void nc_fidelity_frame(NcFidelityLog *l, EcsWorld *w, NetcodeClient *c, const NetcodeStats *st, v3 eye, v3 look,
                                   u32 shot) {
  if (!l->file || !c)
    return;
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  u32 cap = 0;
  ecs_query_foreach(ecs_query(w, EcsWith(NcBody), EcsWith(NetGhost)), it) cap += (u32)it.count;
  NcRecPlayer *p = ALLOC_ARRAY_NO_ZERO(&ta.allocator, NcRecPlayer, cap + 1);
  u32 *ptick = ALLOC_ARRAY_NO_ZERO(&ta.allocator, u32, cap + 1), *prx = ALLOC_ARRAY_NO_ZERO(&ta.allocator, u32, cap + 1);
  u8 *ptier = ALLOC_ARRAY_NO_ZERO(&ta.allocator, u8, cap + 1);
  f32 frac = 0.0f;
  NcFlFrame fr = {.magic = NC_FL_FRAME, .frame = ++l->frames, .tick = netcode_client_interpolation_tick(c, &frac),
                  .eye = eye, .look = look, .shot = shot, .wall_us = (u64)os_ticks_to_us(os_time_now()),
                  .rx_bytes = st->client_link.snapshot_bytes + st->client_link.field_bytes};
  fr.fraction = frac;
  v3 view = look;
  u32 n = 0;
  for (u32 pass = 0; pass < 2; pass++) {
    EcsQuery *q = pass ? ecs_query(w, EcsIn(NcBody), EcsIn(NcLook), EcsIn(NetGhost), EcsNot(NcVisual))
                       : ecs_query(w, EcsIn(NcBody), EcsIn(NcLook), EcsIn(NetGhost), EcsIn(NcVisual));
    ecs_query_foreach(q, it) {
      const NcBody *b = ecs_iter_get_component(&it, NcBody, 0);
      const NcLook *lk = ecs_iter_get_component(&it, NcLook, 1);
      const NetGhost *g = ecs_iter_get_component(&it, NetGhost, 2);
      const NcVisual *v = pass ? NULL : ecs_iter_get_component(&it, NcVisual, 3);
      for (i32 k = 0; k < it.count && n < cap; k++) {
        if (!g[k].id || g[k].id >= NC_FL_IDS)
          continue;
        if (g[k].owner == 1) {
          fr.own_id = g[k].id;
          fr.own_seed = lk[k].name_seed;
          view = b[k].pos;
          continue;
        }
        NetTick tick = 0;
        ptier[n] = (u8)netcode_client_ghost_tier(c, g[k].id, &tick);
        prx[n] = netcode_client_ghost_received(c, g[k].id);
        p[n] = nc_rec_pack(g[k].id, lk[k].name_seed, b[k].pos, b[k].facing);
        ptick[n++] = (tick & ~NC_FL_DRAWN) | (v && v[k].tinted ? NC_FL_DRAWN : 0u);
      }
    }
  }
  fr.count = n;
  // every change is one record: at most an enter, a draw and a tick a player now, a leave each one before
  NcFlEvent *ev = ALLOC_ARRAY_NO_ZERO(&ta.allocator, NcFlEvent, 2 * n + l->n + 1);
  NcFlTick *tk = ALLOC_ARRAY_NO_ZERO(&ta.allocator, NcFlTick, n + 1);
  NcFlPos *ps = ALLOC_ARRAY_NO_ZERO(&ta.allocator, NcFlPos, n + 1);
  NcFlMulti *mu = ALLOC_ARRAY_NO_ZERO(&ta.allocator, NcFlMulti, n + 1);
  u32 en = 0, tn = 0, pn = 0, mn = 0, no = fr.frame;
  for (u32 i = 0; i < n; i++) {
    u32 id = nc_rec_id(p[i]), t = ptick[i];
    b32 was = l->seen[id] == no - 1 && l->seed[id] == p[i].seed;
    if (!was)
      ev[en++] = (NcFlEvent){id | (u32)NC_FL_ENTER << NC_FL_KIND_SHIFT, p[i].seed};
    if (!was || ((l->tick[id] ^ t) & NC_FL_DRAWN))
      ev[en++] = (NcFlEvent){id | (u32)NC_FL_DRAW << NC_FL_KIND_SHIFT, (t & NC_FL_DRAWN) != 0};
    if (!was || ((l->tick[id] ^ t) & ~NC_FL_DRAWN) || l->tier[id] != ptier[i])
      tk[tn++] = (NcFlTick){id | (u32)ptier[i] << NC_FL_TIER_SHIFT, t & ~NC_FL_DRAWN};
    // a ghost's count restarts with a new life: only a forward step counts
    if (was && ((l->tick[id] ^ t) & ~NC_FL_DRAWN) && l->tier[id] == ptier[i] && prx[i] > l->rx[id] + 1 &&
        prx[i] - l->rx[id] <= NC_FL_MULTI_MAX)
      mu[mn++] = (NcFlMulti){id, prx[i] - l->rx[id]};
    l->rx[id] = prx[i];
    l->tier[id] = ptier[i];
    v3 d = v3_sub(nc_rec_pos(p[i]), view);
    if (d.x * d.x + d.z * d.z <= NC_FL_POS_NEAR * NC_FL_POS_NEAR || (id + no) % NC_FL_POS_EVERY == 0)
      ps[pn++] = (NcFlPos){p[i].lo, p[i].hi};
    l->seen[id] = no;
    l->seed[id] = p[i].seed;
    l->tick[id] = t;
  }
  for (u32 i = 0; i < l->n; i++)
    if (l->seen[l->ids[i]] != no)
      ev[en++] = (NcFlEvent){l->ids[i] | (u32)NC_FL_LEAVE << NC_FL_KIND_SHIFT, 0};
  for (u32 i = 0; i < n; i++)
    l->ids[i] = nc_rec_id(p[i]);
  l->n = n;
  fr.events = en;
  fr.ticks = tn;
  fr.positions = pn;
  fr.multi = mn;
  b32 ok = os_append_write(l->file, &fr, sizeof fr) && (!en || os_append_write(l->file, ev, en * sizeof *ev)) &&
           (!tn || os_append_write(l->file, tk, tn * sizeof *tk)) && (!pn || os_append_write(l->file, ps, pn * sizeof *ps)) &&
           (!mn || os_append_write(l->file, mu, mn * sizeof *mu));
  if (!ok) {
    log_error("nc fidelity: write failed after % frames; the log stops", fmt_u32(l->frames));
    os_append_close(l->file);
    l->file = NULL;
  } else {
    l->bytes += nc_fl_frame_len(&fr);
  }
  tctx_temp_allocator_end(ta);
}

// ---- the oracle: --no-gpu --crowd-replay <recording> --oracle <log> ----

// a file read front to back in chunks
typedef struct NcStream {
  const char *path;
  u64 size, read_at;
  u8 *data;
  u32 cap, lo, hi;
} NcStream;

hz_internal b32 nc_stream_have(NcStream *st, u32 len) {
  if (st->hi - st->lo >= len)
    return true;
  mem_move(st->data, st->data + st->lo, st->hi - st->lo);
  st->hi -= st->lo;
  st->lo = 0;
  if (len > st->cap)
    return false;
  while (st->hi < len && st->read_at < st->size) {
    u32 want = (u32)MIN((u64)(st->cap - st->hi), st->size - st->read_at);
    u32 got = os_read_file_range(st->path, st->read_at, want, st->data + st->hi);
    if (!got)
      return false;
    st->hi += got;
    st->read_at += got;
  }
  return st->hi >= len;
}

// what the client holds of one id, as the log's changes left it
typedef struct NcOrHeld {
  b32 held;
  u32 seed;
  u32 tick; // with NC_FL_DRAWN
  v3 pos;   // the last logged position
  u32 pos_no;  // the log frame that logged it
  u32 tick_no;  // the log frame its tick last changed
  u32 multi_no; // the log frame it took more than one state, and how many
  u32 multi;
  u32 tick_step; // ticks its tick advanced at that frame: one state a tick at most
  u32 tier;      // the client's tier at its last tick record (0: not logged)
  u32 ev_no;   // the log frame an event last changed it; then `was` is the state before
  b32 was_held, was_drawn;
  u32 was_seed;
} NcOrHeld;

// updates and walking player-seconds of one client tier
typedef struct NcOrRate {
  u64 updates;
  f64 s;
} NcOrRate;

typedef struct NcOrBand {
  u64 truth, held, drawn;   // player-frames
  u64 pops, draw_pops;      // held or drawn state flipped while the recording had the player throughout
  u64 departs;              // let go between the recording's last frame with the player and its first without: not a pop
  u64 arrivals;             // held from within the recording's first frame with the player: not a pop
  u64 updates;              // new body ticks while the player walks
  f64 held_s;               // player-seconds held while walking, for the rate
  NcOrRate by_near, by_mid, by_far; // the same by the client's tier: the term's own split
  u32 *err;                 // [NC_FL_ERR_BUCKETS]
  u64 err_n;
} NcOrBand;

typedef struct NcOracle {
  NcStream log;
  NcFlHeader h;
  NcOrHeld *held;    // [max_ghosts]
  u32 *truth_seen;   // [max_ghosts] the log frame whose truth had the id (with truth_seed), for "throughout"
  u32 *truth_seed;
  u32 *pend, *pend_no; // [NC_OR_PEND] players that popped, followed until the recording loses them
  u32 *rec_first;      // [max_ghosts] the recording frame the player's run of log frames began in
  u32 *rec_first_tick; // [max_ghosts] that frame's tick
  u32 pend_n;
  NcOrBand band[NC_FL_BANDS];
  NcOrBand warm[NC_FL_BANDS]; // the warm-up's
  NcOrBand sec[NC_FL_BANDS]; // this second's
  u32 frames, cur;
  NcFlFrame prev;
  b32 any;
  u64 sec_wall, sec_rx;
  u32 sec_frames;
  u64 first_wall, first_rx, last_wall, last_rx;
  u64 stale; // player-frames the client held that the recording did not have
  u32 seconds;
  u32 *kbs; // [seconds] each second's KB/s past the warm-up
  u32 kbs_cap, kbs_n;
  u64 term_wall, term_rx; // where the terms start: the warm-up's end
  u32 term_frames;
  b32 done;
} NcOracle;

#define NC_OR_PEND 1024
#define NC_OR_PEND_FRAMES 600 // frames a popped player is followed

hz_internal u32 nc_or_band(f32 d) { return d < NC_FL_NEAR ? 0u : d < NC_FL_MID ? 1u : 2u; }

hz_internal b32 nc_oracle_open(NcOracle *o, NcReplay *r, Allocator *a, const char *path) {
  o->log = (NcStream){.path = path, .size = os_file_size(path), .cap = (u32)MB(64)};
  o->log.data = ALLOC_ARRAY_NO_ZERO(a, u8, o->log.cap);
  if (!nc_stream_have(&o->log, sizeof o->h)) {
    log_error("nc oracle: cannot read %", fmt_cstr(path));
    return false;
  }
  mem_cpy(&o->h, o->log.data, sizeof o->h);
  o->log.lo = sizeof o->h;
  if (o->h.magic != NC_FL_MAGIC || o->h.version != NC_FL_VERSION) {
    log_error("nc oracle: % is not a fidelity log", fmt_cstr(path));
    return false;
  }
  o->held = ALLOC_ARRAY(a, NcOrHeld, r->h.max_ghosts);
  o->truth_seen = ALLOC_ARRAY(a, u32, r->h.max_ghosts);
  o->truth_seed = ALLOC_ARRAY(a, u32, r->h.max_ghosts);
  o->pend = ALLOC_ARRAY(a, u32, NC_OR_PEND);
  o->rec_first = ALLOC_ARRAY(a, u32, r->h.max_ghosts);
  o->rec_first_tick = ALLOC_ARRAY(a, u32, r->h.max_ghosts);
  o->pend_no = ALLOC_ARRAY(a, u32, NC_OR_PEND);
  for (u32 b = 0; b < NC_FL_BANDS; b++) {
    o->band[b].err = ALLOC_ARRAY(a, u32, NC_FL_ERR_BUCKETS);
    o->sec[b].err = ALLOC_ARRAY(a, u32, NC_FL_ERR_BUCKETS);
    o->warm[b].err = ALLOC_ARRAY(a, u32, NC_FL_ERR_BUCKETS);
  }
  o->kbs_cap = 4096;
  o->kbs = ALLOC_ARRAY(a, u32, o->kbs_cap);
  log_info("nc oracle: % (% MB) against the recording", fmt_cstr(path), fmt_f64_p((f64)o->log.size / (1024.0 * 1024.0), 1));
  return true;
}

// the error under which `q` of the band's samples fall, metres
hz_internal f32 nc_or_err_q(const NcOrBand *b, f64 q) {
  if (!b->err_n)
    return 0.0f;
  u64 want = (u64)((f64)b->err_n * q), sum = 0;
  for (u32 i = 0; i < NC_FL_ERR_BUCKETS; i++) {
    sum += b->err[i];
    if (sum > want)
      return (f32)(i + 1) / 100.0f;
  }
  return (f32)NC_FL_ERR_BUCKETS / 100.0f;
}

hz_internal void nc_or_rate_add(NcOrRate *into, const NcOrRate *from) {
  into->updates += from->updates;
  into->s += from->s;
}

hz_internal f64 nc_or_rate_hz(const NcOrRate *r) { return r->s > 0 ? (f64)r->updates / r->s : 0.0; }

hz_internal void nc_or_band_add(NcOrBand *into, NcOrBand *from) {
  into->truth += from->truth;
  into->held += from->held;
  into->drawn += from->drawn;
  into->pops += from->pops;
  into->draw_pops += from->draw_pops;
  into->departs += from->departs;
  into->arrivals += from->arrivals;
  into->updates += from->updates;
  into->held_s += from->held_s;
  nc_or_rate_add(&into->by_near, &from->by_near);
  nc_or_rate_add(&into->by_mid, &from->by_mid);
  nc_or_rate_add(&into->by_far, &from->by_far);
  into->err_n += from->err_n;
  for (u32 i = 0; i < NC_FL_ERR_BUCKETS; i++)
    into->err[i] += from->err[i];
  u32 *err = from->err;
  mem_set(err, 0, NC_FL_ERR_BUCKETS * sizeof(u32));
  *from = (NcOrBand){.err = err};
}

hz_internal const char *nc_or_band_name(u32 k) { return k == 0 ? "0-16 m" : k == 1 ? "16-64 m" : "64+ m"; }

hz_internal void nc_or_log_bands(const char *what, const NcOrBand *b, u32 frames, f64 seconds, f64 kbs) {
  for (u32 k = 0; k < NC_FL_BANDS; k++) {
    f64 pf = b[k].truth ? (f64)b[k].truth : 1.0;
    log_info("nc oracle %: % coverage held % % drawn % %, % players in the band (mean), pops % (% a minute) draw pops % departures % arrivals %, "
             "update rate % Hz, position error p50 % m p99 % m",
             fmt_cstr(what), fmt_cstr(nc_or_band_name(k)), fmt_f64_p(100.0 * (f64)b[k].held / pf, 3), fmt_char('%'),
             fmt_f64_p(100.0 * (f64)b[k].drawn / pf, 3), fmt_char('%'), fmt_f64_p(frames ? (f64)b[k].truth / (f64)frames : 0.0, 1),
             fmt_u64(b[k].pops), fmt_f64_p(seconds > 0 ? (f64)b[k].pops * 60.0 / seconds : 0.0, 1), fmt_u64(b[k].draw_pops),
             fmt_u64(b[k].departs), fmt_u64(b[k].arrivals), fmt_f64_p(b[k].held_s > 0 ? (f64)b[k].updates / b[k].held_s : 0.0, 2), fmt_f32_p(nc_or_err_q(&b[k], 0.5), 2),
             fmt_f32_p(nc_or_err_q(&b[k], 0.99), 2));
  }
  // the update-rate term by the client's tier, every band together
  NcOrRate tn = {0}, tm = {0}, tf = {0};
  for (u32 k = 0; k < NC_FL_BANDS; k++) {
    log_info("nc oracle %: % by tier: near % Hz (% player-s), mid % Hz (% player-s), far % Hz (% player-s)", fmt_cstr(what),
             fmt_cstr(nc_or_band_name(k)), fmt_f64_p(nc_or_rate_hz(&b[k].by_near), 2), fmt_f64_p(b[k].by_near.s, 0),
             fmt_f64_p(nc_or_rate_hz(&b[k].by_mid), 2), fmt_f64_p(b[k].by_mid.s, 0), fmt_f64_p(nc_or_rate_hz(&b[k].by_far), 2),
             fmt_f64_p(b[k].by_far.s, 0));
    nc_or_rate_add(&tn, &b[k].by_near);
    nc_or_rate_add(&tm, &b[k].by_mid);
    nc_or_rate_add(&tf, &b[k].by_far);
  }
  log_info("nc oracle %: update rate by tier (walking players): near % Hz (% player-s), mid % Hz (% player-s), far % Hz (% player-s)",
           fmt_cstr(what), fmt_f64_p(nc_or_rate_hz(&tn), 2), fmt_f64_p(tn.s, 0), fmt_f64_p(nc_or_rate_hz(&tm), 2), fmt_f64_p(tm.s, 0),
           fmt_f64_p(nc_or_rate_hz(&tf), 2), fmt_f64_p(tf.s, 0));
  log_info("nc oracle %: downlink % KB/s", fmt_cstr(what), fmt_f64_p(kbs, 1));
}

// the recording at shown tick `tick` (+ a fraction under 1) reached: a is at or before it, b after; exact, in ticks
hz_internal void nc_or_truth_seek(NcReplay *r, NetTick tick) {
  while (!r->ended && !net_tick_newer(r->b->fr.tick, tick)) {
    NcReplayBuf *old = r->a;
    r->a = r->b;
    r->b = old;
    r->ended = !nc_replay_next(r, r->b);
  }
}

// one log frame against the truth at its shown tick
hz_internal void nc_oracle_frame(NcOracle *o, NcReplay *r, const NcFlFrame *fr, const NcFlEvent *ev, const NcFlTick *tk,
                                 const NcFlPos *ps, const NcFlMulti *mu) {
  if (net_tick_since(fr->tick, r->first_tick) < 0)
    return;
  nc_or_truth_seek(r, fr->tick);
  // between the two frames in ticks: seconds in f32 round a tick's fraction away late in a run
  i32 span = net_tick_since(r->b->fr.tick, r->a->fr.tick);
  f64 into = (f64)net_tick_since(fr->tick, r->a->fr.tick) + (f64)fr->fraction;
  f32 alpha = r->ended || span <= 0 ? 0.0f : (f32)m_clampf((f32)(into / (f64)span), 0.0f, 1.0f);
  u32 no = ++o->frames;
  NcOrHeld *hs = o->held;
  for (u32 i = 0; i < fr->events; i++) {
    u32 id = ev[i].id_kind & ((1u << NC_FL_KIND_SHIFT) - 1), kind = ev[i].id_kind >> NC_FL_KIND_SHIFT;
    if (id >= r->h.max_ghosts)
      continue;
    NcOrHeld *h = &hs[id];
    if (h->ev_no != no) {
      h->ev_no = no;
      h->was_held = h->held;
      h->was_drawn = h->held && (h->tick & NC_FL_DRAWN);
      h->was_seed = h->seed;
    }
    if (kind == NC_FL_ENTER) {
      h->held = true;
      h->seed = ev[i].val;
      h->tick = 0;
    } else if (kind == NC_FL_LEAVE) {
      h->held = false;
    } else if (kind == NC_FL_DRAW) {
      h->tick = (h->tick & ~NC_FL_DRAWN) | (ev[i].val ? NC_FL_DRAWN : 0u);
    }
  }
  for (u32 i = 0; i < fr->ticks; i++) {
    u32 id = tk[i].id & NC_FL_TIER_IDS;
    if (id >= r->h.max_ghosts)
      continue;
    NcOrHeld *h = &hs[id];
    h->tier = tk[i].id >> NC_FL_TIER_SHIFT;
    u32 old = h->tick & ~NC_FL_DRAWN;
    // a record for a tier change alone is no new state
    if (old == tk[i].tick && h->tick_no)
      continue;
    i32 step = old ? net_tick_since(tk[i].tick, old) : 1;
    h->tick_step = step > 0 ? (u32)step : 1u;
    h->tick = (h->tick & NC_FL_DRAWN) | tk[i].tick;
    h->tick_no = no;
  }
  for (u32 i = 0; i < fr->multi; i++)
    if (mu[i].id < r->h.max_ghosts && mu[i].received <= hs[mu[i].id].tick_step) {
      hs[mu[i].id].multi_no = no;
      hs[mu[i].id].multi = mu[i].received;
    }
  for (u32 i = 0; i < fr->positions; i++) {
    NcRecPlayer rp = {.lo = ps[i].lo, .hi = ps[i].hi};
    u32 id = nc_rec_id(rp);
    if (id < r->h.max_ghosts) {
      hs[id].pos = nc_rec_pos(rp);
      hs[id].pos_no = no;
    }
  }
  // the viewer: its avatar in the truth, else the camera's look point (a spectator)
  v3 view = fr->look;
  if (fr->own_id && fr->own_id < r->h.max_ghosts) {
    const NcReplaySeen *s = &r->a->seen[fr->own_id];
    if (s->stamp == r->a->no && s->seed == fr->own_seed)
      view = s->pos;
  }
  f64 dt = o->any ? (f64)net_tick_since(fr->tick, o->prev.tick) * (f64)r->h.tick_us / 1e6 +
                        (f64)(fr->fraction - o->prev.fraction) * (f64)r->h.tick_us / 1e6
                  : 0.0;
  if (dt < 0.0)
    dt = 0.0;
  u32 truth_n = 0;
  for (u32 i = 0; i < r->a->fr.count; i++) {
    u32 id = r->a->ids[i];
    const NcReplaySeen *sa = &r->a->seen[id], *sb = &r->b->seen[id];
    if (id == fr->own_id && sa->seed == fr->own_seed)
      continue;
    b32 both = !r->ended && sb->stamp == r->b->no && sb->seed == sa->seed;
    v3 pos = both ? v3_lerp(sa->pos, sb->pos, alpha) : sa->pos;
    f32 d = v2_length((v2){pos.x - view.x, pos.z - view.z});
    NcOrBand *b = &o->sec[nc_or_band(d)];
    b->truth++;
    truth_n++;
    const NcOrHeld *h = &hs[id];
    b32 held = h->held && h->seed == sa->seed;
    b32 drawn = held && (h->tick & NC_FL_DRAWN);
    // throughout: the truth had this player at the previous log frame too
    b32 throughout = o->truth_seen[id] == no - 1 && o->truth_seed[id] == sa->seed;
    o->truth_seen[id] = no;
    o->truth_seed[id] = sa->seed;
    if (!throughout) {
      o->rec_first[id] = r->a->no;
      o->rec_first_tick[id] = r->a->fr.tick;
    }
    // still within the recording's first frame with the player: it joined within that frame's ticks
    b32 arriving = o->rec_first[id] == r->a->no;
    if (held) {
      b->held++;
      b->drawn += drawn ? 1u : 0u;
      // positions are logged near the view each frame, else one frame in NC_FL_POS_EVERY: an even sample
      if (h->pos_no == no) {
        f32 e = v2_length((v2){h->pos.x - pos.x, h->pos.z - pos.z});
        u32 bucket = (u32)m_minf(e * 100.0f, (f32)(NC_FL_ERR_BUCKETS - 1));
        b->err[bucket]++;
        b->err_n++;
      }
    }
    // a standing player is rightly never resent: the rate counts walking players only
    b32 walking = both && v2_length((v2){sb->pos.x - sa->pos.x, sb->pos.z - sa->pos.z}) > 0.005f;
    if (throughout && o->any) {
      b32 changed = h->ev_no == no;
      b32 was_held = changed ? h->was_held && h->was_seed == sa->seed : held;
      b32 was_drawn = changed ? was_held && h->was_drawn : drawn;
      // the recording's next frame lacks the player: it left within that frame's ticks, at a tick the frames cannot place
      b32 departing = !both && !held && !drawn;
      b32 arrival = arriving && held && !was_held;
      b32 pop = was_held != held && !departing && !arrival;
      b->departs += departing && was_held ? 1u : 0u;
      b->arrivals += arrival ? 1u : 0u;
      if (departing && was_held)
        log_info("nc oracle departure: frame % tick % id % at % m (band %), its tick %", fmt_u32(no), fmt_u32(fr->tick),
                 fmt_u32(id), fmt_f32_p(d, 1), fmt_u32(nc_or_band(d)), fmt_u32(h->tick & ~NC_FL_DRAWN));
      b->pops += pop ? 1u : 0u;
      if (pop)
        log_info("nc oracle pop: frame % tick % id % % at % m (band %), its tick % (logged at frame %), drawn % -> %",
                 fmt_u32(no), fmt_u32(fr->tick), fmt_u32(id), fmt_cstr(held ? "appeared" : "vanished"), fmt_f32_p(d, 1),
                 fmt_u32(nc_or_band(d)), fmt_u32(h->tick & ~NC_FL_DRAWN), fmt_u32(h->tick_no), fmt_b32(was_drawn), fmt_b32(drawn));
      if (pop && held)
        log_info("nc oracle pop: id % had been in the recording % frames (since frame %, tick %); the recording's frames now at "
                 "ticks % and %; the log's previous frame showed tick % + %, this one % + %",
                 fmt_u32(id), fmt_u32(r->a->no - o->rec_first[id]), fmt_u32(o->rec_first[id]), fmt_u32(o->rec_first_tick[id]),
                 fmt_u32(r->a->fr.tick), fmt_u32(r->b->fr.tick), fmt_u32(o->prev.tick), fmt_f32_p(o->prev.fraction, 3),
                 fmt_u32(fr->tick), fmt_f32_p(fr->fraction, 3));
      if (pop && o->pend_n < NC_OR_PEND) {
        o->pend[o->pend_n] = id;
        o->pend_no[o->pend_n++] = no;
      }
      b->draw_pops += was_drawn != drawn && !departing && !(arriving && drawn && !was_drawn) ? 1u : 0u;
      if (held && was_held && walking) {
        u64 up = h->tick_no == no ? (h->multi_no == no ? h->multi : 1u) : 0u;
        b->held_s += dt;
        b->updates += up;
        NcOrRate *tr = h->tier == NETCODE_TIER_NEAR ? &b->by_near : h->tier == NETCODE_TIER_MID ? &b->by_mid
                       : h->tier == NETCODE_TIER_FAR ? &b->by_far : NULL;
        if (tr) {
          tr->s += dt;
          tr->updates += up;
        }
      }
    }
  }
  for (u32 i = 0; i < o->pend_n;) {
    u32 id = o->pend[i], since = no - o->pend_no[i];
    if (o->truth_seen[id] != no || since >= NC_OR_PEND_FRAMES) {
      log_info("nc oracle pop: id % %: % frames after the pop (tick %)", fmt_u32(id),
               fmt_cstr(o->truth_seen[id] != no ? "left the recording" : "still in the recording"), fmt_u32(since),
               fmt_u32(fr->tick));
      o->pend[i] = o->pend[--o->pend_n];
      o->pend_no[i] = o->pend_no[o->pend_n];
    } else {
      i++;
    }
  }
  if (fr->count > truth_n)
    o->stale += fr->count - truth_n;
  if (!o->any) {
    o->first_wall = fr->wall_us;
    o->first_rx = fr->rx_bytes;
    o->sec_wall = fr->wall_us;
    o->sec_rx = fr->rx_bytes;
  }
  o->last_wall = fr->wall_us;
  o->last_rx = fr->rx_bytes;
  o->prev = *fr;
  o->any = true;
  o->sec_frames++;
  if (fr->wall_us - o->sec_wall >= 1000000u) {
    f64 s = (f64)(fr->wall_us - o->sec_wall) / 1e6;
    f64 kbs = (f64)(fr->rx_bytes - o->sec_rx) / 1024.0 / s;
    o->seconds++;
    const NcOrBand *b = o->sec;
    f64 t0 = b[0].truth ? (f64)b[0].truth : 1.0, t1 = b[1].truth ? (f64)b[1].truth : 1.0, t2 = b[2].truth ? (f64)b[2].truth : 1.0;
    log_info("nc oracle second % (tick %, % frames%): held % / % / % %, pops % / % / %, walking update Hz % / % / %, "
             "error p99 m % / % / %, % KB/s, players in the bands % / % / %",
             fmt_u32(o->seconds), fmt_u32(fr->tick), fmt_u32(o->sec_frames),
             fmt_cstr(fr->wall_us - o->first_wall < NC_FL_WARMUP_US ? ", warm-up" : ""), fmt_f64_p(100.0 * (f64)b[0].held / t0, 2),
             fmt_f64_p(100.0 * (f64)b[1].held / t1, 2), fmt_f64_p(100.0 * (f64)b[2].held / t2, 2), fmt_char('%'), fmt_u64(b[0].pops),
             fmt_u64(b[1].pops), fmt_u64(b[2].pops), fmt_f64_p(b[0].held_s > 0 ? (f64)b[0].updates / b[0].held_s : 0.0, 1),
             fmt_f64_p(b[1].held_s > 0 ? (f64)b[1].updates / b[1].held_s : 0.0, 1),
             fmt_f64_p(b[2].held_s > 0 ? (f64)b[2].updates / b[2].held_s : 0.0, 1), fmt_f32_p(nc_or_err_q(&b[0], 0.99), 2),
             fmt_f32_p(nc_or_err_q(&b[1], 0.99), 2), fmt_f32_p(nc_or_err_q(&b[2], 0.99), 2), fmt_f64_p(kbs, 1),
             fmt_f64_p((f64)b[0].truth / (f64)o->sec_frames, 1), fmt_f64_p((f64)b[1].truth / (f64)o->sec_frames, 1),
             fmt_f64_p((f64)b[2].truth / (f64)o->sec_frames, 1));
    b32 warm = fr->wall_us - o->first_wall < NC_FL_WARMUP_US;
    for (u32 k = 0; k < NC_FL_BANDS; k++)
      nc_or_band_add(warm ? &o->warm[k] : &o->band[k], &o->sec[k]);
    if (warm) {
      o->term_wall = fr->wall_us;
      o->term_rx = fr->rx_bytes;
      o->term_frames = o->frames;
    } else if (o->seconds <= o->kbs_cap) {
      o->kbs[o->kbs_n++] = (u32)(kbs * 10.0);
    }
    o->sec_wall = fr->wall_us;
    o->sec_rx = fr->rx_bytes;
    o->sec_frames = 0;
  }
}

// GPU-less, main lane, some frames at a time: the log against the recording, then the run's totals
hz_internal void nc_oracle_step(NcOracle *o, NcReplay *r) {
  if (o->done)
    return;
  for (u32 n = 0; n < 64; n++) {
    NcFlFrame fr;
    b32 ok = nc_stream_have(&o->log, sizeof fr);
    if (ok) {
      mem_cpy(&fr, o->log.data + o->log.lo, sizeof fr);
      ok = fr.magic == NC_FL_FRAME && nc_stream_have(&o->log, nc_fl_frame_len(&fr));
    }
    if (!ok) {
      o->done = true;
      break;
    }
    const u8 *body = o->log.data + o->log.lo + sizeof fr;
    const NcFlEvent *ev = (const NcFlEvent *)body;
    const NcFlTick *tk = (const NcFlTick *)(ev + fr.events);
    const NcFlPos *ps = (const NcFlPos *)(tk + fr.ticks);
    nc_oracle_frame(o, r, &fr, ev, tk, ps, (const NcFlMulti *)(ps + fr.positions));
    o->log.lo += nc_fl_frame_len(&fr);
  }
  if (!o->done)
    return;
  b32 tail_warm = o->last_wall - o->first_wall < NC_FL_WARMUP_US;
  for (u32 k = 0; k < NC_FL_BANDS; k++)
    nc_or_band_add(tail_warm ? &o->warm[k] : &o->band[k], &o->sec[k]);
  f64 ws = (f64)(o->term_wall - o->first_wall) / 1e6;
  log_info("nc oracle: % log frames over % s; % player-frames held that the recording did not have", fmt_u32(o->frames),
           fmt_f64_p((f64)(o->last_wall - o->first_wall) / 1e6, 1), fmt_u64(o->stale));
  nc_or_log_bands("warm-up", o->warm, o->term_frames, ws, ws > 0 ? (f64)(o->term_rx - o->first_rx) / 1024.0 / ws : 0.0);
  f64 s = (f64)(o->last_wall - o->term_wall) / 1e6;
  f64 kbs = s > 0 ? (f64)(o->last_rx - o->term_rx) / 1024.0 / s : 0.0;
  nc_or_log_bands("terms", o->band, o->frames - o->term_frames, s, kbs);
  u32 n = o->kbs_n;
  if (n) {
    TempAllocator ta = tctx_temp_allocator_begin(NULL);
    const u32 *sorted = radix_sort_u32(o->kbs, NULL, ALLOC_ARRAY_NO_ZERO(&ta.allocator, u32, n), NULL, n, (RadixSortDesc){0}).keys;
    log_info("nc oracle terms: downlink by second KB/s p50 % p99 % max %", fmt_f32_p((f32)sorted[n / 2] / 10.0f, 1),
             fmt_f32_p((f32)sorted[(n * 99) / 100] / 10.0f, 1), fmt_f32_p((f32)sorted[n - 1] / 10.0f, 1));
    tctx_temp_allocator_end(ta);
  }
}

// ---- the video's other half: --crowd-replay <recording> --follow <log> --frames-out <dir> ----

typedef struct NcFollow {
  NcStream log;
  NcFlFrame fr; // the captured log frame being drawn
  b32 have, ended;
} NcFollow;

hz_internal b32 nc_follow_open(NcFollow *f, Allocator *a, const char *path) {
  f->log = (NcStream){.path = path, .size = os_file_size(path), .cap = (u32)MB(64)};
  f->log.data = ALLOC_ARRAY_NO_ZERO(a, u8, f->log.cap);
  NcFlHeader h;
  if (!nc_stream_have(&f->log, sizeof h)) {
    log_error("nc follow: cannot read %", fmt_cstr(path));
    return false;
  }
  mem_cpy(&h, f->log.data, sizeof h);
  f->log.lo = sizeof h;
  if (h.magic != NC_FL_MAGIC || h.version != NC_FL_VERSION) {
    log_error("nc follow: % is not a fidelity log", fmt_cstr(path));
    return false;
  }
  return true;
}

// the next log frame the live client captured; false at the log's end
hz_internal b32 nc_follow_next(NcFollow *f) {
  for (;;) {
    NcFlFrame fr;
    if (!nc_stream_have(&f->log, sizeof fr))
      return false;
    mem_cpy(&fr, f->log.data + f->log.lo, sizeof fr);
    u32 len = nc_fl_frame_len(&fr);
    if (fr.magic != NC_FL_FRAME || !nc_stream_have(&f->log, len))
      return false;
    f->log.lo += len;
    if (fr.shot) {
      f->fr = fr;
      return true;
    }
  }
}
