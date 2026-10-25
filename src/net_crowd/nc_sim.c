// net_crowd's tick systems: walking (both sides; a client only what it predicts), and the server's joins

force_inline v3 nc_grid3(v3 v, f32 scale) {
  return (v3){net_dq_f32(net_q_f32(v.x, scale), scale), net_dq_f32(net_q_f32(v.y, scale), scale), net_dq_f32(net_q_f32(v.z, scale), scale)};
}

// the same on the wire: floats by their grid step, never by bits (the fast-math build may keep a -0 the wire sends as 0)
force_inline b32 nc_same3(v3 a, v3 b, f32 scale) {
  return net_q_f32(a.x, scale) == net_q_f32(b.x, scale) && net_q_f32(a.y, scale) == net_q_f32(b.y, scale) &&
         net_q_f32(a.z, scale) == net_q_f32(b.z, scale);
}

hz_internal b32 nc_state_same(const NcBody *b, const NcMotion *m, const NcBody *b0, const NcMotion *m0) {
  return nc_same3(b->pos, b0->pos, NC_GRID) && net_q_f32(b->facing.x, NC_GRID) == net_q_f32(b0->facing.x, NC_GRID) &&
         net_q_f32(b->facing.y, NC_GRID) == net_q_f32(b0->facing.y, NC_GRID) && nc_same3(m->velocity, m0->velocity, NC_VEL_GRID) &&
         net_q_f32(m->yaw, NC_ANGLE_GRID) == net_q_f32(m0->yaw, NC_ANGLE_GRID) && m->ground == m0->ground &&
         nc_same3(m->ground_normal, m0->ground_normal, NC_ANGLE_GRID) &&
         net_q_f32(m->coyote, NC_TIME_GRID) == net_q_f32(m0->coyote, NC_TIME_GRID) &&
         net_q_f32(m->jump_buffer, NC_TIME_GRID) == net_q_f32(m0->jump_buffer, NC_TIME_GRID);
}

// one tick of the engine's kinematic controller from the replicated state, back onto the wire's grids
hz_internal void nc_move(EcsWorld *w, const CharacterTuning *t, NcBody *b, NcMotion *m, v2 move, f32 dt) {
  b32 still = move.x == 0.0f && move.y == 0.0f;
  if (still && m->rest)
    return;
  NcBody b0 = *b;
  NcMotion m0 = *m;
  Character c = {.position = b->pos, .velocity = m->velocity, .yaw = m->yaw, .render_position = b->pos,
                 .ground = (CharacterGround)m->ground, .ground_normal = m->ground_normal, .coyote = m->coyote,
                 .jump_buffer = m->jump_buffer, .placed = true};
  // world +x and +z: the heading frame at heading 0 has x right and y toward -z
  CharacterInput in = {.move = {move.x, -move.y}};
  WorldTransform wt = {.position = b->pos, .rotation = quat_identity(), .scale = V3_ONE};
  character_kinematic_step(w, t, &c, &in, &wt, dt);
  b->pos = nc_grid3(c.position, NC_GRID);
  b->facing = (v2){nc_grid(m_sinf(c.yaw)), nc_grid(m_cosf(c.yaw))};
  // the timers only matter above 0: held there, they stay on a short range
  *m = (NcMotion){.velocity = nc_grid3(c.velocity, NC_VEL_GRID),
                  .yaw = net_dq_f32(net_q_f32(c.yaw, NC_ANGLE_GRID), NC_ANGLE_GRID),
                  .ground = (u32)c.ground,
                  .ground_normal = nc_grid3(c.ground_normal, NC_ANGLE_GRID),
                  .coyote = net_dq_f32(net_q_f32(m_maxf(c.coyote, 0.0f), NC_TIME_GRID), NC_TIME_GRID),
                  .jump_buffer = net_dq_f32(net_q_f32(m_maxf(c.jump_buffer, 0.0f), NC_TIME_GRID), NC_TIME_GRID)};
  m->rest = still && nc_state_same(b, m, &b0, &m0);
}

