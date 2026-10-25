#include "scatter.h"
#include "assets/assets.h"
#include "ecs/ecs_physics/ecs_physics.h"
#include "ecs/transform/ecs_transform.h"
#include "lib/assert.h"

#define SCATTER_CID(w, T) ecs_component_ensure((w), sizeof(T), _Alignof(T), #T)
#define SCATTER_GRID_CELL 8.0f
#define SCATTER_PARTS_MAX 32u // RendererLodGroupLod's per-submesh shadow bits

typedef struct ScatterSpeciesRt {
  u32 variant_count, tint_count, lod_count;
  f32 lod_coverage[SCATTER_LOD_MAX];
  f32 cull_coverage, fade_band;
  b32 fixed_distance;
  ScatterQualityTable density_scale;
  ScatterCollider collider;
} ScatterSpeciesRt;

// a runtime model built over one part list, reused while a later variant lists the same submeshes
typedef struct ScatterModel {
  HzHandleT(RendererModel) model;
  FixedArray(HzHandleT(RendererSubMesh), SCATTER_PARTS_MAX) submeshes;
} ScatterModel;

typedef struct ScatterEndless {
  ArenaAllocator *arena;
  Allocator alloc;
  ArenaAllocator *scratch; // one window's candidates and grid, reset every rebuild
  Allocator scratch_alloc;
  f32 cell_size;
  i32 radius_cells;
  u32 seed;
  PgScatterSpecies *pg;
  u32 *pool_size;
  PgScatterHeightFn height;
  PgScatterNormalFn normal;
  void *surface_ctx;
  ScatterWindowFn window;
  void *window_ctx;
  ScatterPlacement *placements; // grouped by species, at most pool_size each
  u32 *species_first;           // [species_count + 1]
  i32 cell_x, cell_z;
  b32 placed;
  u32 generation;
  u32 rebuilds;
  f32 last_us, max_us;
} ScatterEndless;

struct ScatterField {
  b32 active;
  ScatterMode mode;
  b32 follow_camera;
  i32 quality_override;
  u32 species_count;
  ScatterSpeciesRt *species;
  i32 applied_quality;
  b32 has_focus;
  v3 focus;
  b32 force, clearing;
  u32 placements;
  HzHandleT(RendererLodGroups) *sets; // static: one per spawned species, variant and tint
  u32 set_count, set_cap;
  ScatterModel *models;
  u32 model_count, model_cap;
  DynArray(HzHandleT(RendererMaterial)) tints; // static: per-placement material instances (tint_param)
  ScatterEndless *endless;
};

struct ScatterSink {
  ScatterEndless *e;
  PgScatterGrid *grid;
  DynArray(ScatterPlacement) placed;
};

/* ------------------------------------------------------------------ helpers */

u32 scatter_pick(u32 count, f32 roll) {
  if (count <= 1)
    return 0;
  return MIN(count - 1, (u32)m_floorf(roll * (f32)count));
}

f32 scatter_coverage_at(f32 radius, f32 distance, f32 fov_y_degrees) {
  f32 tan_half = m_tanf(fov_y_degrees * (M_PI / 360.0f));
  return radius / (m_maxf(distance, 1e-4f) * m_maxf(tan_half, 1e-4f));
}

hz_internal const ScatterVariant *scatter_variant(const ScatterSpeciesDesc *sp, u32 variant, u32 tint, u32 lod);

hz_internal b32 scatter_variant_bounds(const RendererState *r, const ScatterVariant *v, aabb *out) {
  aabb b = aabb_invalidate();
  for (u32 p = 0; p < v->part_count; p++) {
    const RendererSubMesh *sub = renderer_submesh(r, v->parts[p].submesh);
    if (!sub)
      return false;
    b = aabb_merge(b, sub->geometry.bounds);
  }
  *out = b;
  return v->part_count > 0;
}

f32 scatter_species_radius(const RendererState *r, const ScatterSpeciesDesc *sd, u32 species, const ScatterPlacement *p,
                           u32 count) {
  f64 sum = 0.0;
  u32 n = 0;
  for (u32 i = 0; i < count; i++) {
    aabb b;
    if (p[i].species != species || !scatter_variant_bounds(r, scatter_variant(sd, p[i].variant, p[i].tint, 0), &b))
      continue;
    // the renderer's LOD sphere: the world AABB of the posed base model; a placement covers ground by radius squared
    f64 radius = aabb_radius(aabb_transform_trs(b, p[i].position, quat_to_m3(p[i].rotation), p[i].scale));
    sum += radius * radius;
    n++;
  }
  return n ? (f32)m_sqrt(sum / n) : 0.0f;
}

RendererQuality scatter_quality_level(const RendererState *r, i32 quality_override) {
  if (quality_override >= 0)
    return (RendererQuality)MIN(quality_override, (i32)RENDERER_QUALITY_COUNT - 1);
  RendererQuality q = renderer_quality(r);
  return q >= RENDERER_QUALITY_COUNT ? RENDERER_QUALITY_HIGH : q;
}

