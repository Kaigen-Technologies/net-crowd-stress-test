// net_crowd's replicated types: one avatar per player, predicted by its owner and interpolated by everyone else
#pragma once

#include "lib/reflect.h"
#include "lib/hzmath.h"
#include "ecs/ecs.h"
#include "netcode/netcode.h"
#include "gameplay/character/character.h"

#define NC_PROTOCOL 0x4e435257u // the tokens' and transport's protocol id
#define NC_PORT 4433
#define NC_WS_PORT 4434
#define NC_SPEED 4.4f           // walking, m/s
#define NC_PLAZA_HALF 224.0f    // half the plaza's side: 50,000 players at 4 m2 each
#define NC_AREA_PER_PLAYER 4u    // m2 a player at capacity by default: the spawn square's size
#define NC_TILE_METRES 4u        // the replication tile's base side (--tile); the server coarsens it where the crowd thins
#define NC_VIEW_RADIUS 16.0f    // about 200 players in view at capacity density; the engine keeps the nearest 256
#define NC_GRID 1000.0f         // the wire's position grid (1 mm): the state is on it, so a restore is bit for bit
#define NC_COLOURS 8
#define NC_HUMAN_ID (1ull << 30) // client ids from here are people (tickets); below are the bots' fleet ids
#define NC_SPECTATOR_RADIUS 24.0f
#define NC_TICK_HZ 60u
#define NC_DROP_HEIGHT 60.0f // metres every player starts the match at
#define NC_FALL_SPEED 6.0f   // m/s down until the ground in a bot's own reckoning; the server's controller falls by its gravity
#define NC_WALL 1.0f         // the plaza's walls: thickness, past NC_PLAZA_HALF
#define NC_HP 100000u        // a player's health, in thousandths
#define NC_HP_OUT (-0x40000000) // out, its avatar going at the flush's end

ECS_COMPONENT_TYPEDEF(NcBody);
HZ_REFLECT()
NET_COMPONENT()
typedef struct NcBody {
  NET_F(.quant = 1000) v3 pos;
  NET_F(.quant = 1000) v2 facing; // unit, where it last walked
} NcBody;

// set once at join: a ghost's unchanged components cost only their mask bits after the first baseline
ECS_COMPONENT_TYPEDEF(NcLook);
HZ_REFLECT()
NET_COMPONENT()
typedef struct NcLook {
  u32 name_seed; // the generated name
  u32 colour;    // palette index
  u32 body;      // 0 male, 1 female
} NcLook;

// a battle royale player's health: falls only outside the zone
ECS_COMPONENT_TYPEDEF(NcVitals);
HZ_REFLECT()
NET_COMPONENT()
typedef struct NcVitals {
  i32 hp; // thousandths, NC_HP full; at or below 0 the player is out, and the least negative of a tick's last wins
} NcVitals;

#define NC_VEL_GRID 1000.0f   // m/s on the wire: 1 mm/s
#define NC_ANGLE_GRID 10000.0f // radians and unit normals: 1e-4
#define NC_TIME_GRID 10000.0f  // the controller's timers: 0.1 ms

// the owner's predicted controller state, on the wire's grids after every step, so a rollback restores it exactly
ECS_COMPONENT_TYPEDEF(NcMotion);
HZ_REFLECT()
NET_COMPONENT(.send = NET_SEND_PREDICTED, .owner = NET_OWNER_ONLY)
typedef struct NcMotion {
  NET_F(.quant = 1000, .smooth = NET_SMOOTH_NONE) v3 velocity;
  NET_F(.quant = 10000, .smooth = NET_SMOOTH_NONE) f32 yaw;
  u32 ground; // CharacterGround
  NET_F(.quant = 10000, .smooth = NET_SMOOTH_NONE) v3 ground_normal;
  NET_F(.quant = 10000, .smooth = NET_SMOOTH_NONE) f32 coyote;
  NET_F(.quant = 10000, .smooth = NET_SMOOTH_NONE) f32 jump_buffer;
  u32 rest; // 1: the last step had no input and returned the state it started from, so the next one with none does too
} NcMotion;

ECS_COMPONENT_TYPEDEF(NcInput);
HZ_REFLECT()
NET_INPUT()
typedef struct NcInput {
  NET_F(.quant = 1000) v2 move; // x and z, length at most 1
} NcInput;

#define NC_MAP_CHUNK 256 // dots an RPC: 256 cells at up to 32 bits each fit a 1,200-byte RPC message
#define NC_MAP_BITS 12   // a dot's x and z across the map's square (4,096 steps)
#define NC_INSTANCE_WORDS 4

