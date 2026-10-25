// net_crowd: a plaza of walking players on one server; --server, --join, --bots
#include "app/app.h"
#include "hz_engine.h"
#include "assets/assets.h"
#include "ecs/ecs.h"
#include "ecs/ecs.gen.h"
#include "ecs/ecs_assets/ecs_assets.h"
#include "ecs/ecs_netcode/ecs_netcode.h"
#include "ecs/ecs_physics/ecs_physics.h"
#include "ecs/input/ecs_input.h"
#include "ecs/renderer/ecs_renderer.h"
#include "ecs/renderer/ecs_renderer.gen.h"
#include "ecs/transform/ecs_transform.h"
#include "ecs/transform/ecs_transform.gen.h"
#include "animation/animation.h"
#include "input.h"
#include "gpu/gpu.h"
#include "lib/allocator_tlsf.h"
#include "lib/crypto/crypto.h"
#include "lib/hzmath.h"
#include "lib/net/net.h"
#include "lib/radix_sort.h"
#include "lib/string.h"
#include "lib/string_builder.h"
#include "lib/thread_context.h"
#include "os/os.h"
#include "procgen/pg_geo.h"
#include "rendering/camera.h"
#include "rendering/renderer.h"
#include "ui/ui.h"
#include "generated/types_app.gen.h"
#include "net_crowd/nc_game.h"
#include "net_crowd/nc_names.h"
#include "net_crowd/nc_game.gen.h"
#include "generated/netcode_app.gen.h"

// the app; a test binary links only the game's types and helpers
#ifndef HZ_TEST

// a window paces on its display; a server, bots and a minimized window pace themselves on the tick
// the frame at most 1080p: 256 skinned players at a laptop's full retina resolution are GPU-bound (measured)
// server, bots and the replay check run GPU-less with the engine's --no-gpu
const HzAppConfig hz_app_config = {.window_title = "net_crowd", .target_frame_time_ms = -1.0f, .max_pixels = 1920 * 1080};

#define NC_TEXT_MAX 256
#define NC_ACME_DIRECTORY "https://acme-v02.api.letsencrypt.org/directory"
#define NC_REPORT_FRAMES 256 // a second's ticking frames, at most
#define NC_PHASES 3          // a server frame's receive, simulate, and gather + write + send
#define NC_HELD_BUCKETS 10   // the views histogram: 8 buckets of 32 ghosts, more than 256, then the largest view
#define NC_TICK_US 16667
#define NC_BOTS_DRAWN_MAX 512 // the nearest 256 a view holds, those still leaving it, and the own
#define NC_BOT_BONES 66       // the Kaigen bot's 65 joints and a skin bind
#define NC_CMD_RESERVED MB(512) // a lane's command buffer when a frame may spawn the whole plaza's bodies (50,000 at once)

#include "nc_bots.c"
#include "nc_view.c"
#include "nc_map.c"
#include "nc_record.c"
#include "nc_oracle.c"

typedef enum NcRole {
  NC_HOST = 0, // a listen server with a local player
  NC_JOIN,     // a client
  NC_SERVER,   // a dedicated server
  NC_BOTS,     // players for a server's load
  NC_REPLAY,   // a recording, drawn
} NcRole;

typedef struct AppState {
  EcsWorld *world;        // the local player's, or a dedicated server's
  EcsWorld *server_world; // a host's own or a dedicated server's; NULL on a client
  ArenaAllocator *arena;
  TLSFAllocator *tlsf;
  Allocator net_alloc;
  Netcode *net;
  NetcodeArgs args;
  u32 role;
  b32 draws; // a window: the renderer and UI exist
  u32 capacity;
  u32 area; // --area: m2 a player at capacity
  char *public_host; // [NC_TEXT_MAX] what tickets and the bots line name
  char *join_host;   // [NC_TEXT_MAX] --join-host: what the join lines' tickets name (default public_host)
  u8 *key;           // [NET_TOKEN_KEY_BYTES] the connect tokens' key, shared with the bots
  b32 announced;
  u64 frame_at, next_frame;
  u32 last_frame_us; // a window's previous frame, start to its renderer submit, on the main lane
  // the server's second
  u64 report_at;
  NetTick report_tick;
  u32 busy_count;
  u32 *busy_us; // [NC_REPORT_FRAMES] a server's ticking frames' work, a window's frame-to-frame times
  NetcodeEncodeStats enc_last; // the server's tile encode counts at the last report
  NetcodeFieldSendCounters field_last; // the field's sends over the connections at the last report
  NetcodeLinkStats human_last;         // the first human connection's link counters at the last report
  NetcodeFieldSendCounters human_field; // and its field's sends
  u32 human_conn;                      // and its id + 1 (0: none yet)
  NetcodeLinkTransport tr_last; // and its connections' packet totals
  u32 *phase_us; // [NC_PHASES * NC_REPORT_FRAMES] a server's ticking frames' work by phase, at busy_us's index
  b32 report_due; // the server's second ends this frame: every lane counts its share of the views first
  u32 *held_hist; // [HZ_MAX_LANES * NC_HELD_BUCKETS] each lane's views histogram, its last bucket the largest view
  u32 *cpu_us;  // [NC_REPORT_FRAMES] a window's frame work on the main lane
  NetcodeStats stats_last;
  // a window's
  Camera camera;
  UISystem *ui;
  NcBotAssets bot_assets;
  HzHandleT(RendererInstanceBatch) floor;
  f64 rate_at;
  f64 report_at_s; // a window's last frame report
  f32 snapshots_s;
  NetcodeStats rate_last;
  NcMapServer map_server;
  NcMapClient map;
  u32 second; // the server reports so far
  b32 royale;     // --royale: a battle royale from the announce plus --royale-lobby seconds
  u32 royale_lobby_s, royale_shrink_s;
  u32 map_hz;          // --map-hz: map sends a second to those that asked (royale 5)
  NetTick map_sent;    // the tick of the last map send
  EcsEntity followed;  // a spectator's: the player its camera follows
  b32 winner_logged;
  b32 free_cam;  // a spectator moved its camera itself: it follows nobody until F
  v3 own_last;   // where this client's avatar last was
  u32 spectate_why; // NcSpectateWhy
  b32 spectator; // a client past the server's human cap
  v3 spec_pos;   // a spectator's free camera target
  f64 spec_sent_at;
  NcRecorder rec;
  NcReplay replay;
  f32 shot_rise_s; // --shot-rise: a replay's camera climbs from eye level to the whole plaza over this many seconds
  u32 shots;       // --frames-out: frames captured
  f32 replay_t;    // the recording's clock: with --frames-out it moves only when a frame is captured
  f32 frame_dt;    // every lane: this frame's dt; capturing, 1/K of the output frame
  u32 capture_every; // --capture-every K: app frames an output frame (the capture slot's latency)
  u32 frame_ms;      // --frame-ms N: a joining client's frames last at least N ms (a slow client, for diagnosis)
  u32 hitch_ms, hitch_every, hitch_n; // --hitch-ms N --hitch-every K: every Kth frame lasts at least N ms (for diagnosis)
  u32 capture_s;     // --capture-seconds S: a live client captures only its first S seconds past the join (0: all)
  f64 capture_from;  // when its captures began
  u32 live_frames;
  b32 field_whole; // the join's loading phase is over: every player on the server is held
  f64 join_at;     // when the first snapshot arrived
  u32 shots_missed; // the slot was still busy when a frame was due: that output frame is missing
  b32 replay_live; // the first bodies are drawn: the recording's clock runs
  v3 cam_look;      // the point the window's camera looks at this frame
  NcFidelityLog flog; // --fidelity-log: a client's held players each frame
  NcOracle oracle;    // --oracle: the log against the recording
  b32 oracle_on;
  NcFollow follow; // --follow <log>: a replay drawn at the log's captured frames, from its camera
#ifdef NET_HAS_UDP
  NcBots bots;
#endif
} AppState;

#include "nc_sim.c"

hz_internal void nc_simulate(void *user, EcsWorld *w) {
  AppState *s = user;
  nc_schedule_tick(w, w == s->server_world);
}

hz_internal void nc_static_box(EcsWorld *w, v3 centre, v3 half) {
  EcsEntity e = ecs_entity_create(w);
  ecs_component_set(w, e, ColliderBody, {.collider = collider_box(V3_ZERO, half), .pos = centre, .rot = quat_identity(), .scale = 1.0f});
  ecs_component_add(w, e, StaticBody);
}

// the plaza every controller walks on: the floor's top at y 0, the walls' inner faces at +-NC_PLAZA_HALF
hz_internal void nc_plaza_colliders(EcsWorld *w) {
  f32 h = NC_PLAZA_HALF, t = NC_WALL, o = h + 0.5f * t;
  PhysicsSettings ps = physics_settings_default(aabb_make((v3){-h - 2.0f * t, -2.0f, -h - 2.0f * t},
                                                          (v3){h + 2.0f * t, NC_DROP_HEIGHT + 8.0f, h + 2.0f * t}),
                                                (iv3){8, 1, 8});
  ecs_physics_init(w, ps.collision_layer_settings);
  nc_static_box(w, (v3){0.0f, -0.5f, 0.0f}, (v3){h + t, 0.5f, h + t});
  nc_static_box(w, (v3){o, 2.0f, 0.0f}, (v3){0.5f * t, 2.0f, h + t});
  nc_static_box(w, (v3){-o, 2.0f, 0.0f}, (v3){0.5f * t, 2.0f, h + t});
  nc_static_box(w, (v3){0.0f, 2.0f, o}, (v3){h + t, 2.0f, 0.5f * t});
  nc_static_box(w, (v3){0.0f, 2.0f, -o}, (v3){h + t, 2.0f, 0.5f * t});
}

hz_internal void nc_world_init(AppState *s, EcsWorld *w, b32 server) {
  ecs_resource_set(w, NcSide, {.client = !server, .spawn_half = nc_spawn_half(s->capacity, s->area)});
  nc_plaza_colliders(w);
}