ScatterPlacement scatter_placement_from_point(const PgScatterPoint *p, const ScatterSpeciesDesc *species) {
  u32 variants = species && species->variant_count ? species->variant_count : 1;
  u32 tints = species && species->tint_count ? species->tint_count : 1;
  return (ScatterPlacement){.species = p->species,
                            .position = p->position,
                            .rotation = p->rotation,
                            .scale = p->scale,
                            .variant = scatter_pick(variants, p->roll.x),
                            .tint = scatter_pick(tints, p->roll.y),
                            .roll = p->roll};
}

hz_internal ScatterState *scatter_state(EcsWorld *w) {
  ScatterState *s = ecs_resource_get_mut(w, ScatterState);
  assert_msg(s, "scatter: call scatter_init first");
  return s;
}

hz_internal ScatterField *scatter_field(const ScatterState *s, u32 field) {
  assert_msg(field < s->field_count, "scatter: field % was never created", fmt_u32(field));
  return &s->fields[field];
}

hz_internal void scatter_copy_species(ScatterField *f, const ScatterSpeciesDesc *species) {
  for (u32 i = 0; i < f->species_count; i++) {
    const ScatterSpeciesDesc *d = &species[i];
    u32 lods = d->lod_count ? d->lod_count : 1;
    assert_msg(lods <= SCATTER_LOD_MAX, "scatter: species % has % LODs, the limit is %", fmt_u32(i), fmt_u32(lods),
               fmt_u32(SCATTER_LOD_MAX));
    ScatterSpeciesRt *rt = &f->species[i];
    *rt = (ScatterSpeciesRt){.variant_count = d->variant_count ? d->variant_count : 1,
                             .tint_count = d->tint_count ? d->tint_count : 1,
                             .lod_count = lods,
                             .cull_coverage = d->cull_coverage,
                             .fade_band = d->fade_band,
                             .fixed_distance = d->fixed_distance,
                             .density_scale = d->density_scale,
                             .collider = d->collider};
    for (u32 l = 0; l < SCATTER_LOD_MAX; l++)
      rt->lod_coverage[l] = d->lod_coverage[l];
  }
}

hz_internal const ScatterVariant *scatter_variant(const ScatterSpeciesDesc *sp, u32 variant, u32 tint, u32 lod) {
  u32 tints = sp->tint_count ? sp->tint_count : 1, lods = sp->lod_count ? sp->lod_count : 1;
  return &sp->variants[(variant * tints + tint) * lods + lod];
}

// the field's runtime model over v's submeshes; materials are per object, so only the submesh list keys it
hz_internal HzHandleT(RendererModel) scatter_model(RendererState *r, Allocator *alloc, ScatterField *f,
                                                   const ScatterVariant *v) {
  assert_msg(v->part_count > 0 && v->part_count <= SCATTER_PARTS_MAX, "scatter: a LOD has % parts, 1..% allowed",
             fmt_u32(v->part_count), fmt_u32(SCATTER_PARTS_MAX));
  for (u32 m = 0; m < f->model_count; m++) {
    const ScatterModel *sm = &f->models[m];
    b32 same = sm->submeshes.len == v->part_count;
    for (u32 p = 0; p < v->part_count && same; p++)
      same = handle_equals(HANDLE_CAST(HzHandle, sm->submeshes.items[p]), HANDLE_CAST(HzHandle, v->parts[p].submesh));
    if (same)
      return sm->model;
  }
  if (f->model_count == f->model_cap) {
    u32 cap = f->model_cap ? f->model_cap * 2 : 16;
    ScatterModel *grown = ALLOC_ARRAY(alloc, ScatterModel, cap);
    if (f->model_count)
      mem_cpy(grown, f->models, sizeof(ScatterModel) * f->model_count);
    f->models = grown;
    f->model_cap = cap;
  }
  ScatterModel *sm = &f->models[f->model_count++];
  *sm = (ScatterModel){0};
  tctx_temp_allocator_scope(ta, NULL) {
    RendererModelSubMeshDesc *subs = ALLOC_ARRAY(&ta.allocator, RendererModelSubMeshDesc, v->part_count);
    for (u32 p = 0; p < v->part_count; p++) {
      subs[p].submesh = v->parts[p].submesh;
      sm->submeshes.items[sm->submeshes.len++] = v->parts[p].submesh;
    }
    sm->model = renderer_create_model_borrowed(r, subs, v->part_count);
  }
  return sm->model;
}

hz_internal void scatter_set_push(Allocator *alloc, ScatterField *f, HzHandleT(RendererLodGroups) h) {
  if (f->set_count == f->set_cap) {
    u32 cap = f->set_cap ? f->set_cap * 2 : 64;
    HzHandleT(RendererLodGroups) *grown = ALLOC_ARRAY(alloc, HzHandleT(RendererLodGroups), cap);
    if (f->set_count)
      mem_cpy(grown, f->sets, sizeof(*grown) * f->set_count);
    f->sets = grown;
    f->set_cap = cap;
  }
  f->sets[f->set_count++] = h;
}

