// net_crowd --bots: players from this process (the engine's netcode loadgen with net_crowd's types), walking the plaza

// exactly `len` bytes from hex text
hz_internal b32 nc_arg_hex(const char *flag, u8 *out, u32 len) {
  const char *v = hz_arg_value(flag);
  return v && cstr_len(v) == 2 * len && netcode_ticket_from_hex(v, out, len) == len;
}

hz_internal b32 nc_arg_u32(const char *flag, u32 def, u32 *out) {
  const char *v = hz_arg_value(flag);
  *out = def;
  return !v || cstr_to_u32((CString)v, out);
}

#ifdef NET_HAS_UDP
#define NC_BOT_TICK_US 16667
#define NC_BOT_ROAM 30.0f          // metres a bot's next waypoint is from where it stands
#define NC_BOT_GROUP_PERIOD 240u   // ticks between a group's waypoints (4 s): groups keep running
#define NC_BOT_GROUP_BLOCK 6u      // a group is the first 3-6 ids of a block of 6, in 40% of blocks: 30% of bots
#define NC_BOT_PLAYERS_PER_SPOT 2000u

typedef struct NcBot {
  NcBody body; // reckoned from its own inputs, as the server applies them
  v2 move;
  v2 target;
  u32 wait; // ticks left standing
  u32 rng;
  NetTick last;
  NetTick spawn; // the ghost it reckons for
  u32 group_k;   // the group waypoint it walks to (a grouped bot)
  f32 margin;    // battle royale: metres inside the zone's edge it turns for the centre (negative: outside)
} NcBot;

typedef struct NcBots {
  NetcodeLoadgen *lg;
  NcBot *bots;
  NetConfig net;
  u8 *key; // [NET_TOKEN_KEY_BYTES]
  u64 first_id;
  f32 spawn_half;
  u32 spots;       // hotspots: where solo bots gather
  f32 spot_radius; // metres around a hotspot its visitors stand in
  u32 *churn;      // [players] each bot's leave dice, its lane's
  u32 leave_per_tick; // of 2^32: `--churn` percent of the bots leave a minute
  u32 players;
  u64 now, start, next_report, end;
  u32 tick;
  u32 report_ticks;
  u64 busy_us, busy_max_us;
  NetcodeLoadgenStats last;
  NcRoyale royale; // --royale <tick>: the rules the server announced
  // section 5's proof: every bot's snapshots each second, as a histogram by rate (the last bucket for NC_RATE_BUCKETS - 1 and up)
  u64 *last_snaps;  // [players]
  u64 *last_bytes;  // [players] snapshot bytes at the last second
  u8 *was_connected; // [players] connected at the last report: only whole seconds count
  u64 *rate_hist;   // [NC_RATE_BUCKETS] over the run
  u32 *rate_tmp;    // [players] this second's rates
  u32 *rate_sorted; // [players]
  u32 *kbs_tmp;     // [players] this second's downlink, tenths of KB/s
  b32 ended;
} NcBots;

#define NC_RATE_BUCKETS 72

force_inline u32 nc_rand(u32 *s) {
  u32 x = *s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *s = x;
}

force_inline f32 nc_rand01(u32 *s) { return (f32)(nc_rand(s) & 0xffffff) / 16777215.0f; }

// hotspot `k`'s centre: the same for every bot process, from the spawn square
hz_internal v2 nc_spot(const NcBots *b, u32 k) {
  u32 h = nc_hash(0x5370u + k), j = nc_hash(0x7453u + k);
  f32 r = b->spawn_half * 0.75f;
  return (v2){((f32)(h & 0xffffff) / 16777215.0f * 2.0f - 1.0f) * r, ((f32)(j & 0xffffff) / 16777215.0f * 2.0f - 1.0f) * r};
}

// a point within `radius` of `c`, uniform over the disc
hz_internal v2 nc_disc(u32 *rng, v2 c, f32 radius) {
  f32 a = nc_rand01(rng) * 2.0f * PI, r = radius * m_sqrtf(nc_rand01(rng));
  return (v2){c.x + m_cosf(a) * r, c.y + m_sinf(a) * r};
}

