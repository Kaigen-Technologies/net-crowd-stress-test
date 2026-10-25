// net_crowd: the overlay's stats ghost, and the server map as the spectator stream (reliable RPCs, once a second)

#define NC_MAP_PX 256 // the map texture's side: 16 cells a pixel

// order-free, so the server's query order and a recording's agree whatever they are
force_inline u64 nc_map_hash_add(u64 h, u32 cell) { return h + nc_hash((u64)cell + 0x51ed27u); }

typedef struct NcMapServer {
  u8 *asked;  // [max_conns] the connection wants the map
  u32 *cells; // [max_ghosts] this second's dots
  u32 max_conns, max_ghosts;
  f32 half; // the map's square, metres from the centre
  EcsEntity stats; // the stats ghost
  u32 seq;          // maps sent
} NcMapServer;

typedef struct NcMapClient {
  u32 *cells; // [cap] the second being received
  u32 cap, got, total, second;
  u32 shown_second, shown_count; // the newest complete second
  b32 want;  // --map or the M key: the camera and spectators ask, players do not pay for it
  b32 asked; // what the server was last told
  b32 complete; // a second finished since the texture was painted
  u8 *pixels;   // [NC_MAP_PX * NC_MAP_PX * 4]
  u16 *density; // [NC_MAP_PX * NC_MAP_PX]
  GpuTexture tex;
  NcStats stats; // the newest the stats ghost showed
  b32 has_stats;
} NcMapClient;

// main lane: the stats ghost, from --instance and --price-hour
hz_internal void nc_map_server_init(NcMapServer *m, EcsWorld *w, Allocator *a, u32 max_conns, u32 max_ghosts) {
  m->max_conns = max_conns;
  m->max_ghosts = max_ghosts;
  m->asked = ALLOC_ARRAY(a, u8, max_conns);
  m->cells = ALLOC_ARRAY(a, u32, max_ghosts);
  NcStats st = {.lanes = tctx_current()->thread_count};
  const char *instance = hz_arg_value("--instance");
  for (u32 i = 0; instance && instance[i] && i < 4 * NC_INSTANCE_WORDS; i++)
    st.instance[i / 4] |= (u32)(u8)instance[i] << (8 * (i % 4));
  const char *price = hz_arg_value("--price-hour");
  f64 dollars = 0.0;
  if (price && cstr_to_f64((CString)price, &dollars))
    st.milli_dollars_hour = (u32)(dollars * 1000.0 + 0.5);
  NcSide *side = ecs_resource_get_mut(w, NcSide);
  side->map_asked = m->asked;
  side->max_conns = max_conns;
  m->half = m_ceilf(side->spawn_half + 2.0f);
  st.map_half_dm = (u32)(m->half * 10.0f);
  m->stats = ecs_entity_create(w);
  ecs_component_set(w, m->stats, NcStats, st);
  ecs_component_set(w, m->stats, NetGhost, {.type = NET_GHOST_ID(NcStatsGhost)});
}

// main lane: every player's cell, and their order-free hash
hz_internal u32 nc_map_server_gather(NcMapServer *m, EcsWorld *w, u64 *hash) {
  u32 n = 0;
  *hash = 0;
  ecs_query_foreach(ecs_query(w, EcsIn(NcBody), EcsIn(NetGhost), EcsWith(NcInput)), it) {
    const NcBody *b = ecs_iter_get_component(&it, NcBody, 0);
    for (i32 k = 0; k < it.count && n < m->max_ghosts; k++) {
      m->cells[n] = nc_map_cell(b[k].pos, m->half);
      *hash = nc_map_hash_add(*hash, m->cells[n++]);
    }
  }
  return n;
}

// main lane: the gathered map to every connection that asked, as map `seq`
hz_internal void nc_map_server_send(NcMapServer *m, Netcode *net, u32 seq, u32 n) {
  NetcodeServer *srv = netcode_server(net);
  u32 askers = 0;
  for (u32 c = 0; c < m->max_conns; c++) {
    if (m->asked[c] && !netcode_server_verified(srv, c))
      m->asked[c] = 0; // left: a connection reusing the id asks for itself
    askers += m->asked[c];
  }
  if (!askers)
    return;
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  NcMapChunk *chunk = ALLOC(&ta.allocator, NcMapChunk);
  for (u32 first = 0; first < n || first == 0; first += NC_MAP_CHUNK) {
    chunk->second = seq;
    chunk->first = first;
    chunk->total = n;
    chunk->count = MIN(NC_MAP_CHUNK, n - first);
    mem_cpy(chunk->cells, m->cells + first, chunk->count * sizeof(u32));
    mem_set(chunk->cells + chunk->count, 0, (NC_MAP_CHUNK - chunk->count) * sizeof(u32));
    for (u32 c = 0; c < m->max_conns; c++)
      if (m->asked[c])
        netcode_rpc(net, c, NcMapChunk, chunk);
    if (!n)
      break;
  }
  tctx_temp_allocator_end(ta);
}