// the server's second, in one unpositioned ghost every connection holds: the overlay's numbers
ECS_COMPONENT_TYPEDEF(NcStats);
HZ_REFLECT()
NET_COMPONENT()
typedef struct NcStats {
  u32 second; // the server report it is from (its log line's second)
  u32 connected; // players: avatars, not spectators
  u32 tick_p50_us;
  u32 tick_p99_us;
  u32 lanes;                         // the server's worker lanes
  u32 milli_dollars_hour;            // --price-hour
  u32 map_half_dm;                   // the map's square: half its side, where players spawn and walk
  u32 instance[NC_INSTANCE_WORDS];   // --instance, 16 ASCII bytes
  // battle royale: the zone is a function of the tick, so clients draw it from these alone
  u32 royale;        // 0 off, else 1
  u32 royale_start;  // the tick everyone drops
  u32 shrink_ticks;  // after landing, the zone shrinks to nothing over these
  i32 zone_x_mm;
  i32 zone_z_mm;
  u32 zone_r0_mm;
  u32 winner_seed;   // the last one standing's name seed, once there is one
  u32 winner;        // 1 once there is a winner
} NcStats;

NET_GHOST()
typedef struct NcStatsGhost {
  NcStats stats;
} NcStatsGhost;

// client to server: send me the map (or stop)
ECS_COMPONENT_TYPEDEF(NcMapAsk);
HZ_REFLECT()
NET_RPC()
typedef struct NcMapAsk {
  u32 on;
} NcMapAsk;

// server to a client that asked: one second's dots, `count` of them from `first` of `total`
ECS_COMPONENT_TYPEDEF(NcMapChunk);
HZ_REFLECT()
NET_RPC()
typedef struct NcMapChunk {
  u32 second;
  u32 first;
  u32 total;
  u32 count;
  u32 cells[NC_MAP_CHUNK]; // x << NC_MAP_BITS | z
} NcMapChunk;

typedef enum NcSpectateWhy {
  NC_SPECTATE_CAP = 0, // the human cap is full
  NC_SPECTATE_OUT,     // eliminated
  NC_SPECTATE_RUNNING, // joined after the drop
} NcSpectateWhy;

// server to a connection without an avatar (past the cap, out, or late): the map and a camera following a player
ECS_COMPONENT_TYPEDEF(NcSpectate);
HZ_REFLECT()
NET_RPC()
typedef struct NcSpectate {
  u32 humans;
  u32 cap;
  u32 why;   // NcSpectateWhy
  u32 place; // eliminated: the place finished (2 = second)
} NcSpectate;

// spectator to server, 5 times a second: where its camera looks, so its view holds the crowd there
ECS_COMPONENT_TYPEDEF(NcSpectatorAt);
HZ_REFLECT()
NET_RPC()
typedef struct NcSpectatorAt {
  NET_F(.quant = 100) v3 pos;
} NcSpectatorAt;

force_inline u32 nc_map_cell(v3 pos, f32 half) {
  f32 scale = (f32)((1u << NC_MAP_BITS) - 1) / (2.0f * half);
  u32 x = (u32)m_clampf((pos.x + half) * scale + 0.5f, 0.0f, (f32)((1u << NC_MAP_BITS) - 1));
  u32 z = (u32)m_clampf((pos.z + half) * scale + 0.5f, 0.0f, (f32)((1u << NC_MAP_BITS) - 1));
  return x << NC_MAP_BITS | z;
}

// ---- battle royale: the drop, a shrinking zone, damage outside it, last one standing ----

typedef struct NcRoyale {
  b32 on;
  NetTick start;    // the drop
  u32 shrink_ticks; // from landing to a zone of radius 0
  v2 centre;        // the zone's last point
  f32 r0;           // its first radius: the whole spawn square
} NcRoyale;

// ---- resources (not replicated) ----

// which side a world's tick systems run for: a client simulates only what it predicts
ECS_COMPONENT_TYPEDEF(NcSide);
typedef struct NcSide {
  b32 client;
  f32 spawn_half; // server: half the side of the square players join in
  NetcodeServer *srv;
  u8 *map_asked; // [max_conns] server: the connections that asked for the map
  u32 max_conns;
  u8 *kind;      // [max_conns] server: NC_CONN_*
  u32 humans;    // people with an avatar
  u32 human_cap;
  u32 *spectators_new; // [max_conns] joined past the cap this frame: the main lane tells them
  u32 *spectators_why;   // [max_conns] NcSpectateWhy
  u32 *spectators_place; // [max_conns]
  u8 *spectators_human;  // [max_conns] people get the map; bots do not read it
  u32 spectators_new_count;
  // battle royale, server
  NcRoyale royale;
  EcsEntity *dead; // [max_conns] this tick's eliminated
  u32 dead_count;
  u32 alive; // this tick's players with health
  EcsEntity last_alive;
  u32 winner_seed;
  b32 winner;
} NcSide;

typedef enum NcConnKind {
  NC_CONN_NONE = 0,
  NC_CONN_BOT,
  NC_CONN_HUMAN,
  NC_CONN_SPECTATOR,
} NcConnKind;