// the mover reads colliders and one-way marks; no CharacterSurface: a step starts with no ground entity, so no carry reads it
ecs_system(nc_walk, EcsIn(NcInput), EcsInOut(NcBody), EcsInOut(NcMotion), EcsResIn(EcsPhysicsWorld), EcsExternIn(ColliderBody),
           EcsExternIn(CharacterOneWay)) {
  CharacterTuning t = nc_tuning();
  for (i32 i = 0; i < it->count; i++)
    nc_move(it->world, &t, ecs_sys_get_component_mut(NcBody, i), ecs_sys_get_component_mut(NcMotion, i),
            ecs_sys_get_component(NcInput, i)->move, it->delta_time);
}

// a client: only what it predicts, from the tick its prediction starts
ecs_system(nc_walk_predicted, EcsIn(NcInput), EcsInOut(NcBody), EcsInOut(NcMotion), EcsIn(NetPredicted), EcsResIn(NetSimTime),
           EcsResIn(EcsPhysicsWorld), EcsExternIn(ColliderBody), EcsExternIn(CharacterOneWay)) {
  NetTick tick = net_sim_tick(ecs_sys_get_resource(NetSimTime), it->step);
  CharacterTuning t = nc_tuning();
  for (i32 i = 0; i < it->count; i++) {
    if (!net_tick_newer(tick, ecs_sys_get_component(NetPredicted, i)->start))
      continue;
    nc_move(it->world, &t, ecs_sys_get_component_mut(NcBody, i), ecs_sys_get_component_mut(NcMotion, i),
            ecs_sys_get_component(NcInput, i)->move, it->delta_time);
  }
}

// server: each connection's view follows its avatar (one connection an avatar, so lanes never share one)
ecs_system(nc_views, EcsIn(NcBody), EcsIn(NetGhost), EcsWith(NcInput), EcsResIn(NcSide)) {
  NetcodeServer *srv = ecs_sys_get_resource(NcSide)->srv;
  for (i32 i = 0; i < it->count; i++) {
    u32 owner = ecs_sys_get_component(NetGhost, i)->owner;
    if (owner)
      netcode_server_set_view(srv, owner - 1, ecs_sys_get_component(NcBody, i)->pos, NC_VIEW_RADIUS);
  }
}

// server: an avatar per joining connection, placed and dressed by its client id; people past the human cap spectate
hz_internal void nc_spectator_new(NcSide *side, u32 conn, NcSpectateWhy why, u32 place, b32 human) {
  side->humans -= side->kind[conn] == NC_CONN_HUMAN;
  side->kind[conn] = NC_CONN_SPECTATOR;
  side->spectators_human[side->spectators_new_count] = human;
  side->spectators_why[side->spectators_new_count] = why;
  side->spectators_place[side->spectators_new_count] = place;
  side->spectators_new[side->spectators_new_count++] = conn;
}

ecs_system(nc_join, EcsIn(NetConnection), EcsWith(NetConnectionJoined), EcsResIn(NetSimTime), EcsResInOut(NcSide)) {
  NcSide *side = ecs_sys_get_resource_mut(NcSide);
  NetTick tick = net_sim_tick(ecs_sys_get_resource(NetSimTime), it->step);
  b32 running = side->royale.on && net_tick_since(tick, side->royale.start) >= 0;
  for (i32 i = 0; i < it->count; i++) {
    const NetConnection *c = ecs_sys_get_component(NetConnection, i);
    b32 human = c->client_id >= NC_HUMAN_ID;
    if (running || (human && side->humans >= side->human_cap)) {
      nc_spectator_new(side, c->id, running ? NC_SPECTATE_RUNNING : NC_SPECTATE_CAP, 0, human);
      netcode_server_set_view(side->srv, c->id, V3_ZERO, NC_SPECTATOR_RADIUS);
      continue;
    }
    side->kind[c->id] = human ? NC_CONN_HUMAN : NC_CONN_BOT;
    side->humans += human;
    v3 pos = nc_spawn(c->client_id, side->spawn_half);
    u32 h = nc_hash(c->client_id ^ 0x9e3779b97f4a7c15ull);
    EcsEntity e = ecs_entity_create(it->world);
    ecs_component_set(it->world, e, NcBody, {.pos = pos, .facing = {0.0f, 1.0f}});
    ecs_component_set(it->world, e, NcMotion, {.ground = CHARACTER_GROUND_IN_AIR, .ground_normal = V3_UP});
    ecs_component_set(it->world, e, NcLook, {.name_seed = h, .colour = (h >> 8) % NC_COLOURS, .body = (h >> 16) & 1});
    ecs_component_set(it->world, e, NcInput, {0});
    if (side->royale.on)
      ecs_component_set(it->world, e, NcVitals, {.hp = (i32)NC_HP});
    ecs_component_set(it->world, e, NetGhost, {.type = NET_GHOST_ID(NcAvatarGhost), .owner = NET_OWNER(c->id)});
    // its first snapshot already sees the crowd around it, not the default view
    netcode_server_set_view(side->srv, c->id, pos, NC_VIEW_RADIUS);
  }
}