// main lane, once a second with the server report: the stats ghost's numbers, the map's hash; the players
hz_internal u32 nc_map_server_second(NcMapServer *m, EcsWorld *w, Netcode *net, u32 second, u32 p50_us, u32 p99_us) {
  u64 hash = 0;
  u32 n = nc_map_server_gather(m, w, &hash);
  // the replay test: a replay of the recording logs the same line for this second
  log_info("nc map server: second % tick % dots % map hash %", fmt_u32(second), fmt_u32(netcode_stats(net).tick), fmt_u32(n),
           fmt_hex(hash));
  const NcSide *side = ecs_resource_get(w, NcSide);
  NcStats *st = ecs_component_get_mut(w, m->stats, NcStats);
  st->second = second;
  st->connected = n;
  st->tick_p50_us = p50_us;
  st->tick_p99_us = p99_us;
  if (side->royale.on) {
    st->royale = 1;
    st->royale_start = side->royale.start;
    st->shrink_ticks = side->royale.shrink_ticks;
    st->zone_x_mm = (i32)net_q_f32(side->royale.centre.x, NC_GRID);
    st->zone_z_mm = (i32)net_q_f32(side->royale.centre.y, NC_GRID);
    st->zone_r0_mm = (u32)(side->royale.r0 * NC_GRID);
    st->winner = side->winner;
    st->winner_seed = side->winner_seed;
  }
  m->seq++;
  nc_map_server_send(m, net, m->seq, n);
  return n;
}

hz_internal void nc_map_client_init(NcMapClient *m, Allocator *a) {
  m->cap = 1024;
  m->cells = ALLOC_ARRAY(a, u32, m->cap);
  m->pixels = ALLOC_ARRAY(a, u8, NC_MAP_PX * NC_MAP_PX * 4);
  m->density = ALLOC_ARRAY(a, u16, NC_MAP_PX * NC_MAP_PX);
}

// main lane, each frame after network in: tell the server what it wants, gather the chunks, the stats ghost's numbers
hz_internal void nc_map_client_frame(NcMapClient *m, EcsWorld *w, Netcode *net, Allocator *a) {
  if (m->want != m->asked && netcode_state(net) == NETCODE_RUNNING) {
    netcode_rpc(net, NETCODE_TO_SERVER, NcMapAsk, &((NcMapAsk){.on = m->want}));
    m->asked = m->want;
  }
  ecs_query_foreach(ecs_query(w, EcsIn(NcStats), EcsWith(NetGhost)), it) {
    NcStats st = ecs_iter_get_component(&it, NcStats, 0)[0];
    if (!m->has_stats || st.second != m->stats.second)
      log_info("nc overlay: second % players % tick us p50 % p99 %", fmt_u32(st.second), fmt_u32(st.connected),
               fmt_u32(st.tick_p50_us), fmt_u32(st.tick_p99_us));
    m->stats = st;
    m->has_stats = true;
  }
  ecs_query_foreach(ecs_query(w, EcsIn(NcMapChunk), EcsWith(NetRpcReceived)), it) {
    const NcMapChunk *ch = ecs_iter_get_component(&it, NcMapChunk, 0);
    for (i32 k = 0; k < it.count; k++) {
      const NcMapChunk *c = &ch[k];
      if (c->second != m->second || c->first == 0) {
        m->second = c->second;
        m->got = 0;
        m->total = c->total;
      }
      if (m->total > m->cap) {
        m->cap = m->total;
        m->cells = REALLOC_ARRAY(a, m->cells, u32, m->cap);
      }
      if (c->first + c->count > m->total || c->count > NC_MAP_CHUNK)
        continue;
      mem_cpy(m->cells + c->first, c->cells, c->count * sizeof(u32));
      m->got += c->count;
      if (m->got == m->total) {
        m->shown_second = m->second;
        m->shown_count = m->total;
        m->complete = true;
      }
    }
  }
}

