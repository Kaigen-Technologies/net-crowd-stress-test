// Scatter: STATIC fields place renderer LOD groups once; ENDLESS fields recycle pooled model instances around a focus.
// Use: scatter_init, scatter_field_create, scatter_static_spawn (static), then scatter_schedule once in the frame's scope.
#pragma once

#include "ecs/camera/ecs_camera.h"
#include "ecs/ecs.h"
#include "ecs/ecs_physics/ecs_physics.h"
#include "ecs/renderer/ecs_renderer.h"
#include "lib/hzmath.h"
#include "procgen/pg_scatter.h"
#include "rendering/renderer.h"

#define SCATTER_FIELD_MAX 8
#define SCATTER_SPECIES_MAX 32
#define SCATTER_LOD_MAX 4

typedef enum ScatterMode {
  SCATTER_STATIC = 0,
  SCATTER_ENDLESS,
} ScatterMode;

// one drawn piece of a variant at one LOD; at most 32 per LOD
typedef struct ScatterPart {
  HzHandleT(RendererSubMesh) submesh;
  HzHandleT(RendererMaterial) material;
  b32 casts_shadow;
  b32 static_shadow;
} ScatterPart;

typedef struct ScatterVariant {
  const ScatterPart *parts;
  u32 part_count;
} ScatterVariant;

typedef struct ScatterQualityTable {
  f32 value[RENDERER_QUALITY_COUNT]; // indexed by RendererQuality
} ScatterQualityTable;

typedef enum ScatterColliderKind {
  SCATTER_COLLIDER_NONE = 0,
  SCATTER_COLLIDER_CAPSULE, // upright from position + offset, `height` tall
  SCATTER_COLLIDER_SPHERE,  // centred at position + offset
  SCATTER_COLLIDER_BOX,     // centred at position + offset, rotated with the placement
} ScatterColliderKind;

typedef struct ScatterCollider {
  ScatterColliderKind kind;
  v3 offset;
  f32 radius;
  f32 height;
  v3 half;
} ScatterCollider;

typedef struct ScatterPlacement {
  u32 species;
  v3 position;
  quat rotation;
  v3 scale;
  u32 variant; // [0, variant_count)
  u32 tint;    // [0, tint_count)
  v4 instance_tint; // value of the species' tint_param on this instance's own material
  v4 roll;     // the point's deterministic randoms, kept for the game's own use
  ScatterCollider collider; // NONE falls back to the species' template scaled by `scale`
} ScatterPlacement;
DYN_ARRAY_TYPEDEF(ScatterPlacement)

typedef struct ScatterSpeciesDesc {
  u32 variant_count;                // 0 = 1
  u32 tint_count;                   // 0 = 1; materials differ per tint
  u32 lod_count;                    // 1..SCATTER_LOD_MAX; 0 = 1
  const ScatterVariant *variants;   // [variant][tint][lod]
  CString tint_param;               // static: each placement gets its own material with this vec4 = instance_tint
  f32 lod_coverage[SCATTER_LOD_MAX]; // scatter_coverage_at where LOD l takes over; [0] unused; descending
  f32 cull_coverage; // below this on-screen size nothing draws; 0 = never
  f32 fade_band;     // static: dither cross-fade above each switch and the cull, a fraction of it; > 0 needs lod_fade materials
  b32 fixed_distance; // switches and cull ignore renderer_set_lod_distance_scale: the same distance on every level
  ScatterQualityTable density_scale; // per quality: endless fields multiply the pg density by it, a static field's game reads it; all zero = 1
  ScatterCollider collider;         // template for placements that set none
} ScatterSpeciesDesc;

// the window an endless rebuild fills
typedef struct ScatterWindow {
  v2 min, max;
  i32 cell_x, cell_z;
} ScatterWindow;

typedef struct ScatterSink ScatterSink;
void scatter_sink_place(ScatterSink *s, const ScatterPlacement *p);
void scatter_sink_reserve_disc(ScatterSink *s, f32 x, f32 z, f32 radius);
void scatter_sink_reserve_box(ScatterSink *s, f32 x, f32 z, f32 half_x, f32 half_z, f32 yaw);

// runs on one lane before the window's pg_scatter, so ground it reserves stays clear
typedef void (*ScatterWindowFn)(void *ctx, const ScatterWindow *win, ScatterSink *sink);