/* ------------------------------------------------------------------- colliders */

hz_internal ScatterCollider scatter_collider_of(const ScatterPlacement *p, const ScatterSpeciesRt *sp) {
  if (p->collider.kind != SCATTER_COLLIDER_NONE)
    return p->collider;
  ScatterCollider c = sp->collider;
  if (c.kind == SCATTER_COLLIDER_NONE)
    return c;
  c.offset = v3_mul(c.offset, p->scale);
  c.radius *= MAX(p->scale.x, p->scale.z);
  c.height *= p->scale.y;
  c.half = v3_mul(c.half, p->scale);
  return c;
}

hz_internal ColliderBody scatter_collider_body(const ScatterPlacement *p, const ScatterCollider *c) {
  ColliderBody b = {.pos = v3_add(p->position, c->offset), .rot = quat_identity(), .scale = 1.0f};
  switch (c->kind) {
  case SCATTER_COLLIDER_CAPSULE:
    b.collider = collider_capsule(V3_ZERO, (v3){0.0f, MAX(c->height - c->radius, 0.0f), 0.0f}, c->radius);
    break;
  case SCATTER_COLLIDER_SPHERE:
    b.collider = collider_sphere(V3_ZERO, c->radius);
    break;
  case SCATTER_COLLIDER_BOX:
    b.rot = p->rotation;
    b.collider = collider_box(V3_ZERO, c->half);
    break;
  default:
    break;
  }
  return b;
}

/* --------------------------------------------------------------- static sets */

// one LOD group set: the placements `idx` share species, variant and tint; placement idx[i] is the set's index i
hz_internal HzHandleT(RendererLodGroups) scatter_set_create(RendererState *r, Allocator *alloc, ScatterField *f,
                                                            const ScatterSpeciesDesc *sd, const ScatterPlacement *p,
                                                            const u32 *idx, u32 count) {
  const ScatterPlacement *first = &p[idx[0]];
  const ScatterSpeciesRt *rt = &f->species[first->species];
  HzHandleT(RendererLodGroups) set = {0};
  tctx_temp_allocator_scope(ta, NULL) {
    RendererLodGroupLod *lods = ALLOC_ARRAY(&ta.allocator, RendererLodGroupLod, rt->lod_count);
    u32 stride = 0;
    b32 casts = false;
    for (u32 l = 0; l < rt->lod_count; l++) {
      const ScatterVariant *v = scatter_variant(sd, first->variant, first->tint, l);
      lods[l] = (RendererLodGroupLod){.model = scatter_model(r, alloc, f, v), .switch_coverage = rt->lod_coverage[l]};
      for (u32 k = 0; k < v->part_count; k++) {
        if (!v->parts[k].casts_shadow)
          lods[l].shadowless |= 1u << k;
        else if (!v->parts[k].static_shadow)
          lods[l].dynamic_shadow |= 1u << k;
        casts = casts || v->parts[k].casts_shadow;
      }
      stride += v->part_count;
    }
    Transform *xfs = ALLOC_ARRAY(&ta.allocator, Transform, count);
    HzHandleT(RendererMaterial) *mats = ALLOC_ARRAY(&ta.allocator, HzHandleT(RendererMaterial), (size_t)count * stride);
    HzHandleT(RendererMaterial) *bases = ALLOC_ARRAY(&ta.allocator, HzHandleT(RendererMaterial), stride);
    for (u32 i = 0; i < count; i++) {
      const ScatterPlacement *pl = &p[idx[i]];
      xfs[i] = (Transform){pl->position, pl->rotation, pl->scale};
      HzHandleT(RendererMaterial) *row = mats + (size_t)i * stride;
      u32 n = 0;
      for (u32 l = 0; l < rt->lod_count; l++) {
        const ScatterVariant *v = scatter_variant(sd, pl->variant, pl->tint, l);
        for (u32 k = 0; k < v->part_count; k++, n++) {
          bases[n] = row[n] = v->parts[k].material;
          if (!sd->tint_param)
            continue;
          // one instance per base material of the placement, shared by its LODs
          u32 j = 0;
          while (j < n && !handle_equals(HANDLE_CAST(HzHandle, bases[j]), HANDLE_CAST(HzHandle, bases[n])))
            j++;
          if (j < n) {
            row[n] = row[j];
            continue;
          }
          row[n] = renderer_create_material_instance(r, bases[n]);
          material_set_vec4(r, row[n], sd->tint_param,
                            (vec4){pl->instance_tint.x, pl->instance_tint.y, pl->instance_tint.z, pl->instance_tint.w});
          dyn_arr_append(f->tints, row[n]);
        }
      }
    }
    HzHandleT(RendererLodGroups) h = renderer_lod_groups_create(r, &(RendererLodGroupsDesc){
                                                                       .lods = lods,
                                                                       .lod_count = rt->lod_count,
                                                                       .cull_coverage = rt->cull_coverage,
                                                                       .fade_band = rt->fade_band,
                                                                       .fixed_distance = rt->fixed_distance,
                                                                       .transforms = xfs,
                                                                       .count = count,
                                                                       .materials = mats,
                                                                       .casts_shadow = casts,
                                                                       .static_shadow = true,
                                                                   });
    if (handle_is_valid(HANDLE_CAST(HzHandle, h)))
      scatter_set_push(alloc, f, h);
    else
      log_error("scatter: species % variant % tint %: % placements not drawn (the renderer refused the set)",
                fmt_u32(first->species), fmt_u32(first->variant), fmt_u32(first->tint), fmt_u32(count));
    set = h;
  }
  return set;
}