// server: a person who left frees a place under the cap
ecs_system(nc_leave, EcsIn(NetConnection), EcsWith(NetConnectionLeft), EcsResInOut(NcSide)) {
  NcSide *side = ecs_sys_get_resource_mut(NcSide);
  for (i32 i = 0; i < it->count; i++) {
    u32 id = ecs_sys_get_component(NetConnection, i)->id;
    side->humans -= side->kind[id] == NC_CONN_HUMAN;
    side->kind[id] = NC_CONN_NONE;
  }
}

// server: a spectator's view follows its camera
ecs_system(nc_spectator_views, EcsIn(NcSpectatorAt), EcsIn(NetRpcReceived), EcsResIn(NcSide)) {
  const NcSide *side = ecs_sys_get_resource(NcSide);
  for (i32 i = 0; i < it->count; i++) {
    u32 conn = ecs_sys_get_component(NetRpcReceived, i)->connection;
    if (conn < side->max_conns && side->kind[conn] == NC_CONN_SPECTATOR)
      netcode_server_set_view(side->srv, conn, ecs_sys_get_component(NcSpectatorAt, i)->pos, NC_SPECTATOR_RADIUS);
  }
}

// server: who wants the map; an RPC's entity lives until the tick's gather, so a tick system reads it
ecs_system(nc_map_asks, EcsIn(NcMapAsk), EcsIn(NetRpcReceived), EcsResIn(NcSide)) {
  const NcSide *side = ecs_sys_get_resource(NcSide);
  for (i32 i = 0; i < it->count; i++) {
    u32 conn = ecs_sys_get_component(NetRpcReceived, i)->connection;
    if (side->map_asked && conn < side->max_conns)
      side->map_asked[conn] = ecs_sys_get_component(NcMapAsk, i)->on != 0;
  }
}

// ---- battle royale, server ----

// the drop: at the start tick every player is in the air over where it stood
ecs_system(nc_royale_drop, EcsInOut(NcBody), EcsWith(NcVitals), EcsResIn(NetSimTime), EcsResIn(NcSide)) {
  const NcSide *side = ecs_sys_get_resource(NcSide);
  if (net_sim_tick(ecs_sys_get_resource(NetSimTime), it->step) != side->royale.start)
    return;
  for (i32 i = 0; i < it->count; i++)
    ecs_sys_get_component_mut(NcBody, i)->pos.y = NC_DROP_HEIGHT;
}

// a landed player outside the zone loses health
ecs_system(nc_royale_zone, EcsIn(NcBody), EcsInOut(NcVitals), EcsResIn(NetSimTime), EcsResIn(NcSide)) {
  const NcRoyale *z = &ecs_sys_get_resource(NcSide)->royale;
  NetTick tick = net_sim_tick(ecs_sys_get_resource(NetSimTime), it->step);
  if (net_tick_since(tick, z->start) <= (i32)NC_DROP_TICKS)
    return;
  f32 r = nc_zone_radius(z, tick);
  i32 dmg = (i32)nc_zone_damage(z, tick);
  for (i32 i = 0; i < it->count; i++) {
    const NcBody *b = ecs_sys_get_component(NcBody, i);
    v2 d = {b->pos.x - z->centre.x, b->pos.z - z->centre.y};
    NcVitals *v = ecs_sys_get_component_mut(NcVitals, i);
    if (v->hp > 0 && b->pos.y <= 0.0f && v2_dot(d, d) > r * r)
      v->hp -= dmg;
  }
}