// a solo bot's next waypoint: half the time a hotspot, else a walk around where it stands
hz_internal void nc_bot_retarget(NcBots *b, NcBot *bot) {
  f32 h = b->spawn_half;
  v2 t = nc_rand(&bot->rng) & 1 ? nc_disc(&bot->rng, nc_spot(b, nc_rand(&bot->rng) % b->spots), b->spot_radius)
                                : nc_disc(&bot->rng, (v2){bot->body.pos.x, bot->body.pos.z}, NC_BOT_ROAM);
  bot->target = (v2){m_clampf(t.x, -h, h), m_clampf(t.y, -h, h)};
}

// a grouped bot's block (its group), or UINT32_MAX for a solo one; `member` its place in the group
hz_internal u32 nc_bot_group(u64 id, u32 *member) {
  u32 block = (u32)(id / NC_BOT_GROUP_BLOCK), size = 3 + nc_hash(block) % 4;
  *member = (u32)(id % NC_BOT_GROUP_BLOCK);
  return *member < size && nc_hash(block ^ 0x6a09e667u) % 100 < 40 ? block : UINT32_MAX;
}

// the group's k-th waypoint: every member computes it alone, so groups share no state across lanes
hz_internal v2 nc_group_target(const NcBots *b, u32 block, u32 k, u32 member) {
  u32 rng = nc_hash(((u64)block << 32) | k) | 1u;
  v2 c = nc_rand(&rng) & 1 ? nc_disc(&rng, nc_spot(b, nc_rand(&rng) % b->spots), b->spot_radius)
                           : nc_disc(&rng, (v2){0, 0}, b->spawn_half);
  f32 a = (f32)member * (2.0f * PI / 6.0f);
  return (v2){m_clampf(c.x + m_cosf(a) * 1.1f, -b->spawn_half, b->spawn_half),
              m_clampf(c.y + m_sinf(a) * 1.1f, -b->spawn_half, b->spawn_half)};
}

// the player's lane: walk to a waypoint; at one in three stand 1-4 s, else straight on to the next
hz_internal void nc_bot_input(void *user, u32 player, NetTick tick, NetTick spawn_tick, void *out) {
  NcBots *b = user;
  NcBot *bot = &b->bots[player];
  if (bot->spawn != spawn_tick) {
    *bot = (NcBot){.body = {.pos = nc_spawn(b->first_id + player, b->spawn_half), .facing = {0.0f, 1.0f}},
                   .rng = nc_hash(b->first_id + player) | 1u, .last = tick, .spawn = spawn_tick,
                   .group_k = UINT32_MAX};
    nc_bot_retarget(b, bot);
    // from 12 m outside the edge (the careless take damage before they turn) to 20 m inside
    bot->margin = -12.0f + 32.0f * nc_rand01(&bot->rng);
  }
  i32 since = net_tick_since(tick, bot->last);
  u32 steps = since > 0 ? (u32)MIN(since, 64) : 0;
  for (u32 k = 0; k < steps; k++)
    nc_step(&bot->body, bot->move, (f32)NC_BOT_TICK_US / 1e6f);
  if (steps)
    bot->last = tick;
  // battle royale: near the zone's edge (by its own caution) it heads for the zone's last point
  if (b->royale.on && net_tick_since(tick, b->royale.start) >= 0) {
    v2 c = b->royale.centre, d = {c.x - bot->body.pos.x, c.y - bot->body.pos.z};
    f32 dist = v2_length(d);
    if (dist > nc_zone_radius(&b->royale, tick) - bot->margin) {
      bot->target = nc_disc(&bot->rng, c, 3.0f);
      bot->move = dist < 0.3f ? (v2){0, 0} : v2_scale(d, 1.0f / dist);
      ((NcInput *)out)->move = bot->move;
      return;
    }
  }
  u32 member, group = nc_bot_group(b->first_id + player, &member);
  if (group != UINT32_MAX) {
    u32 k = tick / NC_BOT_GROUP_PERIOD;
    if (k != bot->group_k) {
      bot->group_k = k;
      bot->target = nc_group_target(b, group, k, member);
    }
  }
  v2 to = {bot->target.x - bot->body.pos.x, bot->target.y - bot->body.pos.z};
  f32 dist = v2_length(to);
  if (group != UINT32_MAX) {
    bot->move = dist < 0.3f ? (v2){0, 0} : v2_scale(to, 1.0f / dist);
  } else if (bot->wait) {
    bot->wait -= MIN(bot->wait, steps);
    bot->move = (v2){0, 0};
  } else if (dist < 0.3f) {
    bot->wait = nc_rand(&bot->rng) % 3 == 0 ? 60 + nc_rand(&bot->rng) % 181 : 0;
    nc_bot_retarget(b, bot);
    v2 next = {bot->target.x - bot->body.pos.x, bot->target.y - bot->body.pos.z};
    f32 len = v2_length(next);
    bot->move = bot->wait || len < 0.3f ? (v2){0, 0} : v2_scale(next, 1.0f / len);
  } else {
    bot->move = v2_scale(to, 1.0f / dist);
  }
  ((NcInput *)out)->move = bot->move;
}