void scatter_static_spawn(EcsWorld *w, RendererState *r, u32 field, const ScatterSpeciesDesc *species, const ScatterPlacement *p,
                          u32 count) {
  // renderer LOD sets are created here, which is main-lane state
  assert(is_main_thread());
  ScatterState *s = scatter_state(w);
  ScatterField *f = scatter_field(s, field);
  assert_msg(f->mode == SCATTER_STATIC, "scatter: field % is endless", fmt_u32(field));
  if (count == 0)
    return;
  // the spawn's tables are the whole species: thresholds can be measured from the parts' bounds
  scatter_copy_species(f, species);
  Allocator *ca = ecs_cmd_allocator(w);

  // placements bucketed by (species, variant, tint), stable within a bucket
  u32 *key_first = ALLOC_ARRAY(ca, u32, f->species_count + 1);
  for (u32 i = 0; i < f->species_count; i++)
    key_first[i + 1] = key_first[i] + f->species[i].variant_count * f->species[i].tint_count;
  u32 keys = key_first[f->species_count];
  u32 *bucket_first = ALLOC_ARRAY(ca, u32, keys + 1);
  u32 *key_of = ALLOC_ARRAY(ca, u32, count);
  for (u32 i = 0; i < count; i++) {
    const ScatterPlacement *pl = &p[i];
    assert_msg(pl->species < f->species_count, "scatter: placement % has species %", fmt_u32(i), fmt_u32(pl->species));
    const ScatterSpeciesRt *rt = &f->species[pl->species];
    assert_msg(pl->variant < rt->variant_count && pl->tint < rt->tint_count,
               "scatter: placement % has variant % tint %", fmt_u32(i), fmt_u32(pl->variant), fmt_u32(pl->tint));
    key_of[i] = key_first[pl->species] + pl->variant * rt->tint_count + pl->tint;
    bucket_first[key_of[i] + 1]++;
  }
  for (u32 k = 0; k < keys; k++)
    bucket_first[k + 1] += bucket_first[k];
  u32 *fill = ALLOC_ARRAY(ca, u32, keys);
  u32 *order = ALLOC_ARRAY(ca, u32, count);
  for (u32 i = 0; i < count; i++)
    order[bucket_first[key_of[i]] + fill[key_of[i]]++] = i;
  ScatterInstance *insts = ALLOC_ARRAY(ca, ScatterInstance, count);
  for (u32 k = 0; k < keys; k++) {
    u32 n = bucket_first[k + 1] - bucket_first[k];
    if (!n)
      continue;
    const u32 *idx = order + bucket_first[k];
    HzHandleT(RendererLodGroups) h = scatter_set_create(r, &s->alloc, f, &species[p[idx[0]].species], p, idx, n);
    for (u32 i = 0; i < n; i++) {
      const ScatterPlacement *pl = &p[idx[i]];
      insts[idx[i]] = (ScatterInstance){.field = field, .species = pl->species, .variant = pl->variant, .tint = pl->tint,
                                        .set = h, .index = i, .position = pl->position, .rotation = pl->rotation,
                                        .scale = pl->scale};
    }
  }

  // one entity per placement: those with a collider carry its static body, the rest only the instance
  ScatterInstance *solid = ALLOC_ARRAY(ca, ScatterInstance, count);
  ScatterInstance *plain = ALLOC_ARRAY(ca, ScatterInstance, count);
  ColliderBody *bodies = ALLOC_ARRAY(ca, ColliderBody, count);
  u32 solid_count = 0, plain_count = 0;
  for (u32 i = 0; i < count; i++) {
    ScatterCollider c = scatter_collider_of(&p[i], &f->species[p[i].species]);
    if (c.kind == SCATTER_COLLIDER_NONE) {
      plain[plain_count++] = insts[i];
      continue;
    }
    bodies[solid_count] = scatter_collider_body(&p[i], &c);
    solid[solid_count++] = insts[i];
  }
  if (solid_count > 0) {
    StaticBody *marker = ALLOC(ca, StaticBody);
    EcsSpawnColumn *cols = ALLOC_ARRAY(ca, EcsSpawnColumn, 3);
    cols[0] = (EcsSpawnColumn){.component = SCATTER_CID(w, ColliderBody), .data = bodies};
    cols[1] = (EcsSpawnColumn){.component = SCATTER_CID(w, StaticBody), .data = marker, .constant = true};
    cols[2] = (EcsSpawnColumn){.component = SCATTER_CID(w, ScatterInstance), .data = solid};
    ecs_spawn_block(w, &(EcsSpawnBlockDesc){.count = solid_count, .columns = cols, .column_count = 3});
  }
  if (plain_count > 0) {
    EcsSpawnColumn *cols = ALLOC(ca, EcsSpawnColumn);
    cols[0] = (EcsSpawnColumn){.component = SCATTER_CID(w, ScatterInstance), .data = plain};
    ecs_spawn_block(w, &(EcsSpawnBlockDesc){.count = plain_count, .columns = cols, .column_count = 1});
  }
  f->placements += count;
}