// main lane: --capacity N players (the spawn square and the server's limits), --key <hex> (else random), --public-host
hz_internal b32 nc_net_init(AppState *s) {
  Allocator a = make_arena_allocator(s->arena);
  s->key = ALLOC_ARRAY(&a, u8, NET_TOKEN_KEY_BYTES);
  s->public_host = ALLOC_ARRAY(&a, char, NC_TEXT_MAX);
  s->join_host = ALLOC_ARRAY(&a, char, NC_TEXT_MAX);
  u32 net_interval = 1;
  // tiles in metres: shared per-tile encodes, sized to the crowd by the server; 0 replicates per ghost
  u32 tile = 0;
  // transport slots (0: one per lane); more than the lanes lets a lane slowed by interrupts hand its share to others
  u32 slots = 0;
  if (!nc_arg_u32("--capacity", 64, &s->capacity) || !s->capacity || !nc_arg_u32("--net-interval", 1, &net_interval) ||
      !nc_arg_u32("--area", NC_AREA_PER_PLAYER, &s->area) || !s->area || !nc_arg_u32("--tile", NC_TILE_METRES, &tile) ||
      !nc_arg_u32("--slots", 0, &slots)) {
    log_error("net_crowd: --capacity, --area, --net-interval, --tile and --slots take a number");
    return false;
  }
  if (hz_arg_value("--key") && !nc_arg_hex("--key", s->key, NET_TOKEN_KEY_BYTES)) {
    log_error("net_crowd: --key takes % hex digits", fmt_u32(2 * NET_TOKEN_KEY_BYTES));
    return false;
  }
  if (!hz_arg_value("--key")) {
    CryptoRng rng;
    crypto_rng_init(&rng);
    crypto_rng_fill(&rng, s->key, NET_TOKEN_KEY_BYTES);
  }
  const char *host = hz_arg_value("--public-host");
  cstr_copy(s->public_host, (char *)(host ? host : "127.0.0.1"), NC_TEXT_MAX);
  const char *jhost = hz_arg_value("--join-host");
  cstr_copy(s->join_host, (char *)(jhost ? jhost : s->public_host), NC_TEXT_MAX);
  nc_world_init(s, s->world, s->role == NC_SERVER);
  if (s->server_world && s->server_world != s->world)
    nc_world_init(s, s->server_world, true);
  // --cert-acme <public ip>: a Let's Encrypt IP certificate, so Safari (no certificate hashes) can join; bots pin it too
  const char *acme = hz_arg_value("--cert-acme");
  // --cert-chain <pem> --cert-key <pem>: a certificate of one's own (a LAN name's, for a phone on the same network)
  const char *chain = hz_arg_value("--cert-chain");
  // --net-sim <lag=,jitter=,loss=%,...>: every connection of this server over a simulated link
  NetSimProfile sim = {0};
  const char *sim_text = hz_arg_value("--net-sim");
  u32 sim_conn = 0;
  if (sim_text && !netcode_sim_parse((CString)sim_text, &sim_conn, &sim)) {
    log_error("net_crowd: --net-sim: not a link profile (lag=,jitter=,loss=%,dup=%,reorder=%,kbps=)");
    return false;
  }
  // --sphere inner,outer,outer_priority,outside_priority; by default every held player (within 1.25 x 18 m) at full weight
  NetPrioritySphere sphere = {.inner_radius = 2.0f * NC_VIEW_RADIUS, .outer_radius = 4.0f * NC_VIEW_RADIUS,
                              .inner_priority = 1.0f, .outer_priority = 0.2f, .outside_priority = 0.1f};
  const char *sp = hz_arg_value("--sphere");
  if (sp) {
    f32 *f[4] = {&sphere.inner_radius, &sphere.outer_radius, &sphere.outer_priority, &sphere.outside_priority};
    StrSplitIter it = str_split_iter(cstr_to_str(sp), STR_FROM_CSTR(","));
    String field;
    u32 k = 0;
    f64 v = 0.0;
    while (str_split_iter_next(&it, &field) && k < 4 && str_to_f64(field, &v))
      *f[k++] = (f32)v;
    if (k != 4 || it.index >= 0 || sphere.inner_radius >= sphere.outer_radius) {
      log_error("net_crowd: --sphere takes inner,outer,outer_priority,outside_priority (inner below outer)");
      return false;
    }
    sphere.inner_priority = 1.0f;
  }
  s->net = ecs_netcode_init(
      s->world, &(NetcodeDesc){
                    .role = s->args.role,
                    .server_world = s->role == NC_HOST ? s->server_world : NULL,
                    .registry = &app_netcode_registry,
                    .simulate = nc_simulate,
                    .user = s,
                    .protocol_id = NC_PROTOCOL,
                    .server = {.port = NC_PORT,
                               .ws_port = NC_WS_PORT,
                               .cert = acme    ? (NetCertDesc){.mode = NET_CERT_ACME, .name = acme,
                                                               .acme_directory = NC_ACME_DIRECTORY, .state_dir = "acme"}
                                       : chain ? (NetCertDesc){.mode = NET_CERT_FILES, .chain_path = chain,
                                                               .key_path = hz_arg_value("--cert-key")}
                                               : (NetCertDesc){.mode = NET_CERT_FINGERPRINT, .name = "localhost"},
                               .token_key = s->key,
                               .net_interval = net_interval,
                               .slots = slots,
                               .replication = {.max_ghosts = s->capacity + s->capacity / 4 + 64,
                                               .max_connections = s->capacity,
                                               .sphere = sphere,
                                               .tile_size = (f32)tile,
                                               .field = {.on = true},
                                               .play_space = {{-NC_PLAZA_HALF - 1.0f, -4.0f, -NC_PLAZA_HALF - 1.0f},
                                                              {NC_PLAZA_HALF + 1.0f, 4.0f, NC_PLAZA_HALF + 1.0f}}}},
                    // --always-rollback: re-simulate from every snapshot, so a resim that differs from the forward run shows as mispredicted
                    .client = {.ticket = s->args.ticket, .ticket_len = s->args.ticket_len, .network_sim = true,
                               .replication = {.always_rollback = hz_arg_flag("--always-rollback")}},
                    .sim = sim,
                    .dev = s->args.dev,
                    .alloc = &s->net_alloc,
                });
  if (!s->net)
    return false;
  if (s->server_world) {
    ecs_resource_get_mut(s->server_world, NcSide)->srv = netcode_server(s->net);
    nc_map_server_init(&s->map_server, s->server_world, &a, s->capacity, s->capacity + s->capacity / 4 + 64);
    NcSide *side = ecs_resource_get_mut(s->server_world, NcSide);
    side->kind = ALLOC_ARRAY(&a, u8, s->capacity);
    side->spectators_new = ALLOC_ARRAY(&a, u32, s->capacity);
    side->spectators_why = ALLOC_ARRAY(&a, u32, s->capacity);
    side->spectators_place = ALLOC_ARRAY(&a, u32, s->capacity);
    side->spectators_human = ALLOC_ARRAY(&a, u8, s->capacity);
    side->dead = ALLOC_ARRAY(&a, EcsEntity, s->capacity);
    s->royale = hz_arg_flag("--royale");
    if (!nc_arg_u32("--royale-lobby", 20, &s->royale_lobby_s) || !nc_arg_u32("--royale-shrink", 120, &s->royale_shrink_s) ||
        !s->royale_shrink_s || !nc_arg_u32("--map-hz", s->royale ? 5 : 1, &s->map_hz) || !s->map_hz || s->map_hz > 60) {
      log_error("net_crowd: --royale-lobby, --royale-shrink and --map-hz (1-60) take a number");
      return false;
    }
    if (!nc_arg_u32("--human-cap", s->capacity, &side->human_cap)) {
      log_error("net_crowd: --human-cap takes a number");
      return false;
    }
    const char *rec = hz_arg_value("--crowd-record");
    if (rec && !nc_record_open(&s->rec, &a, rec, s->capacity, s->capacity + s->capacity / 4 + 64, s->map_server.half))
      return false;
  }
  if (s->draws && s->role != NC_SERVER) {
    nc_map_client_init(&s->map, &a);
    s->map.want = hz_arg_flag("--map");
  }
  return true;
}

// main lane, once the server listens: the line that starts bots against it, and a player's join ticket
hz_internal void nc_announce(AppState *s) {
  if (s->announced || !s->server_world || netcode_state(s->net) != NETCODE_RUNNING)
    return;
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  u8 *ticket = ALLOC_ARRAY(&ta.allocator, u8, NETCODE_TICKET_MAX);
  char *hex = ALLOC_ARRAY(&ta.allocator, char, 2 * NETCODE_TICKET_MAX + 1);
  char *pin = ALLOC_ARRAY(&ta.allocator, char, 65);
  char *key = ALLOC_ARRAY(&ta.allocator, char, 2 * NET_TOKEN_KEY_BYTES + 1);
  u32 len = netcode_server_ticket(s->net, &(NetcodeTicketDesc){.client_id = NC_HUMAN_ID, .host = s->join_host, .expire_s = 3600},
                                  ticket, NETCODE_TICKET_MAX);
  if (len && netcode_ticket_nonce(ticket, len, pin, 65) && netcode_ticket_to_hex(ticket, len, hex, 2 * NETCODE_TICKET_MAX + 1) &&
      netcode_ticket_to_hex(s->key, NET_TOKEN_KEY_BYTES, key, 2 * NET_TOKEN_KEY_BYTES + 1)) {
    s->announced = true;
    String royale = str_format(&ta.allocator, "");
    if (s->royale) {
      NcSide *side = ecs_resource_get_mut(s->server_world, NcSide);
      NetTick start = net_tick_add(netcode_stats(s->net).tick, s->royale_lobby_s * NC_TICK_HZ);
      side->royale = nc_royale_rules(side->spawn_half, start, s->royale_shrink_s);
      royale = str_format(&ta.allocator, "--royale % --royale-shrink %", fmt_u32(start), fmt_u32(s->royale_shrink_s));
      log_info("nc royale: the drop at tick % (in % s), the zone shrinks to (% %) over % s after landing", fmt_u32(start),
               fmt_u32(s->royale_lobby_s), fmt_f32_p(side->royale.centre.x, 1), fmt_f32_p(side->royale.centre.y, 1),
               fmt_u32(s->royale_shrink_s));
    }
    log_info("nc bots line: --no-gpu --bots <n> --first-id <id> --server %:% --key % --pin % --capacity % --area % %",
             fmt_cstr(s->public_host), fmt_u32(NC_PORT), fmt_cstr(key), fmt_cstr(pin), fmt_u32(s->capacity), fmt_u32(s->area),
             fmt_str(royale));
    // the engine's net_loadgen tool: players that connect and ack without the game's input
    u64 rh = netcode_registry_hash(&app_netcode_registry);
    u8 *rh_bytes = ALLOC_ARRAY(&ta.allocator, u8, 8);
    for (u32 i = 0; i < 8; i++)
      rh_bytes[i] = (u8)(rh >> (8 * i));
    char *rh_hex = ALLOC_ARRAY(&ta.allocator, char, 17);
    if (netcode_ticket_to_hex(rh_bytes, 8, rh_hex, 17))
      log_info("nc loadgen line: --players <n> --first-id <id> --server %:% --key % --pin % --hash % --protocol %",
               fmt_cstr(s->public_host), fmt_u32(NC_PORT), fmt_cstr(key), fmt_cstr(pin), fmt_cstr(rh_hex),
               fmt_u32(NC_PROTOCOL));
    log_info("nc join line: --join %", fmt_cstr(hex));
    // more people, each its own client id (one id joining twice replaces its first connection)
    for (u32 k = 1; k < 4; k++) {
      u32 n = netcode_server_ticket(s->net, &(NetcodeTicketDesc){.client_id = NC_HUMAN_ID + k, .host = s->join_host, .expire_s = 3600},
                                    ticket, NETCODE_TICKET_MAX);
      if (n && netcode_ticket_to_hex(ticket, n, hex, 2 * NETCODE_TICKET_MAX + 1))
        log_info("nc join line %: --join %", fmt_u32(k), fmt_cstr(hex));
    }
  }
  tctx_temp_allocator_end(ta);
}