// main lane: --bots N --server <host>:<port> --key <hex> --pin <hex> [--first-id] [--capacity] [--cmd-interval] [--seconds]
hz_internal b32 nc_bots_setup(NcBots *b, Allocator *a) {
  u32 players = 0, first = 1, capacity = 0, area = 0, cmd = 1, seconds = 0, royale_start = 0, shrink_s = 120;
  const char *server = hz_arg_value("--server");
  b->key = ALLOC_ARRAY(a, u8, NET_TOKEN_KEY_BYTES);
  u8 *pin = ALLOC_ARRAY(a, u8, 32);
  u32 colon = 0;
  for (u32 i = 0; server && server[i]; i++)
    colon = server[i] == ':' ? i : colon;
  u32 port = 0, churn_pct = 1;
  OsNetAddr addr = {0};
  char *host = server && colon ? ALLOC_ARRAY(a, char, colon + 1) : NULL;
  if (host)
    mem_cpy(host, server, colon);
  b32 ok = nc_arg_u32("--bots", 0, &players) && players && nc_arg_u32("--first-id", 1, &first) &&
           nc_arg_u32("--capacity", 64, &capacity) && nc_arg_u32("--area", NC_AREA_PER_PLAYER, &area) && area && nc_arg_u32("--cmd-interval", 1, &cmd) && cmd &&
           nc_arg_u32("--seconds", 0, &seconds) && host && cstr_to_u32((CString)(server + colon + 1), &port) && port &&
           port < 65536 && os_net_resolve(host, (u16)port, &addr) && nc_arg_hex("--key", b->key, NET_TOKEN_KEY_BYTES) &&
           nc_arg_hex("--pin", pin, 32) && nc_arg_u32("--royale", 0, &royale_start) &&
           nc_arg_u32("--royale-shrink", 120, &shrink_s) && shrink_s && nc_arg_u32("--churn", 1, &churn_pct) && churn_pct <= 100;
  if (!ok) {
    log_error("net_crowd: --bots <n> needs --server <host>:<port> --key <hex> --pin <hex> "
              "[--first-id 1] [--capacity 64] [--area 4] [--cmd-interval 1] [--seconds 0] [--churn 1 (percent leaving a minute)] "
              "(the server prints the line)");
    return false;
  }
  b->leave_per_tick = (u32)((u64)churn_pct * 4294967296ull / 360000ull);
  NetConfig *c = &b->net;
  net_config_defaults(c);
  c->version = (NetVersion){1, 0, 0};
  c->protocol_id = NC_PROTOCOL;
  c->game_rate_bytes_per_s = NETCODE_SEND_RATE;
  c->game_send_interval_us = NC_BOT_TICK_US * cmd;
  c->quic.tls.server_name = host;
  mem_cpy(c->quic.tls.cert_pins[0], pin, 32);
  c->quic.tls.pin_count = 1;
  b->first_id = first;
  b->spawn_half = nc_spawn_half(capacity, area);
  if (hz_arg_value("--royale"))
    b->royale = nc_royale_rules(b->spawn_half, royale_start, shrink_s);
  b->bots = ALLOC_ARRAY(a, NcBot, players);
  b->last_snaps = ALLOC_ARRAY(a, u64, players);
  b->last_bytes = ALLOC_ARRAY(a, u64, players);
  b->kbs_tmp = ALLOC_ARRAY(a, u32, players);
  b->was_connected = ALLOC_ARRAY(a, u8, players);
  b->rate_hist = ALLOC_ARRAY(a, u64, NC_RATE_BUCKETS);
  b->rate_tmp = ALLOC_ARRAY(a, u32, players);
  b->rate_sorted = ALLOC_ARRAY(a, u32, players);
  b->churn = ALLOC_ARRAY(a, u32, players);
  b->players = players;
  b->spots = MAX(1u, players / NC_BOT_PLAYERS_PER_SPOT + 2);
  b->spot_radius = m_clampf(b->spawn_half * 0.05f, 4.0f, 12.0f);
  u64 unix_now = os_time_unix_ms() / 1000;
  NetcodeLoadgenDesc d = {.players = players, .first_player_id = first, .server = addr, .net = c, .fleet_key = b->key,
                          .protocol_id = NC_PROTOCOL, .token_expire_unix_s = unix_now + 6 * 3600, .now_unix_s = unix_now,
                          .registry_hash = netcode_registry_hash(&app_netcode_registry), .channel = 1,
                          .cmd_interval = cmd, .lanes = tctx_current()->thread_count, .tick_us = NC_BOT_TICK_US,
                          .registry = &app_netcode_registry, .input = nc_bot_input, .input_user = b};
  b->lg = netcode_loadgen_create(&d, a);
  b->now = b->start = (u64)os_ticks_to_us(os_time_now());
  b->next_report = b->now + 1000000;
  b->end = seconds ? b->now + (u64)seconds * 1000000 : 0;
  log_info("nc bots: % players (ids % to %) on % lanes against %, a command every % ticks", fmt_u32(players),
           fmt_u32(first), fmt_u32(first + players - 1), fmt_u32(d.lanes), fmt_cstr(server), fmt_u32(cmd));
  return true;
}