/* ----------------------------------------------------------------------- sink */

void scatter_sink_place(ScatterSink *s, const ScatterPlacement *p) { dyn_arr_append(s->placed, *p); }

void scatter_sink_reserve_disc(ScatterSink *s, f32 x, f32 z, f32 radius) { pg_scatter_grid_reserve_disc(s->grid, x, z, radius); }

void scatter_sink_reserve_box(ScatterSink *s, f32 x, f32 z, f32 half_x, f32 half_z, f32 yaw) {
  pg_scatter_grid_reserve_box(s->grid, x, z, half_x, half_z, yaw);
}

/* ------------------------------------------------------------- endless field */

hz_internal f32 scatter_density_scale(const ScatterSpeciesRt *sp, RendererQuality q) {
  const f32 *v = sp->density_scale.value;
  b32 any = false;
  for (u32 i = 0; i < RENDERER_QUALITY_COUNT; i++)
    any = any || v[i] != 0.0f;
  return any ? v[q] : 1.0f;
}

hz_internal void scatter_endless_rebuild(ScatterField *f, i32 cx, i32 cz) {
  ScatterEndless *e = f->endless;
  u64 t0 = os_time_now();
  ALLOC_RESET(&e->scratch_alloc);
  f32 reach = (f32)e->radius_cells * e->cell_size;
  v2 center = {((f32)cx + 0.5f) * e->cell_size, ((f32)cz + 0.5f) * e->cell_size};
  v2 half = {reach + e->cell_size * 0.5f, reach + e->cell_size * 0.5f};
  ScatterWindow win = {.min = v2_sub(center, half), .max = v2_add(center, half), .cell_x = cx, .cell_z = cz};

  ScatterSink sink = {.e = e, .grid = pg_scatter_grid_create(&e->scratch_alloc, win.min, win.max, SCATTER_GRID_CELL)};
  sink.placed = dyn_arr_new_alloc(&e->scratch_alloc, ScatterPlacement, 256);
  if (e->window)
    e->window(e->window_ctx, &win, &sink);

  RendererQuality q = (RendererQuality)f->applied_quality;
  PgScatterSpecies *pg = ALLOC_ARRAY(&e->scratch_alloc, PgScatterSpecies, f->species_count);
  for (u32 i = 0; i < f->species_count; i++) {
    pg[i] = e->pg[i];
    pg[i].density *= scatter_density_scale(&f->species[i], q);
  }
  PgScatterOut out = {0};
  pg_scatter(&(PgScatterDesc){.min = win.min,
                              .max = win.max,
                              .seed = e->seed,
                              .species = pg,
                              .species_count = f->species_count,
                              .height = e->height,
                              .normal = e->normal,
                              .surface_ctx = e->surface_ctx,
                              .grid = sink.grid},
             &e->scratch_alloc, &out);

  // placements grouped by species, each capped at its pool
  u32 *counts = ALLOC_ARRAY(&e->scratch_alloc, u32, f->species_count);
  for (u32 i = 0; i < sink.placed.len; i++)
    counts[sink.placed.items[i].species]++;
  for (u32 i = 0; i < f->species_count; i++)
    counts[i] += out.species_start[i + 1] - out.species_start[i];
  e->species_first[0] = 0;
  for (u32 i = 0; i < f->species_count; i++) {
    if (counts[i] > e->pool_size[i])
      log_warn("scatter: endless species % wants % placements, its pool holds %", fmt_u32(i), fmt_u32(counts[i]), fmt_u32(e->pool_size[i]));
    e->species_first[i + 1] = e->species_first[i] + MIN(counts[i], e->pool_size[i]);
  }
  u32 *filled = ALLOC_ARRAY(&e->scratch_alloc, u32, f->species_count);
  for (u32 i = 0; i < sink.placed.len; i++) {
    const ScatterPlacement *pl = &sink.placed.items[i];
    u32 s = pl->species;
    if (filled[s] < e->pool_size[s] && e->species_first[s] + filled[s] < e->species_first[s + 1])
      e->placements[e->species_first[s] + filled[s]++] = *pl;
  }
  for (u32 i = 0; i < out.count; i++) {
    ScatterPlacement pl = scatter_placement_from_point(&out.points[i], NULL);
    u32 s = pl.species;
    if (filled[s] < e->pool_size[s] && e->species_first[s] + filled[s] < e->species_first[s + 1])
      e->placements[e->species_first[s] + filled[s]++] = pl;
  }
  e->placed = true;
  e->cell_x = cx;
  e->cell_z = cz;
  e->generation++;
  e->rebuilds++;
  f32 us = (f32)os_ticks_to_ms(os_time_diff(os_time_now(), t0)) * 1000.0f;
  e->last_us = us;
  e->max_us = MAX(e->max_us, us);
}