// every lane, before the second's report: the ghosts each of its range of connections holds, in 32-wide buckets
hz_internal void nc_views_share(AppState *s) {
  u32 *h = s->held_hist + tctx_current()->thread_idx * NC_HELD_BUCKETS;
  mem_set(h, 0, NC_HELD_BUCKETS * sizeof(u32));
  const NetcodeServer *srv = netcode_server(s->net);
  Range(u32) r = lane_range(s->capacity);
  for (u32 c = r.min; c < r.max; c++) {
    if (!netcode_server_verified(srv, c))
      continue;
    u32 n = netcode_server_held(srv, c);
    h[n > 256 ? 8 : (n ? (n - 1) / 32 : 0)]++;
    h[NC_HELD_BUCKETS - 1] = MAX(h[NC_HELD_BUCKETS - 1], n);
  }
}

// main lane, once a second: connections, tick rate and each ticking frame's work (p50, p99, max), snapshots out
hz_internal const char *nc_stage_name(u32 k) {
  switch (k) {
  case NETCODE_STAGE_RESOLVE: return "resolve";
  case NETCODE_STAGE_TILES: return "tiles";
  case NETCODE_STAGE_CANDIDATES: return "candidates";
  case NETCODE_STAGE_BODIES: return "bodies";
  case NETCODE_STAGE_BLOCKS: return "blocks";
  case NETCODE_STAGE_CHOOSE: return "choose";
  case NETCODE_STAGE_EMIT: return "emit";
  case NETCODE_STAGE_FINISH: return "finish";
  case NETCODE_STAGE_FIELD: return "field";
  case NETCODE_STAGE_SEND: return "send";
  default: return "?";
  }
}