typedef struct ScatterEndlessDesc {
  f32 cell_size;      // the window recentres when the focus crosses a cell of this size
  i32 radius_cells;   // the window reaches this many cells each way
  u32 seed;
  const PgScatterSpecies *pg;       // [species_count], base densities (scaled by density_scale per quality)
  const u32 *pool_size;             // [species_count] placements the species can show at once
  PgScatterHeightFn height;         // NULL = flat y 0
  PgScatterNormalFn normal;
  void *surface_ctx;
  ScatterWindowFn window;
  void *window_ctx;
} ScatterEndlessDesc;

typedef struct ScatterFieldDesc {
  ScatterMode mode;
  const ScatterSpeciesDesc *species;
  u32 species_count;
  b32 follow_camera;    // endless: else the focus set by scatter_field_set_focus
  i32 quality_override; // 0.. a RendererQuality; negative follows renderer_quality
  ScatterEndlessDesc endless;
} ScatterFieldDesc;

// a pooled model instance of an endless field: the slot's current placement is read from the field
ECS_COMPONENT_TYPEDEF(ScatterPoolSlot);
typedef struct ScatterPoolSlot {
  u32 field;
  u32 species;
  u32 slot;
  u32 applied; // the field generation this entity last showed
} ScatterPoolSlot;

// a static field's placement: one entity each, carrying its collider when it has one; destroyed with the field
ECS_COMPONENT_TYPEDEF(ScatterInstance);
typedef struct ScatterInstance {
  u32 field;
  u32 species, variant, tint;
  HzHandleT(RendererLodGroups) set; // renderer_lod_groups_lod / _draw(set, index); invalid if the renderer refused it
  u32 index;
  v3 position;
  quat rotation;
  v3 scale;
} ScatterInstance;

// resource: the fields; systems declare it, games drive it through the functions below
typedef struct ScatterField ScatterField;
typedef struct ScatterState {
  ScatterField *fields;
  u32 field_count;
  Allocator alloc;
  b32 has_camera;
  v3 camera;
} ScatterState;
ECS_COMPONENT_TYPEDEF(ScatterState);

// one pg_scatter point as a placement: variant and tint picked from its rolls
ScatterPlacement scatter_placement_from_point(const PgScatterPoint *p, const ScatterSpeciesDesc *species);
// min(count - 1, floor(roll * count))
u32 scatter_pick(u32 count, f32 roll);
// the on-screen size the renderer's LOD pick measures: radius over distance x tan(vertical fov / 2)
f32 scatter_coverage_at(f32 radius, f32 distance, f32 fov_y_degrees);
// the quadratic-mean LOD sphere radius of a species' placements: a threshold from it at d draws as many as a cut at d
f32 scatter_species_radius(const RendererState *r, const ScatterSpeciesDesc *sd, u32 species, const ScatterPlacement *p,
                           u32 count);
// the level a field reads; override negative follows renderer_quality (HIGH before the first renderer_set_quality)
RendererQuality scatter_quality_level(const RendererState *r, i32 quality_override);

// main lane, after ecs_renderer_init
void scatter_init(EcsWorld *w);
// main lane, outside the scope; endless fields create their pool entities here. Returns the field id
u32 scatter_field_create(EcsWorld *w, RendererState *r, const ScatterFieldDesc *d);
// main lane, outside renderer_update: the LOD sets and a ScatterInstance entity per placement; `species` gives the draw data
void scatter_static_spawn(EcsWorld *w, RendererState *r, u32 field, const ScatterSpeciesDesc *species, const ScatterPlacement *p,
                          u32 count);
// destroys the field's sets and placement entities at this frame's flush; call from a system holding EcsResInOut(ScatterState)
void scatter_field_clear(ScatterState *s, u32 field);
// the next tick re-reads the field's quality (an endless field's density)
void scatter_field_set_quality(ScatterState *s, u32 field, i32 quality_override);
// the point an endless field recentres on, when it does not follow the camera
void scatter_field_set_focus(ScatterState *s, u32 field, v3 focus);
// placements a static field holds
u32 scatter_field_placement_count(const ScatterState *s, u32 field);
// the endless field's window placements, valid until the next tick, and the windows built so far
const ScatterPlacement *scatter_field_placements(const ScatterState *s, u32 field, u32 *count);
u32 scatter_field_rebuilds(const ScatterState *s, u32 field);
// microseconds of the last endless rebuild and the largest so far
void scatter_field_rebuild_cost(const ScatterState *s, u32 field, f32 *last_us, f32 *max_us);

// inside the frame's scope, once
void scatter_schedule(EcsWorld *w);