/* ------------------------------------------------------------------ creation */

void scatter_init(EcsWorld *w) {
  // the world's resources are main-lane state until the first scope
  assert(is_main_thread());
  ScatterState st = {0};
  st.alloc = make_arena_allocator(arena_create(MB(2), KB(64)));
  st.fields = ALLOC_ARRAY(&st.alloc, ScatterField, SCATTER_FIELD_MAX);
  ecs_resource_set(w, ScatterState, st);
}

// one hidden model instance per slot of species i, its first variant's LODs with the species' switches and cull
hz_internal HzHandleT(RendererModelInstance) scatter_pool_instance(RendererState *r, Allocator *alloc, ScatterField *f,
                                                                   const ScatterSpeciesDesc *sd, u32 i) {
  const ScatterSpeciesRt *rt = &f->species[i];
  const ScatterVariant *base = scatter_variant(sd, 0, 0, 0);
  b32 casts = base->parts[0].casts_shadow, statics = base->parts[0].static_shadow;
  for (u32 l = 0; l < rt->lod_count; l++) {
    const ScatterVariant *v = scatter_variant(sd, 0, 0, l);
    for (u32 k = 0; k < v->part_count; k++)
      assert_msg(v->parts[k].casts_shadow == casts && v->parts[k].static_shadow == statics,
                 "scatter: endless species % mixes shadow flags across its parts (a model instance has one)", fmt_u32(i));
  }
  HzHandleT(RendererModelInstance) h = renderer_model_instantiate(
      r, &(RendererModelInstanceDesc){.model = scatter_model(r, alloc, f, base),
                                      .transform = {.rotation = quat_identity(), .scale = V3_ONE},
                                      .casts_shadow = casts,
                                      .static_shadow = statics,
                                      .cull_coverage = rt->cull_coverage,
                                      .fixed_distance = rt->fixed_distance});
  for (u32 l = 1; l < rt->lod_count; l++)
    renderer_model_instance_add_lod_model(r, h, scatter_model(r, alloc, f, scatter_variant(sd, 0, 0, l)),
                                          rt->lod_coverage[l]);
  // the part's own material per LOD: a shared submesh's material belongs to its other users
  const RendererModelInstance *mi = renderer_model_instance(r, h);
  for (u32 l = 0; l < rt->lod_count; l++) {
    const ScatterVariant *v = scatter_variant(sd, 0, 0, l);
    const RendererMeshInstance *mesh = &mi->lods.items[l].mesh_instances.items[0];
    for (u32 k = 0; k < v->part_count && k < mesh->children.len; k++)
      renderer_submesh_instance_set_material(r, mesh->children.items[k], v->parts[k].material);
  }
  renderer_model_instance_set_visible(r, h, false);
  return h;
}

// the pool of an endless field: one model-instance entity per slot of each species
hz_internal void scatter_endless_spawn_pool(EcsWorld *w, RendererState *r, Allocator *alloc, u32 field, ScatterField *f,
                                            const ScatterEndless *e, const ScatterSpeciesDesc *species) {
  tctx_temp_allocator_scope(ta, NULL) {
    Allocator *a = &ta.allocator;
    u32 cap = 0;
    for (u32 i = 0; i < f->species_count; i++)
      cap += e->pool_size[i];
    WorldTransform *xfs = ALLOC_ARRAY(a, WorldTransform, cap);
    EcsModelInstance *insts = ALLOC_ARRAY(a, EcsModelInstance, cap);
    EcsVisibility *vis = ALLOC_ARRAY(a, EcsVisibility, cap);
    ScatterPoolSlot *slots = ALLOC_ARRAY(a, ScatterPoolSlot, cap);
    u32 n = 0;
    for (u32 i = 0; i < f->species_count; i++) {
      for (u32 k = 0; k < e->pool_size[i]; k++, n++) {
        insts[n].instance = scatter_pool_instance(r, alloc, f, &species[i], i);
        xfs[n] = (WorldTransform){.rotation = quat_identity(), .scale = V3_ONE};
        vis[n] = (EcsVisibility){.visible = false, .casts_shadow = scatter_variant(&species[i], 0, 0, 0)->parts[0].casts_shadow};
        slots[n] = (ScatterPoolSlot){.field = field, .species = i, .slot = k};
      }
    }
    if (n > 0) {
      EcsSpawnColumn *cols = ALLOC_ARRAY(a, EcsSpawnColumn, 4);
      cols[0] = (EcsSpawnColumn){.component = SCATTER_CID(w, WorldTransform), .data = xfs};
      cols[1] = (EcsSpawnColumn){.component = SCATTER_CID(w, EcsModelInstance), .data = insts};
      cols[2] = (EcsSpawnColumn){.component = SCATTER_CID(w, EcsVisibility), .data = vis};
      cols[3] = (EcsSpawnColumn){.component = SCATTER_CID(w, ScatterPoolSlot), .data = slots};
      ecs_spawn_block(w, &(EcsSpawnBlockDesc){.count = n, .columns = cols, .column_count = 4});
    }
  }
}