hz_internal u32 nc_server_report(AppState *s, u64 now) {
  if (!s->report_due)
    return 0;
  NetcodeStats st = netcode_stats(s->net), o = s->stats_last;
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  u32 n = s->busy_count;
  u32 *tmp = ALLOC_ARRAY_NO_ZERO(&ta.allocator, u32, n ? n : 1);
  RadixSortU32Out sorted = radix_sort_u32(s->busy_us, NULL, tmp, NULL, n, (RadixSortDesc){0});
  const u32 *b = sorted.keys;
  f64 span = (f64)(now - s->report_at + 1000000) / 1e6;
  // the density of views: ghosts each client holds, in 32-wide buckets, the last for more than 256
  u32 *hist = ALLOC_ARRAY(&ta.allocator, u32, NC_HELD_BUCKETS);
  u32 held_max = 0;
  for (u32 l = 0; l < tctx_current()->thread_count; l++)
    for (u32 k = 0; k < NC_HELD_BUCKETS; k++) {
      const u32 *h = s->held_hist + l * NC_HELD_BUCKETS;
      if (k < NC_HELD_BUCKETS - 1)
        hist[k] += h[k];
      else
        held_max = MAX(held_max, h[k]);
    }
  log_info("nc views: held 1-32 % 33-64 % 65-96 % 97-128 % 129-160 % 161-192 % 193-224 % 225-256 % over 256 %; max %",
           fmt_u32(hist[0]), fmt_u32(hist[1]), fmt_u32(hist[2]), fmt_u32(hist[3]), fmt_u32(hist[4]), fmt_u32(hist[5]),
           fmt_u32(hist[6]), fmt_u32(hist[7]), fmt_u32(hist[8]), fmt_u32(held_max));
  // ghost entries encoded once per tile and baselines, reused by other viewers, or encoded for one viewer alone
  NetcodeEncodeStats enc = netcode_server_encode_stats(netcode_server(s->net));
  // sums over the connections held now: one that left takes its share, so a second's change floors at 0
#define NC_DELTA(f) (enc.f > s->enc_last.f ? enc.f - s->enc_last.f : 0)
  u64 e_sh = NC_DELTA(shared), e_en = NC_DELTA(encoded), e_pr = NC_DELTA(priv);
  u64 e_bl = NC_DELTA(blocks), e_fr = NC_DELTA(fresh), e_ag = NC_DELTA(age);
#undef NC_DELTA
  s->enc_last = enc;
  log_info("nc encode: second %: entries encoded % shared % private % (% sends an encode); blocks % fresh % newest baseline % ticks old",
           fmt_u32(s->second + 1), fmt_u64(e_en), fmt_u64(e_sh), fmt_u64(e_pr),
           fmt_f64_p(e_en + e_pr ? (f64)(e_en + e_sh + e_pr) / (f64)(e_en + e_pr) : 0.0, 2), fmt_u64(e_bl), fmt_u64(e_fr),
           fmt_f64_p(e_bl > e_fr ? (f64)e_ag / (f64)(e_bl - e_fr) : 0.0, 2));
  // the field's sends: totals over the connections this second, and the connection with the most keyframe bits so far
  NetcodeServer *srv = netcode_server(s->net);
  NetcodeFieldSendCounters fs = {0}, worst = {0};
  u32 worst_conn = 0;
  for (u32 c = 0; c < s->capacity; c++) {
    NetcodeFieldSendCounters fc = netcode_server_field_counters(srv, c);
    fs.keys += fc.keys, fs.deltas += fc.deltas, fs.events += fc.events, fs.key_bits += fc.key_bits;
    fs.delta_bits += fc.delta_bits, fs.owed += fc.owed, fs.lost += fc.lost, fs.asks += fc.asks;
    if (fc.key_bits > worst.key_bits)
      worst = fc, worst_conn = c;
  }
#define NC_DELTA(f) (fs.f > s->field_last.f ? fs.f - s->field_last.f : 0)
  log_info("nc field sends: second %: keyframes % (% KB) deltas % (% KB) events % owed % lost % asked %; most keyframe bits: "
           "connection % keyframes % (% KB) deltas % (% KB) owed % lost % asked % since its join",
           fmt_u32(s->second + 1), fmt_u64(NC_DELTA(keys)), fmt_u64(NC_DELTA(key_bits) / 8000), fmt_u64(NC_DELTA(deltas)),
           fmt_u64(NC_DELTA(delta_bits) / 8000), fmt_u64(NC_DELTA(events)), fmt_u64(NC_DELTA(owed)), fmt_u64(NC_DELTA(lost)),
           fmt_u64(NC_DELTA(asks)), fmt_u32(worst_conn), fmt_u64(worst.keys), fmt_u64(worst.key_bits / 8000), fmt_u64(worst.deltas),
           fmt_u64(worst.delta_bits / 8000), fmt_u64(worst.owed), fmt_u64(worst.lost), fmt_u64(worst.asks));
#undef NC_DELTA
  s->field_last = fs;
  // the first human connection (the camera): what its snapshots were allowed and what they carried
  const NcSide *hside = s->server_world ? ecs_resource_get(s->server_world, NcSide) : NULL;
  if (hside && (!s->human_conn || hside->kind[s->human_conn - 1] != NC_CONN_HUMAN)) {
    s->human_conn = 0;
    s->human_last = (NetcodeLinkStats){0};
    s->human_field = (NetcodeFieldSendCounters){0};
    for (u32 c = 0; c < s->capacity && !s->human_conn; c++)
      s->human_conn = hside->kind[c] == NC_CONN_HUMAN ? c + 1 : 0;
  }
  if (s->human_conn) {
    NetcodeLinkStats hs = netcode_connection_link_stats(s->net, s->human_conn - 1), hl = s->human_last;
    u64 sn = hs.snapshots - hl.snapshots;
    log_info("nc human: second %: connection % snapshots % (% B mean, cap % B mean), cut % skipped % parted %, field % messages % KB",
             fmt_u32(s->second + 1), fmt_u32(s->human_conn - 1), fmt_u64(sn),
             fmt_u64(sn ? (hs.snapshot_bytes - hl.snapshot_bytes) / sn : 0), fmt_u64(sn ? (hs.snapshot_cap - hl.snapshot_cap) / sn : 0),
             fmt_u64(hs.cut - hl.cut), fmt_u64(hs.skipped - hl.skipped), fmt_u64(hs.parted - hl.parted),
             fmt_u64(hs.field_messages - hl.field_messages), fmt_u64((hs.field_bytes - hl.field_bytes) / 1024));
    s->human_last = hs;
    NetcodeFieldSendCounters hf = netcode_server_field_counters(srv, s->human_conn - 1), hp = s->human_field;
    log_info("nc human field: second %: keyframes % (% KB) deltas % (% KB) events % owed % lost % asked %", fmt_u32(s->second + 1),
             fmt_u64(hf.keys - hp.keys), fmt_u64((hf.key_bits - hp.key_bits) / 8192), fmt_u64(hf.deltas - hp.deltas),
             fmt_u64((hf.delta_bits - hp.delta_bits) / 8192), fmt_u64(hf.events - hp.events), fmt_u64(hf.owed - hp.owed),
             fmt_u64(hf.lost - hp.lost), fmt_u64(hf.asks - hp.asks));
    s->human_field = hf;
  }
  s->second++;
  // ticking frames over the tick's budget, exactly: a window's p99 is within budget when at most 1% of its ticks are
  u32 over = 0;
  while (over < n && b[n - 1 - over] > 1000000u / NC_TICK_HZ)
    over++;
  u32 players = nc_map_server_second(&s->map_server, s->server_world, s->net, s->second, n ? b[n / 2] : 0,
                                     n ? b[(n * 99) / 100] : 0);
  log_info("nc server: second %: % players, % connections, % ticks/s; tick work us p50 % p99 % max %; snapshots % /s, % KB/s; "
           "skipped % refused % starved % cut % in parts %; ticks % over budget %",
           fmt_u32(s->second), fmt_u32(players), fmt_u32(st.connections), fmt_f64_p((f64)net_tick_since(st.tick, s->report_tick) / span, 1),
           fmt_u32(n ? b[n / 2] : 0), fmt_u32(n ? b[(n * 99) / 100] : 0), fmt_u32(n ? b[n - 1] : 0),
           fmt_f64_p((f64)(st.server_link.snapshots - o.server_link.snapshots) / span, 0),
           fmt_f64_p((f64)(st.server_link.snapshot_bytes - o.server_link.snapshot_bytes) / 1024.0 / span, 0),
           fmt_u64(st.server_link.skipped - o.server_link.skipped), fmt_u64(st.server_link.refused - o.server_link.refused),
           fmt_u64(st.server_link.starved), fmt_u64(st.server_link.cut - o.server_link.cut),
           fmt_u64(st.server_link.parted - o.server_link.parted), fmt_u32(n), fmt_u32(over));
  u32 *ph = ALLOC_ARRAY_NO_ZERO(&ta.allocator, u32, NC_PHASES * (n ? n : 1));
  const u32 **pk = ALLOC_ARRAY_NO_ZERO(&ta.allocator, const u32 *, NC_PHASES);
  for (u32 k = 0; k < NC_PHASES; k++)
    pk[k] = radix_sort_u32(s->phase_us + k * NC_REPORT_FRAMES, NULL, ph + k * (n ? n : 1), NULL, n, (RadixSortDesc){0}).keys;
  const u32 *pr = pk[0], *ps = pk[1], *pw = pk[2];
  NetcodeLinkTransport tr = netcode_transport(s->net);
  // totals over the connections held now: one that left takes its share with it
  u64 lost = tr.packets_lost > s->tr_last.packets_lost ? tr.packets_lost - s->tr_last.packets_lost : 0;
  u64 psent = tr.packets_sent > s->tr_last.packets_sent ? tr.packets_sent - s->tr_last.packets_sent : 0;
  u64 bsent = tr.bytes_sent > s->tr_last.bytes_sent ? tr.bytes_sent - s->tr_last.bytes_sent : 0;
  u64 dsent = tr.datagrams_sent > s->tr_last.datagrams_sent ? tr.datagrams_sent - s->tr_last.datagrams_sent : 0;
  u64 asent = tr.ack_only_sent > s->tr_last.ack_only_sent ? tr.ack_only_sent - s->tr_last.ack_only_sent : 0;
  u64 hsent = tr.held_sent > s->tr_last.held_sent ? tr.held_sent - s->tr_last.held_sent : 0;
  s->tr_last = tr;
  log_info("nc transport: second %: % connections; cwnd p10 % p50 %; allowance p10 % p50 %; in flight p50 %; srtt us p50 % p99 %; "
           "packets lost % of % sent, % B each, % datagrams each, % ack-only, % outside the flush",
           fmt_u32(s->second), fmt_u32(tr.conns), fmt_u64(tr.cwnd_p10), fmt_u64(tr.cwnd_p50), fmt_u64(tr.allowance_p10),
           fmt_u64(tr.allowance_p50), fmt_u64(tr.in_flight_p50), fmt_u64(tr.srtt_p50_us), fmt_u64(tr.srtt_p99_us),
           fmt_u64(lost), fmt_u64(psent), fmt_u64(psent ? bsent / psent : 0), fmt_f64_p(psent ? (f64)dsent / (f64)psent : 0.0, 2),
           fmt_u64(asent), fmt_u64(hsent));
  log_info("nc phases: second %: receive us p50 % p99 %; simulate p50 % p99 %; gather+write+send p50 % p99 %", fmt_u32(s->second),
           fmt_u32(n ? pr[n / 2] : 0), fmt_u32(n ? pr[(n * 99) / 100] : 0), fmt_u32(n ? ps[n / 2] : 0),
           fmt_u32(n ? ps[(n * 99) / 100] : 0), fmt_u32(n ? pw[n / 2] : 0), fmt_u32(n ? pw[(n * 99) / 100] : 0));
  // where the lanes' time went: each phase's wall over the second's ticking frames times the lanes, against the work in it
  u64 *wall_us = ALLOC_ARRAY(&ta.allocator, u64, NC_PHASES);
  for (u32 k = 0; k < NC_PHASES; k++)
    for (u32 f = 0; f < n; f++)
      wall_us[k] += s->phase_us[k * NC_REPORT_FRAMES + f];
  u32 lanes = tctx_current()->thread_count;
  log_info("nc walls: second %: % ticking frames, % lanes; lane-ms receive % simulate % gather+write+send %", fmt_u32(s->second),
           fmt_u32(n), fmt_u32(lanes), fmt_f64_p((f64)wall_us[0] * lanes / 1000.0, 0), fmt_f64_p((f64)wall_us[1] * lanes / 1000.0, 0),
           fmt_f64_p((f64)wall_us[2] * lanes / 1000.0, 0));
  NetcodeStageTimes stt = netcode_server_stage_times(netcode_server(s->net));
  StringBuilder sb = sb_create(KB(4), &ta.allocator);
  for (u32 k = 0; k < NETCODE_SERVER_STAGES; k++)
    sb_append_format(&sb, " %: % / %;", fmt_cstr(nc_stage_name(k)), fmt_f64_p((f64)stt.ns[k] / 1e6, 1),
                     fmt_f64_p((f64)stt.max_ns[k] / 1e6, 1));
  log_info("nc stages: second %: % lanes, ms all lanes / busiest lane:%", fmt_u32(s->second), fmt_u32(stt.lanes),
           fmt_cstr(sb_get(&sb)));
  EcsNetcodePhaseTime *pt = ALLOC_ARRAY_NO_ZERO(&ta.allocator, EcsNetcodePhaseTime, 96);
  u32 pn = ecs_netcode_phase_times(s->world, pt, 96);
  StringBuilder sp = sb_create(KB(8), &ta.allocator);
  for (u32 k = 0; k < pn; k++)
    sb_append_format(&sp, " %: % / % (%);", fmt_cstr(pt[k].name), fmt_f64_p((f64)pt[k].busy_ns / 1e6, 1),
                     fmt_f64_p((f64)pt[k].max_ns / 1e6, 1), fmt_u32(pt[k].lanes));
  log_info("nc systems: second %: ms all lanes / busiest lane (lanes):%", fmt_u32(s->second), fmt_cstr(sb_get(&sp)));
  EcsFlushTimes ft = ecs_flush_times(s->world);
  log_info("nc flush: second %: % flushes on % lanes; ms main scheduling % commands %; all lanes / busiest: flush % / % queue % / % "
           "tasks % / %",
           fmt_u32(s->second), fmt_u32(ft.flushes), fmt_u32(ft.lanes), fmt_f64_p((f64)ft.append_ns / 1e6, 1),
           fmt_f64_p((f64)ft.commands_ns / 1e6, 1), fmt_f64_p((f64)ft.flush_ns / 1e6, 0), fmt_f64_p((f64)ft.flush_max_ns / 1e6, 1),
           fmt_f64_p((f64)ft.queue_ns / 1e6, 0), fmt_f64_p((f64)ft.queue_max_ns / 1e6, 1), fmt_f64_p((f64)ft.tasks_ns / 1e6, 0),
           fmt_f64_p((f64)ft.tasks_max_ns / 1e6, 1));
  log_info("nc spread: second %: lane-ms finish spread % start spread % gap after the previous system % single tasks %",
           fmt_u32(s->second), fmt_f64_p((f64)ft.finish_spread_ns / 1e6, 0), fmt_f64_p((f64)ft.start_spread_ns / 1e6, 0),
           fmt_f64_p((f64)ft.gap_ns / 1e6, 0), fmt_f64_p((f64)ft.single_ns / 1e6, 0));
  EcsSystemSpread *sv = ALLOC_ARRAY_NO_ZERO(&ta.allocator, EcsSystemSpread, 256);
  u32 svn = ecs_system_spreads(s->world, sv, 256);
  StringBuilder ss = sb_create(KB(16), &ta.allocator);
  for (u32 k = 0; k < svn; k++)
    if (sv[k].finish_ns + sv[k].start_ns >= 50000000ull)
      sb_append_format(&ss, " %: % / %;", fmt_cstr(sv[k].name), fmt_f64_p((f64)sv[k].finish_ns / 1e6, 0),
                       fmt_f64_p((f64)sv[k].start_ns / 1e6, 0));
  log_info("nc spread systems: second %: lane-ms finish / start, 50 or more:%", fmt_u32(s->second), fmt_cstr(sb_get(&ss)));
  // QUIC datagrams that were no live connection's: what the server did with them this second
  NetcodeQuicServerStats q = st.quic_server, qo = o.quic_server;
  log_info("nc quic: accepted % retry % reset % version % dropped % (short initial % full % token % malformed % version % "
           "unknown long % reply full %)",
           fmt_u64(q.accepted - qo.accepted), fmt_u64(q.retries_sent - qo.retries_sent), fmt_u64(q.resets_sent - qo.resets_sent),
           fmt_u64(q.vn_sent - qo.vn_sent), fmt_u64(q.dropped - qo.dropped),
           fmt_u64(q.drops[QUIC_DROP_SHORT_INITIAL] - qo.drops[QUIC_DROP_SHORT_INITIAL]),
           fmt_u64(q.drops[QUIC_DROP_FULL] - qo.drops[QUIC_DROP_FULL]), fmt_u64(q.drops[QUIC_DROP_TOKEN] - qo.drops[QUIC_DROP_TOKEN]),
           fmt_u64(q.drops[QUIC_DROP_MALFORMED] - qo.drops[QUIC_DROP_MALFORMED]),
           fmt_u64(q.drops[QUIC_DROP_VERSION] - qo.drops[QUIC_DROP_VERSION]),
           fmt_u64(q.drops[QUIC_DROP_UNKNOWN_LONG] - qo.drops[QUIC_DROP_UNKNOWN_LONG]),
           fmt_u64(q.drops[QUIC_DROP_REPLY_FULL] - qo.drops[QUIC_DROP_REPLY_FULL]));
  const NcSide *side = ecs_resource_get(s->server_world, NcSide);
  if (side->royale.on)
    log_info("nc royale: second %: % alive, zone radius % m, % s since the drop", fmt_u32(s->second), fmt_u32(players),
             fmt_f32_p(nc_zone_radius(&side->royale, st.tick), 1), fmt_i32(net_tick_since(st.tick, side->royale.start) / (i32)NC_TICK_HZ));
  tctx_temp_allocator_end(ta);
  nc_record_report(&s->rec);
  s->stats_last = st;
  s->report_tick = st.tick;
  s->busy_count = 0;
  s->report_at = now + 1000000;
  return s->second;
}