// client: a ghost's bot (the skinned entity drawn) and its animation state
ECS_COMPONENT_TYPEDEF(NcVisual);
typedef struct NcVisual {
  EcsEntity bot;
  v3 last;   // the position last frame
  f32 speed; // smoothed, m/s
  b32 tinted;
  b32 running;
} NcVisual;

// client: on a bot, the ghost it shows (the bot goes when the ghost does)
ECS_COMPONENT_TYPEDEF(NcVisualOf);
typedef struct NcVisualOf {
  EcsEntity ghost;
} NcVisualOf;

NET_GHOST(.mode = NET_GHOST_OWNER_PREDICTED, .input = NcInput, .position = body.pos)
typedef struct NcAvatarGhost {
  NcBody body;
  NcMotion motion;
  NET_FIELD NcLook look;
  NET_OPTIONAL NcVitals vitals; // battle royale only
} NcAvatarGhost;

force_inline f32 nc_grid(f32 x) { return net_dq_f32(net_q_f32(x, NC_GRID), NC_GRID); }

// a bot's own reckoning of where its inputs take it, to steer to its waypoints; the server moves it with the controller
force_inline void nc_step(NcBody *b, v2 move, f32 dt) {
  f32 l = v2_length(move);
  if (l > 1.0f) {
    move = v2_scale(move, 1.0f / l);
    l = 1.0f;
  }
  if (b->pos.y > 0.0f)
    b->pos.y = nc_grid(m_maxf(b->pos.y - NC_FALL_SPEED * dt, 0.0f));
  b->pos.x = nc_grid(m_clampf(b->pos.x + move.x * NC_SPEED * dt, -NC_PLAZA_HALF, NC_PLAZA_HALF));
  b->pos.z = nc_grid(m_clampf(b->pos.z + move.y * NC_SPEED * dt, -NC_PLAZA_HALF, NC_PLAZA_HALF));
  if (l > 0.1f)
    b->facing = (v2){nc_grid(move.x / l), nc_grid(move.y / l)};
}

// every avatar's kinematic controller: the engine's defaults at a walk
force_inline CharacterTuning nc_tuning(void) {
  return character_tuning_resolve((CharacterTuning){.mover = CHARACTER_MOVER_KINEMATIC, .walk_speed = NC_SPEED});
}

force_inline u32 nc_hash(u64 x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return (u32)x;
}

// where a player with this client id joins: the same on the server and in its bot
force_inline v3 nc_spawn(u64 client_id, f32 spawn_half) {
  u32 h = nc_hash(client_id * 2 + 1), k = nc_hash(client_id * 2 + 2);
  f32 x = ((f32)(h & 0xffffff) / 16777215.0f * 2.0f - 1.0f) * spawn_half;
  f32 z = ((f32)(k & 0xffffff) / 16777215.0f * 2.0f - 1.0f) * spawn_half;
  return (v3){nc_grid(x), 0.0f, nc_grid(z)};
}

// --area: m2 a player at capacity (server and bots must agree)
force_inline f32 nc_spawn_half(u32 capacity, u32 area) {
  return m_clampf(0.5f * m_sqrtf((f32)capacity * (f32)area), 8.0f, NC_PLAZA_HALF);
}

#define NC_DROP_TICKS ((u32)(NC_DROP_HEIGHT / NC_FALL_SPEED) * NC_TICK_HZ)

// the same rules on the server and every bot; clients read them from the stats ghost
force_inline NcRoyale nc_royale_rules(f32 spawn_half, NetTick start, u32 shrink_s) {
  u32 h = nc_hash(0x52594c00000000ull | start);
  f32 a = (f32)(h & 0xffff) / 65535.0f * 2.0f * M_PI, d = (f32)(h >> 16) / 65535.0f * 0.4f * spawn_half;
  v2 c = {nc_grid(m_cosf(a) * d), nc_grid(m_sinf(a) * d)};
  return (NcRoyale){.on = true, .start = start, .shrink_ticks = shrink_s * NC_TICK_HZ, .centre = c,
                    .r0 = spawn_half * 1.4143f + d};
}

// the zone's radius at `tick`: whole until everyone lands, then linear to 0
force_inline f32 nc_zone_radius(const NcRoyale *z, NetTick tick) {
  i32 t = net_tick_since(tick, z->start) - (i32)NC_DROP_TICKS;
  if (t <= 0)
    return z->r0;
  return t >= (i32)z->shrink_ticks ? 0.0f : z->r0 * (1.0f - (f32)t / (f32)z->shrink_ticks);
}

// thousandths of health a tick outside the zone: 4 a second at landing, 20 at the end
force_inline u32 nc_zone_damage(const NcRoyale *z, NetTick tick) {
  i32 t = net_tick_since(tick, z->start) - (i32)NC_DROP_TICKS;
  f32 k = t <= 0 ? 0.0f : m_minf((f32)t / (f32)z->shrink_ticks, 1.0f);
  return (u32)((4.0f + 16.0f * k) * 1000.0f / (f32)NC_TICK_HZ + 0.5f);
}