// every lane before netcode_loadgen_open: 2% of its bots join 10-120 s late (none in a battle royale: they would spectate)
hz_internal void nc_bots_late(NcBots *b, u32 players) {
  u32 lanes = tctx_current()->thread_count;
  for (u32 i = tctx_current()->thread_idx; i < players; i += lanes) {
    b->churn[i] = nc_hash(b->first_id + i + 0x63687572ull) | 1u;
    if (!b->royale.on && nc_rand(&b->churn[i]) % 100 < 2)
      netcode_loadgen_leave(b->lg, i, 600 + nc_rand(&b->churn[i]) % 6601);
  }
}

// every lane, once a tick: a bot leaves with probability `--churn`% a minute (default 1) and joins again, with a fresh token, 2-30 s later
hz_internal void nc_bots_churn(NcBots *b) {
  if (b->royale.on)
    return;
  u32 lanes = tctx_current()->thread_count, players = b->players;
  for (u32 i = tctx_current()->thread_idx; i < players; i += lanes)
    if (nc_rand(&b->churn[i]) < b->leave_per_tick)
      netcode_loadgen_leave(b->lg, i, 120 + nc_rand(&b->churn[i]) % 1681);
}

// main lane, once a second: each bot connected the whole second, its snapshots that second; min, p1, median
hz_internal void nc_bots_rates(NcBots *b) {
  u32 n = 0;
  for (u32 i = 0; i < b->players; i++) {
    NetcodeLoadgenPlayer p = netcode_loadgen_player(b->lg, i);
    if (p.connected && b->was_connected[i]) {
      u32 r = (u32)MIN(p.snapshots - b->last_snaps[i], (u64)UINT32_MAX);
      b->kbs_tmp[n] = (u32)MIN((p.snapshot_bytes - b->last_bytes[i]) * 10 / 1024, (u64)UINT32_MAX);
      b->rate_tmp[n++] = r;
      b->rate_hist[MIN(r, NC_RATE_BUCKETS - 1)]++;
    }
    b->last_snaps[i] = p.snapshots;
    b->last_bytes[i] = p.snapshot_bytes;
    b->was_connected[i] = (u8)p.connected;
  }
  if (n) {
    const u32 *s = radix_sort_u32(b->rate_tmp, NULL, b->rate_sorted, NULL, n, (RadixSortDesc){0}).keys;
    u32 under = 0;
    for (u32 i = 0; i < n && s[i] < 57; i++)
      under++;
    log_info("nc rates: % bots a whole second: snapshots/s min % p1 % median % max %; under 57 a second (95 percent of 60): %", fmt_u32(n),
             fmt_u32(s[0]), fmt_u32(s[n / 100]), fmt_u32(s[n / 2]), fmt_u32(s[n - 1]), fmt_u32(under));
    // each bot's downlink this second (the client side of the downlink term): median, worst 1%, worst
    const u32 *k = radix_sort_u32(b->kbs_tmp, NULL, b->rate_sorted, NULL, n, (RadixSortDesc){0}).keys;
    log_info("nc downlink: % bots a whole second: KB/s median % p99 % max %", fmt_u32(n), fmt_f32_p((f32)k[n / 2] / 10.0f, 1),
             fmt_f32_p((f32)k[(n * 99) / 100] / 10.0f, 1), fmt_f32_p((f32)k[n - 1] / 10.0f, 1));
  }
}