u32 scatter_field_create(EcsWorld *w, RendererState *r, const ScatterFieldDesc *d) {
  // renderer instances and the world outside a scope are main-lane state
  assert(is_main_thread());
  ScatterState *s = scatter_state(w);
  assert_msg(s->field_count < SCATTER_FIELD_MAX, "scatter: more than % fields", fmt_u32(SCATTER_FIELD_MAX));
  assert_msg(d->species && d->species_count > 0 && d->species_count <= SCATTER_SPECIES_MAX, "scatter: % species",
             fmt_u32(d->species_count));
  u32 id = s->field_count++;
  ScatterField *f = &s->fields[id];
  *f = (ScatterField){.active = true,
                      .mode = d->mode,
                      .follow_camera = d->follow_camera,
                      .quality_override = d->quality_override,
                      .applied_quality = (i32)scatter_quality_level(r, d->quality_override),
                      .force = true};
  f->species = ALLOC_ARRAY(&s->alloc, ScatterSpeciesRt, d->species_count);
  f->species_count = d->species_count;
  scatter_copy_species(f, d->species);
  if (d->mode == SCATTER_STATIC) {
    f->tints = dyn_arr_new_alloc(&s->alloc, HzHandleT(RendererMaterial), 64);
    return id;
  }

  const ScatterEndlessDesc *ed = &d->endless;
  assert_msg(ed->cell_size > 0.0f && ed->radius_cells > 0 && ed->pg && ed->pool_size,
             "scatter: an endless field needs a cell size, a radius, pg species and pool sizes");
  ScatterEndless *e = ALLOC(&s->alloc, ScatterEndless);
  f->endless = e;
  e->arena = arena_create(MB(2), KB(64));
  e->alloc = make_arena_allocator(e->arena);
  e->scratch = arena_create(MB(8), KB(256));
  e->scratch_alloc = make_arena_allocator(e->scratch);
  e->cell_size = ed->cell_size;
  e->radius_cells = ed->radius_cells;
  e->seed = ed->seed;
  e->height = ed->height;
  e->normal = ed->normal;
  e->surface_ctx = ed->surface_ctx;
  e->window = ed->window;
  e->window_ctx = ed->window_ctx;
  e->pg = ALLOC_ARRAY(&e->alloc, PgScatterSpecies, d->species_count);
  e->pool_size = ALLOC_ARRAY(&e->alloc, u32, d->species_count);
  e->species_first = ALLOC_ARRAY(&e->alloc, u32, d->species_count + 1);
  u32 total = 0;
  for (u32 i = 0; i < d->species_count; i++) {
    e->pg[i] = ed->pg[i];
    e->pool_size[i] = ed->pool_size[i];
    total += ed->pool_size[i];
  }
  e->placements = ALLOC_ARRAY(&e->alloc, ScatterPlacement, total);
  scatter_endless_spawn_pool(w, r, &s->alloc, id, f, e, d->species);
  return id;
}

/* ------------------------------------------------------------------ accessors */

void scatter_field_clear(ScatterState *s, u32 field) { scatter_field(s, field)->clearing = true; }

void scatter_field_set_quality(ScatterState *s, u32 field, i32 quality_override) {
  ScatterField *f = scatter_field(s, field);
  f->quality_override = quality_override;
  f->applied_quality = -1;
}

void scatter_field_set_focus(ScatterState *s, u32 field, v3 focus) {
  ScatterField *f = scatter_field(s, field);
  f->focus = focus;
  f->has_focus = true;
}

u32 scatter_field_placement_count(const ScatterState *s, u32 field) { return scatter_field(s, field)->placements; }

const ScatterPlacement *scatter_field_placements(const ScatterState *s, u32 field, u32 *count) {
  const ScatterField *f = scatter_field(s, field);
  assert_msg(f->endless, "scatter: field % is not endless", fmt_u32(field));
  *count = f->endless->species_first[f->species_count];
  return f->endless->placements;
}

u32 scatter_field_rebuilds(const ScatterState *s, u32 field) {
  const ScatterEndless *e = scatter_field(s, field)->endless;
  return e ? e->rebuilds : 0;
}

void scatter_field_rebuild_cost(const ScatterState *s, u32 field, f32 *last_us, f32 *max_us) {
  const ScatterEndless *e = scatter_field(s, field)->endless;
  *last_us = e ? e->last_us : 0.0f;
  *max_us = e ? e->max_us : 0.0f;
}

/* -------------------------------------------------------------------- systems */

// the camera every following field measures from
ecs_system(scatter_camera_system, EcsIn(Camera), EcsResInOut(ScatterState)) {
  if (it->count == 0)
    return;
  ScatterState *s = ecs_sys_get_resource_mut(ScatterState);
  s->camera = ecs_sys_get_component(Camera, 0)->pos;
  s->has_camera = true;
}

