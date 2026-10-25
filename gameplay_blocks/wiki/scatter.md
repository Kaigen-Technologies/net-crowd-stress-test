---
title: Scatter
description: how do I place thousands of props (trees, rocks, shrubs) as renderer LOD groups with optional colliders, screen-size LOD switches, a dithered cross-fade and a cull, or as an endless field of pooled model instances recycled around the player, with density per quality level?
resource: src/gameplay/scatter/scatter.h
tags: [gameplay, scatter, lod, procgen, quality]
updated: 2026-10-15
---

# What it provides

- Two field modes behind one resource (`ScatterState`, up to `SCATTER_FIELD_MAX` fields, up to 32 species each):
  - STATIC: you author the placements (from [[procgen/scatter]] points or game code). `scatter_static_spawn` makes one renderer LOD group set per (species, variant, tint), and one `ScatterInstance` entity per placement (species, variant, tint, transform, and its `set` + `index` for `renderer_lod_groups_lod`), carrying its static `ColliderBody` when it has a collider. The renderer picks LODs, fades and culls; no scatter system touches the placements per frame.
  - ENDLESS: a `pg_scatter` window of cells around a focus. Each pool slot is one model-instance entity (its LODs and cull from the species), recycled when the focus crosses a cell. A `window` callback runs first, so the game authors landmarks and reserves ground.
- Species data: `ScatterSpeciesDesc` has variants `[variant][tint][lod]` of `ScatterPart`s (up to `SCATTER_LOD_MAX` LODs, 32 parts per LOD), `lod_coverage[lod]`, `cull_coverage`, `fade_band`, `fixed_distance`, `density_scale` per level, a collider template, and `tint_param` for a per-placement vec4 on its own material.
- Thresholds are on-screen sizes, as the renderer measures them: `scatter_coverage_at(radius, distance, fov)` turns metres into one, and `scatter_species_radius(r, species_desc, species, placements, count)` gives the radius to measure with: the RMS of the placements' posed LOD0 bounds, the sphere the renderer uses. Each placement switches on its own size, so larger ones hold each LOD further out.
- Quality: author the thresholds once (at HIGH's distances) and let the renderer's `renderer_set_lod_distance_scale(r, level, lod_scale, view_scale)` scale every switch and cull per level, the shadow cascades' picks included; a quality change re-picks, no respawn. `fixed_distance` exempts a species (a draw distance that holds on every level). Endless fields read `renderer_quality(r)` (`scatter_quality_level`) or a `quality_override` for density.
- `ScatterPlacement` carries species, transform, variant, tint, `instance_tint`, the point's rolls and an optional per-placement collider (capsule, sphere, box).
- `scatter_field_placements`, `_rebuilds`, `_rebuild_cost` expose an endless window to the game's own systems (lights, fx).

# Example usage

```c
scatter_init(w);                                        // main lane, after ecs_renderer_init
u32 field = scatter_field_create(w, r, &(ScatterFieldDesc){.mode = SCATTER_STATIC, .species = tables, .species_count = n});
// later, main lane outside renderer_update (a deferred call is fine): the draw data and thresholds, same count as the field's
f32 radius = scatter_species_radius(r, &species[0], 0, placements, count);
species[0].lod_coverage[1] = scatter_coverage_at(radius, 120.0f, 55.0f);
species[0].cull_coverage = scatter_coverage_at(radius, 900.0f, 55.0f);
species[0].fade_band = 0.2f;                            // needs lod_fade materials
scatter_static_spawn(w, r, field, species, placements, count);
// inside the frame's scope, once, after the camera:
scatter_schedule(w);
// a new world, from a system holding EcsResInOut(ScatterState):
scatter_field_clear(scatter_state, field);              // sets and placement entities go at this frame's flush; spawn again next frame
```

# Gotchas

- The spawn's species tables are the ones that count for a static field (variant counts, thresholds, colliders); the field's own tables only fix the species count.
- A static field's runtime models borrow the parts' submeshes (`renderer_create_model_borrowed`): free the submeshes only after the clear's flush.
- `fade_band > 0` draws both LODs dithered inside the band; the parts' materials need `lod_fade true`, and a fading draw skips the material's extra passes (a depth pre-pass included: the fade variant lays its own depth).
- Shadows per LOD: a part with `casts_shadow` false is shadowless at that LOD; `static_shadow` false makes it a per-frame caster (swaying parts). The renderer picks each cascade's caster LOD, and the cull, by texel footprint: a placement culled on screen can still cast, so far LODs should cast too.
- One threshold set per species, scaled per level by the renderer: a species whose distance must not scale sets `fixed_distance` (wanderer's draw-distance shrubs, its props).
- Endless species support one variant and one tint, every part of a LOD with the same shadow flags (a model instance has one); endless colliders are not implemented.
- A static field's `density_scale` is only read by the game when it calls `pg_scatter`; endless fields apply it on rebuild.
- `tint_param` gives each placement its own material instance (shared by its LODs): use it only where per-instance colour matters.