// the battle royale's rules as the stats ghost carries them
hz_internal NcRoyale nc_map_royale(const NcStats *st) {
  return (NcRoyale){.on = st->royale != 0, .start = st->royale_start, .shrink_ticks = st->shrink_ticks,
                    .centre = {net_dq_f32((u32)st->zone_x_mm, NC_GRID), net_dq_f32((u32)st->zone_z_mm, NC_GRID)},
                    .r0 = (f32)st->zone_r0_mm / NC_GRID};
}

// main lane, in the UI frame: a finished map painted as density with this player's own dot and the zone, uploaded once
hz_internal void nc_map_client_paint(NcMapClient *m, b32 has_own, v3 own, NetTick tick) {
  if (!m->complete || !m->stats.map_half_dm)
    return;
  m->complete = false;
  mem_set(m->density, 0, NC_MAP_PX * NC_MAP_PX * sizeof(u16));
  u32 shift = NC_MAP_BITS - 8;
  for (u32 i = 0; i < m->shown_count; i++) {
    u32 c = m->cells[i], x = (c >> NC_MAP_BITS) >> shift, z = (c & ((1u << NC_MAP_BITS) - 1)) >> shift;
    u16 *d = &m->density[(NC_MAP_PX - 1 - z) * NC_MAP_PX + x];
    *d = *d < 0xffff ? *d + 1 : *d;
  }
  for (u32 p = 0; p < NC_MAP_PX * NC_MAP_PX; p++) {
    u32 d = m->density[p];
    u8 *px = m->pixels + p * 4;
    // one player a dim teal pixel, a crowd saturates to white
    u32 v = d ? MIN(255u, 110u + d * 36u) : 0u;
    px[0] = (u8)(d ? MIN(255u, 40u + d * 30u) : 18u);
    px[1] = (u8)(d ? v : 22u);
    px[2] = (u8)(d ? MIN(255u, v + 20u) : 30u);
    px[3] = 230;
  }
  if (has_own) {
    u32 c = nc_map_cell(own, (f32)m->stats.map_half_dm / 10.0f), shift2 = NC_MAP_BITS - 8;
    i32 ox = (i32)((c >> NC_MAP_BITS) >> shift2), oz = NC_MAP_PX - 1 - (i32)((c & ((1u << NC_MAP_BITS) - 1)) >> shift2);
    for (i32 dz = -2; dz <= 2; dz++)
      for (i32 dx = -2; dx <= 2; dx++) {
        i32 x = ox + dx, z = oz + dz;
        if (x < 0 || z < 0 || x >= NC_MAP_PX || z >= NC_MAP_PX)
          continue;
        u8 *px = m->pixels + ((u32)z * NC_MAP_PX + (u32)x) * 4;
        px[0] = 255;
        px[1] = 214;
        px[2] = 64;
        px[3] = 255;
      }
  }
  NcRoyale z = nc_map_royale(&m->stats);
  if (z.on) {
    // the zone's edge, a pixel a step
    f32 half = (f32)m->stats.map_half_dm / 10.0f, r = nc_zone_radius(&z, tick), px_m = (f32)NC_MAP_PX / (2.0f * half);
    u32 steps = (u32)m_clampf(2.0f * M_PI * r * px_m, 16.0f, 4096.0f);
    for (u32 k = 0; k < steps; k++) {
      f32 a = (f32)k / (f32)steps * 2.0f * M_PI;
      i32 x = (i32)((z.centre.x + m_cosf(a) * r + half) * px_m), y = NC_MAP_PX - 1 - (i32)((z.centre.y + m_sinf(a) * r + half) * px_m);
      if (x < 0 || y < 0 || x >= NC_MAP_PX || y >= NC_MAP_PX)
        continue;
      u8 *px = m->pixels + ((u32)y * NC_MAP_PX + (u32)x) * 4;
      px[0] = 90;
      px[1] = 160;
      px[2] = 255;
      px[3] = 255;
    }
  }
  if (!handle_is_valid(m->tex))
    m->tex = gpu_make_texture(&(GpuTextureDesc){.data = m->pixels,
                                                .size = NC_MAP_PX * NC_MAP_PX * 4u,
                                                .data_type = GPU_TEXTURE_DATA_RGBA8,
                                                .width = NC_MAP_PX,
                                                .height = NC_MAP_PX,
                                                .is_srgb = true});
  else
    gpu_update_texture_region(m->tex, 0, 0, NC_MAP_PX, NC_MAP_PX, m->pixels);
  log_info("nc map: second % dots % (overlay: second % players %)", fmt_u32(m->shown_second), fmt_u32(m->shown_count),
           fmt_u32(m->stats.second), fmt_u32(m->stats.connected));
}
