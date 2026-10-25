// net_crowd's client presentation: a skinned Kaigen bot per ghost, tinted by its look, and the name tags over them

#define NC_BOT_PATH_MALE "net_crowd/characters/kaigen_bot"
#define NC_BOT_PATH_FEMALE "net_crowd/characters/kaigen_bot_female"
#define NC_SPRINT_SPEED 4.0f   // m/s the sprint clip's stride covers at rate 1
#define NC_RUN_ABOVE 0.35f     // m/s: faster runs, slower idles
#define NC_TAG_HEIGHT 2.05f    // metres above the feet
#define NC_TAG_FADE_FROM 25.0f // metres: tags beyond fade out over the next 10
#define NC_TAG_ALWAYS 64       // the nearest tags never fade

typedef struct NcBotAssets {
  HzHandleT(ModelBlobAsset) model[2];
  HzHandleT(AnimationBlobAsset) idle[2], run[2];
  HzHandleT(MaterialBlobAsset) look[2][NC_COLOURS];
} NcBotAssets;

hz_internal void nc_bot_assets_load(NcBotAssets *b, AssetSystem *assets) {
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  for (u32 k = 0; k < 2; k++) {
    const char *base = k ? NC_BOT_PATH_FEMALE : NC_BOT_PATH_MALE;
    b->model[k] = asset_load(ModelBlobAsset, assets, .path = str_format(&ta.allocator, "%.glb", fmt_cstr(base)).value);
    b->idle[k] = asset_load(AnimationBlobAsset, assets,
                            .path = str_format(&ta.allocator, "%/Idle_Loop.hza", fmt_cstr(base)).value);
    b->run[k] = asset_load(AnimationBlobAsset, assets,
                           .path = str_format(&ta.allocator, "%/Sprint_Loop.hza", fmt_cstr(base)).value);
    for (u32 c = 0; c < NC_COLOURS; c++)
      b->look[k][c] = asset_load(MaterialBlobAsset, assets,
                                 .path = str_format(&ta.allocator, "net_crowd/characters/materials/bot_%_%.hzt",
                                                    fmt_cstr(k ? "female" : "male"), fmt_u32(c))
                                             .value);
  }
  tctx_temp_allocator_end(ta);
}

hz_internal b32 nc_bot_assets_ready(const NcBotAssets *b, AssetSystem *assets) {
  b32 ok = true;
  for (u32 k = 0; k < 2; k++)
    ok = ok && asset_is_ready(assets, b->model[k]) && asset_is_ready(assets, b->idle[k]) && asset_is_ready(assets, b->run[k]);
  for (u32 k = 0; k < 2; k++)
    for (u32 c = 0; c < NC_COLOURS; c++)
      ok = ok && asset_is_ready(assets, b->look[k][c]);
  return ok;
}

hz_internal quat nc_facing_rot(v2 f) { return quat_from_euler((v3){0, m_atan2f(f.x, f.y), 0}); }