// main lane, at the end: the run's whole seconds by rate, to merge across processes by adding
hz_internal void nc_bots_rate_histogram(NcBots *b) {
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  StringBuilder sb = sb_create(1024, &ta.allocator);
  for (u32 k = 0; k < NC_RATE_BUCKETS; k++)
    if (b->rate_hist[k])
      sb_append_format(&sb, " %:%", fmt_u32(k), fmt_u64(b->rate_hist[k]));
  log_info("nc rates histogram (snapshots/s:bot-seconds, % and up in the last):%", fmt_u32(NC_RATE_BUCKETS - 1),
           fmt_cstr(sb_get(&sb)));
  tctx_temp_allocator_end(ta);
}

// main lane, once a second: what each process's players received (the proof's per-process line)
hz_internal void nc_bots_report(NcBots *b) {
  NetcodeLoadgenStats s = netcode_loadgen_stats(b->lg), o = b->last;
  log_info("nc bots: % s: % of % connected, % away, % receiving, % own an avatar; joins/s %, leaves/s %; % snapshots/s (% KB/s), "
           "% inputs/s; datagrams/s out % in %; rejoins %, sockets refused %; % ticks, update % us a tick (max %)",
           fmt_f64_p((f64)(b->now - b->start) / 1e6, 0), fmt_u32(s.connected), fmt_u32(s.players), fmt_u32(s.away),
           fmt_u32(s.receiving), fmt_u32(s.owning), fmt_u64(s.joins - o.joins), fmt_u64(s.leaves - o.leaves), fmt_u64(s.snapshots - o.snapshots),
           fmt_u64((s.snapshot_bytes - o.snapshot_bytes) / 1024), fmt_u64(s.inputs - o.inputs),
           fmt_u64(s.dgrams_sent - o.dgrams_sent), fmt_u64(s.dgrams_recv - o.dgrams_recv), fmt_u32(s.rejoins),
           fmt_u64(s.dgrams_refused), fmt_u32(b->report_ticks),
           fmt_u64(b->report_ticks ? b->busy_us / b->report_ticks : 0), fmt_u64(b->busy_max_us));
  b->last = s;
  b->report_ticks = 0;
  b->busy_us = 0;
  b->busy_max_us = 0;
}

// every lane, a frame a tick at a fixed 60 Hz
hz_internal void nc_bots_frame(NcBots *b) {
  nc_bots_churn(b);
  netcode_loadgen_update(b->lg, b->now, b->tick);
  lane_sync();
  if (is_main_thread()) {
    u64 t = (u64)os_ticks_to_us(os_time_now()), busy = t - b->now;
    b->busy_us += busy;
    b->busy_max_us = MAX(b->busy_max_us, busy);
    b->report_ticks++;
    if (b->now >= b->next_report) {
      nc_bots_report(b);
      nc_bots_rates(b);
      b->next_report += 1000000;
    }
    // a late tick runs at once, the schedule does not drift
    u64 next = b->now + NC_BOT_TICK_US;
    if (t < next)
      os_sleep(next - t);
    t = (u64)os_ticks_to_us(os_time_now());
    b->now = MAX(next, t);
    b->tick++;
    if (b->end && b->now >= b->end && !b->ended) {
      b->ended = true;
      nc_bots_rate_histogram(b);
      hz_request_quit();
    }
  }
  lane_sync();
}
#endif