// per endless field: the quality level and the window around the focus
ecs_system(scatter_tick_system, EcsResInOut(ScatterState), EcsResIn(RendererState)) {
  ScatterState *s = ecs_sys_get_resource_mut(ScatterState);
  const RendererState *r = ecs_sys_get_resource(RendererState);
  for (u32 i = 0; i < s->field_count; i++) {
    ScatterField *f = &s->fields[i];
    if (f->mode != SCATTER_ENDLESS)
      continue;
    RendererQuality q = scatter_quality_level(r, f->quality_override);
    if ((i32)q != f->applied_quality) {
      f->applied_quality = (i32)q;
      f->force = true;
    }
    if (f->follow_camera && s->has_camera) {
      f->focus = s->camera;
      f->has_focus = true;
    }
    if (!f->has_focus)
      continue;
    ScatterEndless *e = f->endless;
    i32 cx = (i32)m_floorf(f->focus.x / e->cell_size), cz = (i32)m_floorf(f->focus.z / e->cell_size);
    if (!e->placed || cx != e->cell_x || cz != e->cell_z || f->force) {
      scatter_endless_rebuild(f, cx, cz);
      f->force = false;
    }
  }
}

// pooled slots of an endless field take the new window's placements, the unused ones hide
ecs_system(scatter_pool_system, EcsInOut(ScatterPoolSlot), EcsInOut(WorldTransform), EcsInOut(EcsVisibility), EcsResIn(ScatterState)) {
  const ScatterState *s = ecs_sys_get_resource(ScatterState);
  for (i32 i = 0; i < it->count; i++) {
    const ScatterPoolSlot *slot = ecs_sys_get_component(ScatterPoolSlot, i);
    const ScatterEndless *e = s->fields[slot->field].endless;
    if (slot->applied == e->generation)
      continue;
    u32 first = e->species_first[slot->species], count = e->species_first[slot->species + 1] - first;
    b32 used = slot->slot < count;
    if (used) {
      const ScatterPlacement *p = &e->placements[first + slot->slot];
      *ecs_sys_get_component_mut(WorldTransform, i) =
          (WorldTransform){.position = p->position, .rotation = p->rotation, .scale = p->scale};
    }
    if (ecs_sys_get_component(EcsVisibility, i)->visible != used)
      ecs_sys_get_component_mut(EcsVisibility, i)->visible = used;
    ecs_sys_get_component_mut(ScatterPoolSlot, i)->applied = e->generation;
  }
}

ecs_system(scatter_instance_clear_system, EcsIn(ScatterInstance), EcsResIn(ScatterState)) {
  const ScatterState *s = ecs_sys_get_resource(ScatterState);
  // the placement entities are many and a clear is rare: a chunk is read only on a clearing frame
  b32 clearing = false;
  for (u32 f = 0; f < s->field_count; f++)
    clearing = clearing || s->fields[f].clearing;
  if (!clearing)
    return;
  for (i32 i = 0; i < it->count; i++)
    if (s->fields[ecs_sys_get_component(ScatterInstance, i)->field].clearing)
      ecs_entity_destroy(it->world, it->entities[i]);
}

// main lane, no system running: renderer sets are destroyed outside renderer_update; the game may free the parts next
hz_internal void scatter_sets_destroy(EcsWorld *w, void *payload) {
  u32 field = *(const u32 *)payload;
  ScatterField *f = scatter_field(scatter_state(w), field);
  RendererState *r = ecs_resource_get_mut(w, RendererState);
  for (u32 i = 0; i < f->set_count; i++)
    renderer_lod_groups_destroy(r, f->sets[i]);
  f->set_count = 0;
  f->placements = 0;
  if (f->mode != SCATTER_STATIC)
    return; // the pool's instances keep their models
  for (u32 m = 0; m < f->model_count; m++)
    renderer_destroy_model(r, f->models[m].model);
  f->model_count = 0;
  for (u32 t = 0; t < f->tints.len; t++)
    renderer_destroy_material(r, f->tints.items[t]);
  f->tints.len = 0;
}

// the clear request lasts one frame, after the instance clear ran; the sets go at this frame's flush
ecs_system(scatter_clear_done_system, EcsResInOut(ScatterState)) {
  ScatterState *s = ecs_sys_get_resource_mut(ScatterState);
  for (u32 i = 0; i < s->field_count; i++) {
    if (!s->fields[i].clearing)
      continue;
    s->fields[i].clearing = false;
    ecs_defer_call(it->world, scatter_sets_destroy, &i, sizeof(i));
  }
}

void scatter_schedule(EcsWorld *w) {
  ecs_schedule_single(w, scatter_camera_system); // writes the resource (one camera)
  ecs_schedule_single(w, scatter_tick_system);   // one window rebuild per endless field
  ecs_schedule(w, scatter_pool_system);
  ecs_schedule(w, scatter_instance_clear_system);
  ecs_schedule_single(w, scatter_clear_done_system);
}