// main lane, outside flushes, after the frame's snapshots and prediction (or a replay's frame): bots follow their bodies
hz_internal void nc_visuals(EcsWorld *w, const NcBotAssets *b, NetcodeClient *c, f32 dt) {
  AssetSystem *assets = ecs_resource_get_mut(w, AssetSystem);
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  u32 cap = 0;
  ecs_query_foreach(ecs_query(w, EcsWith(NcBody), EcsWith(NcLook)), it) cap += (u32)it.count;
  EcsEntity *fresh = ALLOC_ARRAY_NO_ZERO(&ta.allocator, EcsEntity, cap + 1);
  u32 fresh_n = 0;
  ecs_query_foreach(ecs_query(w, EcsWith(NcBody), EcsWith(NcLook), EcsNot(NcVisual)), it) {
    for (i32 k = 0; k < it.count && fresh_n < cap; k++)
      fresh[fresh_n++] = it.entities[k];
  }
  for (u32 i = 0; i < fresh_n; i++) {
    const NcBody *body = ecs_component_get(w, fresh[i], NcBody);
    const NcLook *look = ecs_component_get(w, fresh[i], NcLook);
    u32 k = look->body & 1;
    Transform t = {.position = body->pos, .rotation = nc_facing_rot(body->facing), .scale = V3_ONE};
    EcsEntity bot = ecs_entity_create(w);
    ecs_component_set(w, bot, WorldTransform, {.position = t.position, .rotation = t.rotation, .scale = V3_ONE});
    ecs_component_set(w, bot, EcsModelRef, {.model = blob_asset_ref_from_handle(ModelBlobAsset, b->model[k])});
    ecs_component_set(w, bot, EcsAnimationRef, {.animation = blob_asset_ref_from_handle(AnimationBlobAsset, b->idle[k])});
    ecs_component_set(w, bot, EcsVisibility, {.visible = true, .casts_shadow = true});
    ecs_component_set(w, bot, NcVisualOf, {.ghost = fresh[i]});
    ecs_component_set(w, fresh[i], NcVisual, {.bot = bot, .last = body->pos});
  }
  // bots whose ghost left the view
  u32 bots = 0, gone_n = 0;
  ecs_query_foreach(ecs_query(w, EcsWith(NcVisualOf)), it) bots += (u32)it.count;
  EcsEntity *gone = ALLOC_ARRAY_NO_ZERO(&ta.allocator, EcsEntity, bots + 1);
  ecs_query_foreach(ecs_query(w, EcsIn(NcVisualOf)), it) {
    const NcVisualOf *of = ecs_iter_get_component(&it, NcVisualOf, 0);
    for (i32 k = 0; k < it.count && gone_n < bots; k++)
      if (!ecs_entity_is_alive(w, of[k].ghost))
        gone[gone_n++] = it.entities[k];
  }
  for (u32 i = 0; i < gone_n; i++)
    ecs_entity_destroy(w, gone[i]);
  // the field carries no facing: a body it shows faces where it moves
  if (c)
    ecs_query_foreach(ecs_query(w, EcsInOut(NcBody), EcsIn(NetGhost), EcsIn(NcVisual)), it) {
      NcBody *body = ecs_iter_get_component(&it, NcBody, 0);
      const NetGhost *g = ecs_iter_get_component(&it, NetGhost, 1);
      const NcVisual *v = ecs_iter_get_component(&it, NcVisual, 2);
      for (i32 i = 0; i < it.count; i++) {
        if (netcode_client_ghost_tier(c, g[i].id, NULL) <= NETCODE_TIER_NEAR)
          continue;
        v2 d = {body[i].pos.x - v[i].last.x, body[i].pos.z - v[i].last.z};
        f32 len = v2_length(d);
        if (len > 0.002f)
          body[i].facing = (v2){d.x / len, d.y / len};
      }
    }
  // pose, speed and clip; a bot's submesh takes its colour once the model has spawned it
  ecs_query_foreach(ecs_query(w, EcsIn(NcBody), EcsIn(NcLook), EcsInOut(NcVisual)), it) {
    const NcBody *body = ecs_iter_get_component(&it, NcBody, 0);
    const NcLook *look = ecs_iter_get_component(&it, NcLook, 1);
    NcVisual *v = ecs_iter_get_component(&it, NcVisual, 2);
    for (i32 i = 0; i < it.count; i++) {
      u32 k = look[i].body & 1;
      v3 d = v3_sub(body[i].pos, v[i].last);
      f32 inst = dt > 1e-4f ? v2_length((v2){d.x, d.z}) / dt : 0.0f;
      v[i].speed += (m_minf(inst, 8.0f) - v[i].speed) * m_minf(dt * 10.0f, 1.0f);
      v[i].last = body[i].pos;
      // a root: its world transform is the one written (a lone LocalTransform is copied only once)
      ecs_component_set(w, v[i].bot, WorldTransform,
                        {.position = body[i].pos, .rotation = nc_facing_rot(body[i].facing), .scale = V3_ONE});
      EcsAnimationLayer *al = ecs_component_get_mut(w, v[i].bot, EcsAnimationLayer);
      if (al && asset_is_ready(assets, b->idle[k]) && asset_is_ready(assets, b->run[k])) {
        b32 run = v[i].speed > (v[i].running ? NC_RUN_ABOVE * 0.7f : NC_RUN_ABOVE);
        HzHandleT(AnimationBlobAsset) clip = run ? b->run[k] : b->idle[k];
        AnimationBlobAsset *blob = asset_get(assets, clip, AnimationBlobAsset);
        if (animation_layer_current_animation(&al->layer) != blob && !animation_layer_has_animation(&al->layer, blob))
          animation_layer_play_ex(&al->layer, blob, ANIM_TRANSITION_CROSSFADE, 0.2f, 1.0f, true, 0.0f);
        animation_layer_set_speed(&al->layer, run ? m_clampf(v[i].speed / NC_SPRINT_SPEED, 0.2f, 1.5f) : 1.0f);
        v[i].running = run;
      }
      const EcsSubMeshChildren *kids = v[i].tinted ? NULL : ecs_component_get(w, v[i].bot, EcsSubMeshChildren);
      if (kids && kids->by_slot.len) {
        for (u32 s = 0; s < kids->by_slot.len; s++)
          ecs_component_set(w, kids->by_slot.items[s], EcsMaterial,
                            {.material = blob_asset_ref_from_handle(MaterialBlobAsset, b->look[k][look[i].colour % NC_COLOURS])});
        v[i].tinted = true;
      }
    }
  }
  tctx_temp_allocator_end(ta);
}