// main lane, once a second: a window's frame times (p50, p99, max of the frame-to-frame time) and what it shows
hz_internal void nc_client_report(AppState *s, const AppMemory *memory) {
  if (s->busy_count < NC_REPORT_FRAMES) {
    s->cpu_us[s->busy_count] = s->last_frame_us;
    s->busy_us[s->busy_count++] = (u32)(memory->dt * 1e6f);
  }
  if (memory->total_time - s->report_at_s < 1.0)
    return;
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  u32 n = s->busy_count, in_view = 0, tagged = 0;
  u32 *tmp = ALLOC_ARRAY_NO_ZERO(&ta.allocator, u32, n ? n : 1);
  const u32 *b = radix_sort_u32(s->busy_us, NULL, tmp, NULL, n, (RadixSortDesc){0}).keys;
  u32 *tmp2 = ALLOC_ARRAY_NO_ZERO(&ta.allocator, u32, n ? n : 1);
  const u32 *c = radix_sort_u32(s->cpu_us, NULL, tmp2, NULL, n, (RadixSortDesc){0}).keys;
  ecs_query_foreach(ecs_query(s->world, EcsWith(NcBody), EcsWith(NetGhost)), it) in_view += (u32)it.count;
  ecs_query_foreach(ecs_query(s->world, EcsIn(NcVisual)), it) {
    const NcVisual *v = ecs_iter_get_component(&it, NcVisual, 0);
    for (i32 k = 0; k < it.count; k++)
      tagged += v[k].tinted;
  }
  log_info("nc client: % frames/s, frame ms p50 % p99 % max %, main-lane work ms p50 % p99 %, gpu ms % (0: no timing); "
           "% players in view, % bots drawn; % snapshots/s; in % KB/s, loss in % ppm out % ppm, queued % B",
           fmt_u32(n), fmt_f32_p(n ? (f32)b[n / 2] / 1000.0f : 0.0f, 2), fmt_f32_p(n ? (f32)b[(n * 99) / 100] / 1000.0f : 0.0f, 2),
           fmt_f32_p(n ? (f32)b[n - 1] / 1000.0f : 0.0f, 2), fmt_f32_p(n ? (f32)c[n / 2] / 1000.0f : 0.0f, 2),
           fmt_f32_p(n ? (f32)c[(n * 99) / 100] / 1000.0f : 0.0f, 2), fmt_f64_p(gpu_frame_gpu_ms(), 2), fmt_u32(in_view), fmt_u32(tagged),
           fmt_f32_p(s->snapshots_s, 0), fmt_u32(netcode_stats(s->net).transport.bytes_in_per_s / 1024),
           fmt_u32(netcode_stats(s->net).transport.loss_in_ppm), fmt_u32(netcode_stats(s->net).transport.loss_out_ppm),
           fmt_u32(netcode_stats(s->net).transport.send_queue_bytes));
  NetcodeClient *ncl = netcode_client(s->net);
  NetcodeFieldCounters fc = ncl ? netcode_client_field_counters(ncl) : (NetcodeFieldCounters){0};
  log_info("nc field: messages % keyframes % deltas % events % chains broken % keyframes asked % failed % mid stale % late spawns % "
           "(at most % ticks) (since the join)",
           fmt_u64(fc.messages), fmt_u64(fc.keys), fmt_u64(fc.deltas), fmt_u64(fc.events), fmt_u64(fc.breaks), fmt_u64(fc.asks),
           fmt_u64(fc.failed), fmt_u64(fc.mid_stale), fmt_u64(fc.late_spawns), fmt_u64(fc.late_ticks_max));
  NetcodeSnapshotCounters sc = ncl ? netcode_client_snapshot_counters(ncl) : (NetcodeSnapshotCounters){0};
  log_info("nc snapshots: arrived % applied %, dropped older % duplicate % queue full % before a resync % (since the join)",
           fmt_u64(sc.arrived), fmt_u64(sc.applied), fmt_u64(sc.older), fmt_u64(sc.duplicate), fmt_u64(sc.overflow), fmt_u64(sc.epoch));
  NetPredictStats ps = netcode_stats(s->net).predict;
  log_info("nc prediction: % frames, % rollbacks, % mispredicted, % new ticks, % re-simulated", fmt_u64(ps.frames),
           fmt_u64(ps.rollbacks), fmt_u64(ps.mispredicted), fmt_u64(ps.new_ticks), fmt_u64(ps.resim_ticks));
  tctx_temp_allocator_end(ta);
  s->busy_count = 0;
  s->report_at_s = memory->total_time;
}

hz_internal HzHandleT(RendererMaterial) nc_material(RendererState *r, ColorF32 c) {
  HzHandleT(RendererMaterial) m = renderer_create_material_instance(r, renderer_default_material(r));
  material_set_color4(r, m, "color", c);
  material_set_float(r, m, "smoothness", 0.4f);
  return m;
}

hz_internal void nc_render_init(AppState *s, RendererState *r) {
  PgMesh m;
  pg_mesh_begin(&m, s->arena, 256, 1024);
  pg_box(&m, (v3){1.0f, 1.0f, 1.0f}, 0.0f);
  HzHandleT(RendererSubMesh) box = pg_mesh_upload(r, &m, 1.0f).parts[0];
  s->floor = renderer_instance_batch_create(
      r, &(RendererInstanceBatchDesc){.submesh = box, .material = nc_material(r, (ColorF32){0.34f, 0.36f, 0.38f, 1.0f}),
                                      .capacity = 1});
}

hz_internal void nc_window_init(AppState *s, AppMemory *memory, u32 max_drawn) {
  EcsWorld *w = s->world;
  ecs_resource_set_ref(w, AppMemory, memory);
  ecs_input_init(w);
  ecs_assets_init(w, NULL);
  // phones (the wasm-ios build) without MSAA: 4x through Safari's WebGPU was 48 of an iPhone 13's 66 ms frame
#if defined(WASM_IOS)
  u32 msaa_default = 1;
#else
  u32 msaa_default = 4;
#endif
  u32 msaa = msaa_default;
  if (!nc_arg_u32("--msaa", msaa_default, &msaa) || (msaa != 1 && msaa != 4)) {
    log_warn("net_crowd: --msaa takes 1 or 4; using %", fmt_u32(msaa_default));
    msaa = msaa_default;
  }
  b32 shadows = !hz_arg_flag("--no-shadows");
  ecs_renderer_init(w, &(RendererInitDesc){.sample_count = msaa,
                                           .shadows_enabled = shadows,
                                           .max_batch_instances = 16,
                                           .max_bones = max_drawn * NC_BOT_BONES,
                                           .max_submesh_instances = 2 * max_drawn + 64,
                                           .max_model_instances = max_drawn + 64,
                                           .max_skinned_model_instances = max_drawn});
  ecs_transform_init(w, NULL);
  RendererState *r = ecs_resource_get_mut(w, RendererState);
  EcsEntity sun = ecs_entity_create(w);
  ecs_component_set(w, sun, LocalTransform,
                    {.rotation = quat_look(v3_normalize((v3){-0.4f, -1.0f, -0.55f}), V3_UP), .scale = V3_ONE});
  ecs_component_set(w, sun, EcsDirectionalLight,
                    {.color = {1.0f, 0.97f, 0.92f, 1.0f}, .intensity = 70000.0f, .cast_shadow = shadows,
                     .shadow_strength = 0.8f, .shadow_bias = 0.0015f, .shadow_normal_bias = 0.03f});
  EcsEntity settings = ecs_entity_create(w);
  ecs_component_set(w, settings, EcsSceneSettings,
                    {.ambient_color = {0.62f, 0.68f, 0.80f, 1.0f}, .ambient_intensity = 14000.0f,
                     .max_shadow_distance = 60.0f});
  s->camera = camera_create(&(CameraInitDesc){.pos = {0, 14, 12}, .target = V3_ZERO, .fov_degrees = 50.0f});
  nc_render_init(s, r);
  nc_bot_assets_load(&s->bot_assets, ecs_resource_get_mut(w, AssetSystem));
  // name tags: every player within 35 m, about 3,850 at 1 m2 a player, 14 glyphs and 2 elements each
  s->ui = ui_create(&(UIInitDesc){.initial_glyphs = 65536, .max_glyphs = 131072, .max_elements = 16384},
                    ecs_resource_get_mut(w, AssetSystem));
  ui_system_attach_to_renderer(s->ui, r);
}

HZ_APP_API void app_init(AppMemory *memory) {
  AppState *s = memory->state;
  if (is_main_thread()) {
    s->arena = arena_create(MB(64), MB(1));
    // a server holds per-player relevancy and field state: 128 KB a player of --capacity over the base reserve
    u32 cap = 0;
    u64 reserve = GB(4);
    if (hz_arg_flag("--server") && nc_arg_u32("--capacity", 0, &cap))
      reserve += (u64)cap * KB(128);
    s->tlsf = tlsf_allocator_create(reserve, MB(16));
    s->net_alloc = make_tlsf_allocator(s->tlsf);
    Allocator a = make_arena_allocator(s->arena);
    s->busy_us = ALLOC_ARRAY(&a, u32, NC_REPORT_FRAMES);
    s->phase_us = ALLOC_ARRAY(&a, u32, NC_PHASES * NC_REPORT_FRAMES);
    s->held_hist = ALLOC_ARRAY(&a, u32, HZ_MAX_LANES * NC_HELD_BUCKETS);
    s->cpu_us = ALLOC_ARRAY(&a, u32, NC_REPORT_FRAMES);
    s->draws = hz_has_gpu();
    if (hz_arg_value("--crowd-replay")) {
      s->role = NC_REPLAY;
      // every skinned body's pose lives in the renderer's and animation's per-entity pools: 50,000 need GBs reserved
      s->world = ecs_world_create_ex(&(EcsWorldInitDesc){.bridge_alloc_reserved = GB(8), .cmd_buffer_arena_reserved = NC_CMD_RESERVED});
      if (!nc_replay_open(&s->replay, &s->net_alloc, hz_arg_value("--crowd-replay"))) {
        hz_request_quit();
      } else if (!s->draws && hz_arg_value("--oracle")) {
        s->oracle_on = nc_oracle_open(&s->oracle, &s->replay, &s->net_alloc, hz_arg_value("--oracle"));
        if (!s->oracle_on)
          hz_request_quit();
      } else if (s->draws) {
        if (hz_arg_value("--follow") && !nc_follow_open(&s->follow, &s->net_alloc, hz_arg_value("--follow")))
          hz_request_quit();
        u32 rise = 0;
        nc_arg_u32("--shot-rise", 0, &rise);
        if (!nc_arg_u32("--capture-every", 4, &s->capture_every) || !s->capture_every)
          s->capture_every = 4;
        s->shot_rise_s = (f32)rise;
        u32 bodies = s->replay.h.capacity * s->replay.copies * s->replay.copies;
        // as the server's ghost ids: a quarter more for bodies that left, still held until the GPU lets go of them
        nc_window_init(s, memory, bodies + bodies / 4 + NC_BOTS_DRAWN_MAX);
      }
    } else
#ifdef NET_HAS_UDP
    if (hz_arg_value("--bots")) {
      s->role = NC_BOTS;
      if (!nc_bots_setup(&s->bots, &s->net_alloc))
        hz_request_quit();
    } else
#endif
    {
      // a window draws every player it holds: --capacity sizes the renderer's skinned pools to the server's
      u32 drawn_cap = NC_BOTS_DRAWN_MAX;
      nc_arg_u32("--capacity", 0, &drawn_cap);
      drawn_cap = MAX(drawn_cap + drawn_cap / 4 + 64, NC_BOTS_DRAWN_MAX);
      s->world = s->draws && drawn_cap > NC_BOTS_DRAWN_MAX ? ecs_world_create_ex(&(EcsWorldInitDesc){.bridge_alloc_reserved = GB(8),
                                                                                                .cmd_buffer_arena_reserved = NC_CMD_RESERVED})
                                                          : ecs_world_create();
      b32 ok = netcode_args_parse(hz_arg_count(), hz_argv(), &s->args, &a);
      s->role = s->args.role == NETCODE_CLIENT ? NC_JOIN : s->args.role == NETCODE_SERVER ? NC_SERVER : NC_HOST;
      s->server_world = s->role == NC_SERVER ? s->world : s->role == NC_JOIN ? NULL : ecs_world_create();
      if (s->draws)
        nc_window_init(s, memory, drawn_cap);
      if (hz_arg_value("--fidelity-log") && s->role != NC_SERVER && !nc_fidelity_open(&s->flog, &s->net_alloc, hz_arg_value("--fidelity-log")))
        hz_request_quit();
      if (!nc_arg_u32("--capture-every", 4, &s->capture_every) || !s->capture_every)
        s->capture_every = 4;
      nc_arg_u32("--capture-seconds", 0, &s->capture_s);
      nc_arg_u32("--frame-ms", 0, &s->frame_ms);
      nc_arg_u32("--hitch-ms", 0, &s->hitch_ms);
      nc_arg_u32("--hitch-every", 30, &s->hitch_every);
      if (!ok || !nc_net_init(s)) {
        log_error("net_crowd: netcode did not start");
        hz_request_quit();
      }
    }
  }
  lane_sync();
#ifdef NET_HAS_UDP
  if (s->role == NC_BOTS && s->bots.lg) {
    nc_bots_late(&s->bots, s->bots.players);
    netcode_loadgen_open(s->bots.lg, s->bots.now);
  }
#endif
  lane_sync();
}