ecs_system(nc_royale_begin, EcsResInOut(NcSide)) {
  NcSide *side = ecs_sys_get_resource_mut(NcSide);
  side->dead_count = 0;
  side->alive = 0;
}

// who is out this tick, and who is left
ecs_system(nc_royale_count, EcsIn(NcVitals), EcsWith(NetGhost), EcsResInOut(NcSide)) {
  NcSide *side = ecs_sys_get_resource_mut(NcSide);
  for (i32 i = 0; i < it->count; i++) {
    i32 hp = ecs_sys_get_component(NcVitals, i)->hp;
    if (hp > 0) {
      side->alive++;
      side->last_alive = it->entities[i];
    } else if (hp != NC_HP_OUT && side->dead_count < side->max_conns) {
      side->dead[side->dead_count++] = it->entities[i];
    }
  }
}

// a tick that would leave nobody keeps the one with the most health left, ties to the lowest connection; the rest are out
ecs_system(nc_royale_end, EcsResInOut(NcSide)) {
  NcSide *side = ecs_sys_get_resource_mut(NcSide);
  if (!side->dead_count)
    return;
  if (!side->alive) {
    u32 best = 0;
    for (u32 k = 1; k < side->dead_count; k++) {
      const NcVitals *a = ecs_component_get(it->world, side->dead[k], NcVitals);
      const NcVitals *b = ecs_component_get(it->world, side->dead[best], NcVitals);
      u32 oa = ecs_component_get(it->world, side->dead[k], NetGhost)->owner;
      u32 ob = ecs_component_get(it->world, side->dead[best], NetGhost)->owner;
      if (a->hp > b->hp || (a->hp == b->hp && oa < ob))
        best = k;
    }
    ecs_component_get_mut(it->world, side->dead[best], NcVitals)->hp = 1;
    side->last_alive = side->dead[best];
    side->dead[best] = side->dead[--side->dead_count];
    side->alive = 1;
  }
  for (u32 k = 0; k < side->dead_count; k++) {
    u32 owner = ecs_component_get(it->world, side->dead[k], NetGhost)->owner;
    if (owner)
      nc_spectator_new(side, owner - 1, NC_SPECTATE_OUT, side->alive + 1, side->kind[owner - 1] == NC_CONN_HUMAN);
    // the destroy lands at the flush's end; later ticks of this flush must not count it again
    ecs_component_get_mut(it->world, side->dead[k], NcVitals)->hp = NC_HP_OUT;
    ecs_entity_destroy(it->world, side->dead[k]);
  }
  if (side->alive == 1 && !side->winner) {
    side->winner = true;
    side->winner_seed = ecs_component_get(it->world, side->last_alive, NcLook)->name_seed;
  }
}

hz_internal void nc_schedule_tick(EcsWorld *w, b32 server) {
  b32 royale = server && ecs_resource_get(w, NcSide)->royale.on;
  if (server) {
    ecs_schedule_single(w, nc_leave);
    ecs_schedule_single(w, nc_join);
    ecs_schedule_single(w, nc_map_asks);
    ecs_schedule_single(w, nc_spectator_views);
  }
  if (royale)
    ecs_schedule(w, nc_royale_drop);
  // the plaza's colliders never change: their layers are built once, at the first tick
  if (!ecs_physics_layer_build_count(w, ecs_resource_get(w, EcsPhysicsWorld)->static_layer))
    ecs_physics_schedule(w);
  if (server)
    ecs_schedule(w, nc_walk);
  else
    ecs_schedule(w, nc_walk_predicted);
  if (server)
    ecs_schedule(w, nc_views);
  if (royale) {
    ecs_schedule(w, nc_royale_zone);
    ecs_schedule_single(w, nc_royale_begin);
    ecs_schedule_single(w, nc_royale_count);
    ecs_schedule_single(w, nc_royale_end);
  }
}