// main lane, inside the UI frame: each player's name over its head, nearest first; far ones fade but the `always` nearest
hz_internal void nc_name_tags(EcsWorld *w, const Camera *cam, const AppMemory *memory, u32 always) {
  // the frame's allocator: the UI reads the names when it draws, after this returns
  Allocator fa = tctx_temp_allocator(NULL);
  u32 cap = 0;
  ecs_query_foreach(ecs_query(w, EcsWith(NcBody), EcsWith(NcLook)), it) cap += (u32)it.count;
  u32 *keys = ALLOC_ARRAY_NO_ZERO(&fa, u32, cap + 1), *idx = ALLOC_ARRAY_NO_ZERO(&fa, u32, cap + 1);
  u32 *tk = ALLOC_ARRAY_NO_ZERO(&fa, u32, cap + 1), *tv = ALLOC_ARRAY_NO_ZERO(&fa, u32, cap + 1);
  v3 *at = ALLOC_ARRAY_NO_ZERO(&fa, v3, cap + 1);
  u32 *seed = ALLOC_ARRAY_NO_ZERO(&fa, u32, cap + 1);
  u32 n = 0;
  ecs_query_foreach(ecs_query(w, EcsIn(NcBody), EcsIn(NcLook)), it) {
    const NcBody *body = ecs_iter_get_component(&it, NcBody, 0);
    const NcLook *look = ecs_iter_get_component(&it, NcLook, 1);
    for (i32 i = 0; i < it.count && n < cap; i++) {
      at[n] = v3_add(body[i].pos, (v3){0, NC_TAG_HEIGHT, 0});
      seed[n] = look[i].name_seed;
      keys[n] = (u32)m_minf(v3_length(v3_sub(at[n], cam->pos)) * 100.0f, 4e9f);
      idx[n] = n;
      n++;
    }
  }
  RadixSortU32Out o = radix_sort_u32(keys, idx, tk, tv, n, (RadixSortDesc){0});
  f32 sw = (f32)memory->canvas_width, sh = (f32)memory->canvas_height;
  char *name = ALLOC_ARRAY(&fa, char, NC_NAME_MAX);
  f32 *px = ALLOC_ARRAY(&fa, f32, 2);
  // farthest first, so nearer tags draw over them
  for (u32 r = n; r-- > 0;) {
    u32 i = o.vals[r];
    f32 dist = (f32)o.keys[r] / 100.0f;
    v4 clip = m4_mulv4(cam->view_proj, (v4){at[i].x, at[i].y, at[i].z, 1.0f});
    if (clip.w <= 0.1f)
      continue;
    f32 alpha = r < always ? 1.0f : m_clampf(1.0f - (dist - NC_TAG_FADE_FROM) / 10.0f, 0.0f, 1.0f);
    if (alpha <= 0.0f)
      continue;
    camera_project_to_screen(cam->view_proj, at[i], sw, sh, px);
    if (px[0] < -100.0f || px[0] > sw + 100.0f || px[1] < -40.0f || px[1] > sh + 40.0f)
      continue;
    nc_name(seed[i], name, NC_NAME_MAX);
    String text = str_format(&fa, "%", fmt_cstr(name));
    u16 size = (u16)m_clampf(260.0f / m_maxf(dist, 1.0f), 10.0f, 18.0f);
    ui_element({.id = ui_idi("NcTag", i),
                .floating = {.attach_to = UI_ATTACH_TO_ROOT,
                             .attach_points = {.element = UI_ATTACH_POINT_CENTER_BOTTOM, .parent = UI_ATTACH_POINT_LEFT_TOP},
                             .offset = {px[0], px[1]}},
                .layout = {.padding = {.left = 5, .right = 5, .top = 2, .bottom = 2}},
                .background_color = {10, 12, 16, (u8)(150.0f * alpha)},
                .corner_radius = ui_corner_radius(4)}) {
      ui_text(((HzUIString){.chars = text.value, .length = (i32)text.len}),
              ui_text_config({.fontSize = size, .textColor = {245, 246, 250, (u8)(255.0f * alpha)},
                              .wrapMode = UI_TEXT_WRAP_NONE}));
    }
  }
}