// main lane: this client's walk into its avatar's input (netcode sends it)
hz_internal void nc_sample(AppState *s, const InputSystem *in, const AppMemory *memory) {
  v2 move = {(f32)in->buttons[KEY_D].is_pressed - (f32)in->buttons[KEY_A].is_pressed,
             (f32)in->buttons[KEY_S].is_pressed - (f32)in->buttons[KEY_W].is_pressed};
  if (s->spectator) {
    // it follows a player (the nearest when it lost one; F picks again) until WASD moves the camera at 16 m/s
    s->free_cam = s->free_cam || move.x != 0.0f || move.y != 0.0f;
    if (in->buttons[KEY_F].pressed_this_frame) {
      s->free_cam = false;
      s->followed = (EcsEntity){0};
    }
    if (!s->free_cam && !(ecs_entity_is_alive(s->world, s->followed) && ecs_component_get(s->world, s->followed, NcBody))) {
      f32 best = 1e30f;
      ecs_query_foreach(ecs_query(s->world, EcsIn(NcBody), EcsWith(NetGhost)), it) {
        const NcBody *b = ecs_iter_get_component(&it, NcBody, 0);
        for (i32 k = 0; k < it.count; k++) {
          v3 d = v3_sub(b[k].pos, s->spec_pos);
          if (v3_dot(d, d) < best) {
            best = v3_dot(d, d);
            s->followed = it.entities[k];
          }
        }
      }
    }
    const NcBody *fb = s->free_cam || !ecs_entity_is_alive(s->world, s->followed) ? NULL : ecs_component_get(s->world, s->followed, NcBody);
    if (fb)
      s->spec_pos = v3_lerp(s->spec_pos, (v3){fb->pos.x, 0.0f, fb->pos.z}, m_minf(memory->dt * 6.0f, 1.0f));
    s->spec_pos = v3_add(s->spec_pos, (v3){move.x * 16.0f * memory->dt, 0, move.y * 16.0f * memory->dt});
    s->spec_pos.x = m_clampf(s->spec_pos.x, -NC_PLAZA_HALF, NC_PLAZA_HALF);
    s->spec_pos.z = m_clampf(s->spec_pos.z, -NC_PLAZA_HALF, NC_PLAZA_HALF);
    if (memory->total_time - s->spec_sent_at >= 0.2) {
      netcode_rpc(s->net, NETCODE_TO_SERVER, NcSpectatorAt, &((NcSpectatorAt){.pos = s->spec_pos}));
      s->spec_sent_at = memory->total_time;
    }
    return;
  }
  ecs_query_foreach(ecs_query(s->world, EcsIn(NetGhost), EcsInOut(NcInput)), it) {
    const NetGhost *g = ecs_iter_get_component(&it, NetGhost, 0);
    NcInput *ni = ecs_iter_get_component(&it, NcInput, 1);
    for (i32 k = 0; k < it.count; k++)
      if (g[k].owner == 1)
        ni[k].move = move;
  }
}

// main lane: the floor, and the camera behind this client's own avatar
hz_internal void nc_draw(AppState *s, RendererState *r, const AppMemory *memory) {
  v3 own = V3_ZERO;
  b32 has_own = false;
  ecs_query_foreach(ecs_query(s->world, EcsIn(NcBody), EcsIn(NetGhost)), it) {
    const NcBody *b = ecs_iter_get_component(&it, NcBody, 0);
    const NetGhost *g = ecs_iter_get_component(&it, NetGhost, 1);
    for (i32 k = 0; k < it.count; k++)
      if (s->role != NC_SERVER && g[k].owner == 1) {
        own = b[k].pos;
        has_own = true;
        s->own_last = own;
      }
  }
  Transform floor = {.position = {0, -0.1f, 0}, .rotation = quat_identity(),
                     .scale = {2 * NC_PLAZA_HALF, 0.2f, 2 * NC_PLAZA_HALF}};
  renderer_instance_batch_write(r, s->floor, 0, &floor, 1);
  renderer_instance_batch_submit(r, s->floor, 1);
  if (s->spectator) {
    own = s->spec_pos;
    has_own = false;
  }
  v3 want = v3_add(own, has_own || (s->spectator && !s->free_cam) ? (v3){0, 6, 9} : (v3){0, 22, 24});
  s->camera.pos = v3_lerp(s->camera.pos, want, 0.15f);
  s->cam_look = v3_sub(s->camera.pos, v3_sub(want, v3_add(own, (v3){0, 1.0f, 0})));
  camera_look_at(&s->camera, s->cam_look);
  camera_update(&s->camera, memory->canvas_width, memory->canvas_height);
}

hz_internal void nc_ui(AppState *s, const InputSystem *in, const AppMemory *memory) {
  Allocator ta = tctx_temp_allocator(NULL);
  if (memory->total_time - s->rate_at >= 1.0) {
    NetcodeStats now = netcode_stats(s->net);
    s->snapshots_s = (f32)(now.client_link.snapshots - s->rate_last.client_link.snapshots) /
                     (f32)(memory->total_time - s->rate_at);
    s->rate_last = now;
    s->rate_at = memory->total_time;
  }
  NetcodeStats st = netcode_stats(s->net);
  u32 in_view = 0;
  ecs_query_foreach(ecs_query(s->world, EcsWith(NcBody), EcsWith(NetGhost)), it) in_view += (u32)it.count;
  const NcStats *sv = &s->map.stats;
  char *instance = ALLOC_ARRAY(&ta, char, 4 * NC_INSTANCE_WORDS + 1);
  for (u32 i = 0; i < 4 * NC_INSTANCE_WORDS; i++)
    instance[i] = (char)(sv->instance[i / 4] >> (8 * (i % 4)));
  // the overlay: every number but the last line's is the server's, from its stats ghost
  String l1 = s->role == NC_SERVER ? str_format(&ta, "server tick %   % players", fmt_u32(st.tick), fmt_u32(st.connections))
              : s->map.has_stats   ? str_format(&ta, "% players on one server   tick p50 % ms   p99 % ms", fmt_u32(sv->connected),
                                                fmt_f32_p((f32)sv->tick_p50_us / 1000.0f, 1), fmt_f32_p((f32)sv->tick_p99_us / 1000.0f, 1))
                                   : str_format(&ta, "joining");
  if (s->spectator)
    l1 = str_format(&ta, "spectating (%)   %",
                    fmt_cstr(s->spectate_why == NC_SPECTATE_OUT       ? "out"
                             : s->spectate_why == NC_SPECTATE_RUNNING ? "the match is running"
                                                                      : "the server is full of people"),
                    fmt_str(l1));
  String l2 = str_format(&ta, "% lanes   %   $% / hour", fmt_u32(sv->lanes), fmt_cstr(instance[0] ? instance : "local"),
                         fmt_f32_p((f32)sv->milli_dollars_hour / 1000.0f, 2));
  String l4 = {0};
  if (sv->royale) {
    NcRoyale z = nc_map_royale(sv);
    i32 hp = 0;
    ecs_query_foreach(ecs_query(s->world, EcsIn(NcVitals), EcsWith(NetPredicted)), it) hp = ecs_iter_get_component(&it, NcVitals, 0)[0].hp;
    i32 to_drop = -net_tick_since(st.tick, z.start);
    char *winner = ALLOC_ARRAY(&ta, char, NC_NAME_MAX);
    if (sv->winner)
      nc_name(sv->winner_seed, winner, NC_NAME_MAX);
    l4 = sv->winner      ? str_format(&ta, "battle royale: % is the last one standing", fmt_cstr(winner))
         : to_drop > 0   ? str_format(&ta, "battle royale: the drop in % s", fmt_u32((u32)to_drop / NC_TICK_HZ + 1))
                         : str_format(&ta, "battle royale: % alive   zone % m   you: %", fmt_u32(sv->connected),
                                      fmt_f32_p(nc_zone_radius(&z, st.tick), 0),
                                      fmt_str(s->spectator ? str_format(&ta, "out") : str_format(&ta, "% hp", fmt_u32((u32)MAX(hp, 0) / 1000))));
  }
  String l3 = str_format(&ta, "this client: % snapshots / s   % in view   round trip % ms", fmt_f32_p(s->snapshots_s, 0),
                         fmt_u32(in_view), fmt_f32_p((f32)st.transport.rtt_smoothed_us / 1000.0f, 0));
  ui_begin_frame(s->ui, in, memory->dt);
  nc_name_tags(s->world, &s->camera, memory, NC_TAG_ALWAYS);
  if (s->role != NC_SERVER) {
    b32 has_own = false;
    v3 own = {0};
    ecs_query_foreach(ecs_query(s->world, EcsIn(NcBody), EcsWith(NetPredicted)), it) {
      own = ecs_iter_get_component(&it, NcBody, 0)[0].pos;
      has_own = it.count > 0;
    }
    nc_map_client_paint(&s->map, has_own, own, netcode_stats(s->net).tick);
  }
  HzUITextConfig big = ui_text_config({.fontSize = 18, .textColor = {240, 242, 246, 255}, .wrapMode = UI_TEXT_WRAP_NONE});
  HzUITextConfig small = ui_text_config({.fontSize = 14, .textColor = {200, 206, 216, 255}, .wrapMode = UI_TEXT_WRAP_NONE});
  ui_element({.id = ui_id("NcStats"),
              .floating = {.attach_to = UI_ATTACH_TO_ROOT,
                           .attach_points = {.element = UI_ATTACH_POINT_LEFT_TOP, .parent = UI_ATTACH_POINT_LEFT_TOP},
                           .offset = {16.0f, 16.0f}},
              .layout = {.padding = {.left = 12, .right = 12, .top = 8, .bottom = 8},
                         .child_gap = 4,
                         .layout_direction = UI_TOP_TO_BOTTOM},
              .background_color = {14, 16, 22, 190},
              .corner_radius = ui_corner_radius(6)}) {
    ui_text(((HzUIString){.chars = l1.value, .length = (i32)l1.len}), big);
    if (s->role != NC_SERVER) {
      ui_text(((HzUIString){.chars = l2.value, .length = (i32)l2.len}), small);
      ui_text(((HzUIString){.chars = l3.value, .length = (i32)l3.len}), small);
      if (l4.len)
        ui_text(((HzUIString){.chars = l4.value, .length = (i32)l4.len}), big);
    }
  }
  if (s->role != NC_SERVER && s->map.want && handle_is_valid(s->map.tex)) {
    String cap = str_format(&ta, "server map   % players", fmt_u32(s->map.shown_count));
    ui_element({.id = ui_id("NcMap"),
                .floating = {.attach_to = UI_ATTACH_TO_ROOT,
                             .attach_points = {.element = UI_ATTACH_POINT_RIGHT_TOP, .parent = UI_ATTACH_POINT_RIGHT_TOP},
                             .offset = {-16.0f, 16.0f}},
                .layout = {.padding = {.left = 8, .right = 8, .top = 8, .bottom = 8},
                           .child_gap = 6,
                           .layout_direction = UI_TOP_TO_BOTTOM},
                .background_color = {14, 16, 22, 190},
                .corner_radius = ui_corner_radius(6)}) {
      ui_text(((HzUIString){.chars = cap.value, .length = (i32)cap.len}), small);
      ui_element({.id = ui_id("NcMapImage"),
                  .layout = {.sizing = {.width = ui_sizing_fixed(320), .height = ui_sizing_fixed(320)}},
                  .image = {.texture = s->map.tex}}) {}
    }
  }
  if (s->role != NC_SERVER && !s->field_whole) {
    f32 filled = 0.0f;
    if (netcode_client(s->net))
      netcode_client_field_state(netcode_client(s->net), &filled);
    String msg = str_format(&ta, "loading the plaza   %%", fmt_u32((u32)(filled * 100.0f)), fmt_char('%'));
    ui_element({.id = ui_id("NcLoading"),
                .floating = {.attach_to = UI_ATTACH_TO_ROOT, .z_index = 10},
                .layout = {.sizing = {.width = ui_sizing_fixed((f32)s->ui->canvas_width),
                                      .height = ui_sizing_fixed((f32)s->ui->canvas_height)},
                           .child_alignment = {.x = UI_ALIGN_X_CENTER, .y = UI_ALIGN_Y_CENTER}},
                .background_color = {14, 16, 22, 255}}) {
      ui_text(((HzUIString){.chars = msg.value, .length = (i32)msg.len}), big);
    }
  }
  ui_finish_layout(s->ui);
}

// main lane: a server, bots and a minimized window sleep to the next tick on a schedule that does not drift
hz_internal void nc_pace(AppState *s) {
  if (s->draws && !hz_is_minimized())
    return;
  u64 now = (u64)os_ticks_to_us(os_time_now());
  s->next_frame = s->next_frame && now < s->next_frame + 4 * NC_TICK_US ? s->next_frame + NC_TICK_US : now + NC_TICK_US;
  if (now < s->next_frame)
    os_sleep(s->next_frame - now);
}

// main lane: frames a second, frame times and what the replay draws
hz_internal void nc_replay_report(AppState *s, const AppMemory *memory) {
  if (s->busy_count < NC_REPORT_FRAMES) {
    s->cpu_us[s->busy_count] = s->last_frame_us;
    s->busy_us[s->busy_count++] = (u32)(memory->dt * 1e6f);
  }
  if (memory->total_time - s->report_at_s < 1.0)
    return;
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  u32 n = s->busy_count, drawn = 0, dressed = 0;
  const u32 *b = radix_sort_u32(s->busy_us, NULL, ALLOC_ARRAY_NO_ZERO(&ta.allocator, u32, n + 1), NULL, n, (RadixSortDesc){0}).keys;
  const u32 *c = radix_sort_u32(s->cpu_us, NULL, ALLOC_ARRAY_NO_ZERO(&ta.allocator, u32, n + 1), NULL, n, (RadixSortDesc){0}).keys;
  ecs_query_foreach(ecs_query(s->world, EcsIn(NcVisual)), it) {
    const NcVisual *v = ecs_iter_get_component(&it, NcVisual, 0);
    for (i32 k = 0; k < it.count; k++)
      dressed += v[k].tinted;
    drawn += (u32)it.count;
  }
  log_info("nc replay: % frames/s, frame ms p50 % max %, main-lane work ms p50 % max %, gpu ms %; % bodies (% x % copies), % "
           "skinned drawn, % dressed; recording at % s; captured % missed %",
           fmt_u32(n), fmt_f32_p(n ? (f32)b[n / 2] / 1000.0f : 0.0f, 2), fmt_f32_p(n ? (f32)b[n - 1] / 1000.0f : 0.0f, 2),
           fmt_f32_p(n ? (f32)c[n / 2] / 1000.0f : 0.0f, 2), fmt_f32_p(n ? (f32)c[n - 1] / 1000.0f : 0.0f, 2),
           fmt_f64_p(gpu_frame_gpu_ms(), 2), fmt_u32(s->replay.bodies), fmt_u32(s->replay.copies), fmt_u32(s->replay.copies),
           fmt_u32(drawn), fmt_u32(dressed), fmt_f32_p(s->replay_t, 1), fmt_u32(s->shots), fmt_u32(s->shots_missed));
  tctx_temp_allocator_end(ta);
  s->busy_count = 0;
  s->report_at_s = memory->total_time;
}

// main lane: the wide shot's camera, from eye level in the crowd up over the whole plaza in --shot-rise seconds
hz_internal void nc_replay_camera(AppState *s, const AppMemory *memory) {
  f32 span = (2.0f * s->replay.h.map_half + 4.0f) * (f32)s->replay.copies;
  f32 k = s->shot_rise_s > 0.0f ? m_clampf(s->replay_t / s->shot_rise_s, 0.0f, 1.0f) : 1.0f;
  k = k * k * (3.0f - 2.0f * k);
  f32 h = 1.7f + (span * 0.9f - 1.7f) * k;
  v3 target = {0.0f, 1.0f, 0.0f};
  s->camera.pos = (v3){0.0f, h, 6.0f + span * 0.55f * k};
  camera_look_at(&s->camera, target);
  camera_update(&s->camera, memory->canvas_width, memory->canvas_height);
  Transform floor = {.position = {0, -0.1f, 0}, .rotation = quat_identity(), .scale = {span, 0.2f, span}};
  RendererState *r = ecs_resource_get_mut(s->world, RendererState);
  renderer_instance_batch_write(r, s->floor, 0, &floor, 1);
  renderer_instance_batch_submit(r, s->floor, 1);
}

// a replay's frame: the recording at this frame's time, its bodies posed and drawn, optionally captured to --frames-out
hz_internal void nc_replay_update(AppState *s, AppMemory *memory) {
  EcsWorld *w = s->world;
  InputSystem *in = ecs_resource_get_mut(w, InputSystem);
  ui_font_update(s->ui);
  const char *dir = hz_arg_value("--frames-out");
  if (is_main_thread()) {
    s->frame_at = (u64)os_ticks_to_us(os_time_now());
    input_update(in, &memory->input_events, memory->total_time);
    // the clock starts once the first bodies are dressed
    if (!s->replay_live && nc_bot_assets_ready(&s->bot_assets, ecs_resource_get_mut(w, AssetSystem))) {
      u32 bodies = 0, dressed = 0;
      ecs_query_foreach(ecs_query(w, EcsIn(NcVisual)), it) {
        const NcVisual *v = ecs_iter_get_component(&it, NcVisual, 0);
        for (i32 k = 0; k < it.count; k++)
          dressed += v[k].tinted;
        bodies += (u32)it.count;
      }
      s->replay_live = bodies && bodies == s->replay.bodies && dressed == bodies;
    }
    if (s->follow.log.path) {
      // following a live client's log: each of its captured frames at its shown tick until drawn and captured
      s->frame_dt = 1.0f / 60.0f;
      if (!s->follow.have && !s->follow.ended) {
        s->follow.have = nc_follow_next(&s->follow);
        s->follow.ended = !s->follow.have;
      }
      if (s->follow.have)
        s->replay_t = (f32)(((f64)net_tick_since(s->follow.fr.tick, s->replay.first_tick) + (f64)s->follow.fr.fraction) *
                            (f64)s->replay.h.tick_us / 1e6);
      nc_replay_frame(&s->replay, w, m_maxf(s->replay_t, 0.0f));
    } else {
      // capturing, each app frame is 1/K of an output frame: the capture slot is free again every K frames
      s->frame_dt = dir ? memory->dt / (f32)s->capture_every : memory->dt;
      if (s->replay_live)
        s->replay_t += s->frame_dt;
      nc_replay_frame(&s->replay, w, s->replay_t);
    }
  }
  lane_sync();
  ecs_assets_schedule(w);
  ecs_flush(w, s->frame_dt);
  if (is_main_thread()) {
    nc_visuals(w, &s->bot_assets, NULL, s->frame_dt);
    ecs_transform_schedule(w);
    ecs_renderer_schedule(w);
  }
  lane_sync();
  ecs_flush(w, s->frame_dt);
  if (is_main_thread()) {
    nc_replay_camera(s, memory);
    if (s->follow.have) {
      s->camera.pos = s->follow.fr.eye;
      camera_look_at(&s->camera, s->follow.fr.look);
      camera_update(&s->camera, memory->canvas_width, memory->canvas_height);
    }
    ui_begin_frame(s->ui, in, memory->dt);
    nc_name_tags(w, &s->camera, memory, 0);
    ui_finish_layout(s->ui);
    nc_replay_report(s, memory);
  }
  lane_sync();
  renderer_update(ecs_resource_get_mut(w, RendererState),
                  &(RendererBeginFrameDesc){.camera = &s->camera, .clear_color = (ColorF32){0.55f, 0.66f, 0.80f, 1.0f},
                                            .time = memory->total_time});
  if (is_main_thread()) {
    if (s->follow.log.path) {
      // captured once every body of this tick is dressed; the slot busy, the same frame is drawn again
      u32 bodies = 0, dressed = 0;
      ecs_query_foreach(ecs_query(w, EcsIn(NcVisual)), it) {
        const NcVisual *v = ecs_iter_get_component(&it, NcVisual, 0);
        for (i32 k = 0; k < it.count; k++)
          dressed += v[k].tinted;
        bodies += (u32)it.count;
      }
      if (dir && s->follow.have && bodies == s->replay.bodies && dressed == bodies) {
        TempAllocator ta = tctx_temp_allocator_begin(NULL);
        String path = str_format(&ta.allocator, "%/frame_%.png", fmt_cstr(dir), fmt_u32_pad(s->follow.fr.shot - 1, 5));
        if (hz_screenshot_request(path.value)) {
          s->shots++;
          s->follow.have = false;
        }
        tctx_temp_allocator_end(ta);
      }
      if (s->follow.ended) {
        log_info("nc follow: the log ended; % frames captured", fmt_u32(s->shots));
        hz_request_quit();
      }
    } else if (dir && s->replay_live && s->live_frames++ % s->capture_every == 0) {
      TempAllocator ta = tctx_temp_allocator_begin(NULL);
      String path = str_format(&ta.allocator, "%/frame_%.png", fmt_cstr(dir), fmt_u32_pad(s->shots + s->shots_missed, 5));
      if (hz_screenshot_request(path.value))
        s->shots++;
      else
        s->shots_missed++;
      tctx_temp_allocator_end(ta);
    }
    if (s->replay.ended) {
      log_info("nc replay: the recording ended at % s; % frames captured, % missed", fmt_f32_p(s->replay_t, 1), fmt_u32(s->shots),
               fmt_u32(s->shots_missed));
      hz_request_quit();
    }
    s->last_frame_us = (u32)((u64)os_ticks_to_us(os_time_now()) - s->frame_at);
  }
  lane_sync();
}

HZ_APP_API void app_update_and_render(AppMemory *memory) {
  AppState *s = memory->state;
  if (s->role == NC_REPLAY) {
    if (s->draws && s->ui) {
      nc_replay_update(s, memory);
    } else if (!s->draws && s->oracle_on && is_main_thread()) {
      nc_oracle_step(&s->oracle, &s->replay);
      if (s->oracle.done)
        hz_request_quit();
    } else if (!s->draws && s->replay.a && is_main_thread()) {
      nc_replay_verify(&s->replay);
      if (s->replay.verified)
        hz_request_quit();
    }
    return;
  }
#ifdef NET_HAS_UDP
  if (s->role == NC_BOTS) {
    if (s->bots.lg)
      nc_bots_frame(&s->bots);
    return;
  }
#endif
  if (!s->net)
    return;
  EcsWorld *w = s->world;
  InputSystem *in = s->draws ? ecs_resource_get_mut(w, InputSystem) : NULL;
  if (s->draws)
    ui_font_update(s->ui);
  if (is_main_thread()) {
    s->frame_at = (u64)os_ticks_to_us(os_time_now());
    if (in)
      input_update(in, &memory->input_events, memory->total_time);
    ecs_netcode_schedule_receive(w);
  }
  lane_sync();
  if (s->draws)
    ecs_assets_schedule(w);
  ecs_flush(w, memory->dt);
  if (is_main_thread() && in && s->role != NC_SERVER)
    nc_sample(s, in, memory);
  lane_sync();
  NetTick before = is_main_thread() ? netcode_stats(s->net).tick : 0;
  u64 received_at = is_main_thread() ? (u64)os_ticks_to_us(os_time_now()) : 0;
  ecs_netcode_simulate(w);
  u64 simulated_at = is_main_thread() ? (u64)os_ticks_to_us(os_time_now()) : 0;
  if (is_main_thread()) {
    s->report_due = s->role == NC_SERVER && (u64)os_ticks_to_us(os_time_now()) >= s->report_at;
    if (s->draws)
      nc_visuals(w, &s->bot_assets, netcode_client(s->net), memory->dt);
    ecs_netcode_schedule_send(w);
    if (s->draws) {
      ecs_transform_schedule(w);
      ecs_renderer_schedule(w);
    }
  }
  lane_sync();
  ecs_flush(w, memory->dt);
  if (s->report_due) {
    nc_views_share(s);
    lane_sync();
  }
  if (is_main_thread() && s->role == NC_JOIN && (s->frame_ms || s->hitch_ms)) {
    u32 ms = s->hitch_ms && s->hitch_every && ++s->hitch_n % s->hitch_every == 0 ? s->hitch_ms : s->frame_ms;
    while ((u64)os_ticks_to_us(os_time_now()) - s->frame_at < (u64)ms * 1000u) {
    }
  }
  if (is_main_thread()) {
    u64 now = (u64)os_ticks_to_us(os_time_now());
    if (s->role == NC_SERVER && netcode_stats(s->net).tick != before && s->busy_count < NC_REPORT_FRAMES) {
      s->phase_us[s->busy_count] = (u32)(received_at - s->frame_at);
      s->phase_us[NC_REPORT_FRAMES + s->busy_count] = (u32)(simulated_at - received_at);
      s->phase_us[2 * NC_REPORT_FRAMES + s->busy_count] = (u32)(now - simulated_at);
      s->busy_us[s->busy_count++] = (u32)(now - s->frame_at);
    }
    nc_announce(s);
    if (s->server_world) {
      NcSide *side = ecs_resource_get_mut(s->server_world, NcSide);
      for (u32 k = 0; k < side->spectators_new_count; k++) {
        u32 conn = side->spectators_new[k], why = side->spectators_why[k];
        netcode_rpc(s->net, conn, NcSpectate,
                    &((NcSpectate){.humans = side->humans, .cap = side->human_cap, .why = why, .place = side->spectators_place[k]}));
        s->map_server.asked[conn] = side->spectators_human[k];
        if (why == NC_SPECTATE_OUT)
          log_info("nc royale: connection % out in place %", fmt_u32(conn), fmt_u32(side->spectators_place[k]));
        else
          log_info("nc spectator: connection % joined %", fmt_u32(conn),
                   fmt_cstr(why == NC_SPECTATE_RUNNING ? "after the drop" : "past the human cap"));
      }
      side->spectators_new_count = 0;
      if (side->winner && !s->winner_logged) {
        TempAllocator ta = tctx_temp_allocator_begin(NULL);
        char *name = ALLOC_ARRAY(&ta.allocator, char, NC_NAME_MAX);
        nc_name(side->winner_seed, name, NC_NAME_MAX);
        log_info("nc royale: % is the last one standing, at tick %", fmt_cstr(name), fmt_u32(netcode_stats(s->net).tick));
        tctx_temp_allocator_end(ta);
        s->winner_logged = true;
      }
    }
    if (s->draws && s->role != NC_SERVER) {
      ecs_query_foreach(ecs_query(w, EcsIn(NcSpectate), EcsWith(NetRpcReceived)), it) {
        const NcSpectate *sp = ecs_iter_get_component(&it, NcSpectate, 0);
        if (!s->spectator && sp->why == NC_SPECTATE_OUT)
          log_info("nc: out of the battle royale in place %; spectating", fmt_u32(sp->place));
        else if (!s->spectator)
          log_info("nc: spectating, %", fmt_cstr(sp->why == NC_SPECTATE_RUNNING ? "the match is running"
                                                                                  : "the server's human cap is full"));
        if (!s->spectator)
          s->spec_pos = s->own_last;
        s->spectate_why = sp->why;
        s->spectator = true;
        s->map.want = true;
        s->map.asked = true;
      }
      if (in && in->buttons[KEY_M].pressed_this_frame)
        s->map.want = !s->map.want;
      nc_map_client_frame(&s->map, w, s->net, &s->net_alloc);
    }
    if (s->role == NC_SERVER) {
      NetTick tick = netcode_stats(s->net).tick;
      u32 second = nc_server_report(s, now);
      if (second) {
        s->map_sent = tick;
      } else if (s->map_hz > 1 && net_tick_since(tick, s->map_sent) >= (i32)(NC_TICK_HZ / s->map_hz)) {
        // the map between seconds for spectators (--map-hz)
        u64 hash = 0;
        u32 n = nc_map_server_gather(&s->map_server, s->server_world, &hash);
        nc_map_server_send(&s->map_server, s->net, ++s->map_server.seq, n);
        s->map_sent = tick;
      }
      nc_record_frame(&s->rec, s->server_world, tick, second);
    }
    if (s->draws) {
      nc_draw(s, ecs_resource_get_mut(w, RendererState), memory);
      nc_ui(s, in, memory);
      if (s->role != NC_SERVER)
        nc_client_report(s, memory);
    }
    // a join loads every player's field state before its first frame is shown, logged or captured
    if (s->role != NC_SERVER && !s->field_whole && netcode_client(s->net)) {
      NetcodeFieldState fs = netcode_client_field_state(netcode_client(s->net), NULL);
      if (fs != NETCODE_FIELD_NONE && s->join_at == 0.0)
        s->join_at = memory->total_time;
      if (fs == NETCODE_FIELD_WHOLE) {
        s->field_whole = true;
        log_info("nc: the field is whole % s after its first message: the join's loading phase ends",
                 fmt_f64_p(memory->total_time - s->join_at, 2));
      }
    }
    if (s->role != NC_SERVER && s->field_whole && (s->flog.file || hz_arg_value("--frames-out"))) {
      // --frames-out: every K-th frame captured, the log naming the frame its capture is of
      u32 shot = 0;
      const char *dir = hz_arg_value("--frames-out");
      if (s->draws && dir && s->capture_from == 0.0)
        s->capture_from = memory->total_time;
      b32 window = !s->capture_s || memory->total_time - s->capture_from < (f64)s->capture_s;
      if (s->draws && dir && window && s->live_frames++ % s->capture_every == 0) {
        TempAllocator ta = tctx_temp_allocator_begin(NULL);
        String path = str_format(&ta.allocator, "%/frame_%.png", fmt_cstr(dir), fmt_u32_pad(s->shots, 5));
        if (hz_screenshot_request(path.value))
          shot = ++s->shots;
        else
          s->shots_missed++;
        tctx_temp_allocator_end(ta);
      }
      NetcodeStats st = netcode_stats(s->net);
      nc_fidelity_frame(&s->flog, w, netcode_client(s->net), &st, s->camera.pos, s->cam_look, shot);
    }
  }
  lane_sync();
  if (s->draws)
    renderer_update(ecs_resource_get_mut(w, RendererState),
                    &(RendererBeginFrameDesc){.camera = &s->camera, .clear_color = (ColorF32){0.55f, 0.66f, 0.80f, 1.0f},
                                              .time = memory->total_time});
  if (is_main_thread()) {
    s->last_frame_us = (u32)((u64)os_ticks_to_us(os_time_now()) - s->frame_at);
    nc_pace(s);
  }
  lane_sync();
}

HZ_APP_API size_t app_state_size(void) { return sizeof(AppState); }

#endif
