# Net Crowd Stress Test

A plaza full of walking players on one server, built on the Kaigen Engine, a high performance, cross-platform 3D engine written in C, to stress-test its netcode with tens of thousands of connected players. The same binary is the server, the client and the bot load generator.

[kaigen3d.com](https://kaigen3d.com/) · [Register for the closed beta.](https://kaigen3d.com/)

![Net Crowd Stress Test](preview.png)

## Requirements

The project needs the Kaigen Engine, which is in closed beta, at version **0.7.0** or newer. The netcode this demo uses ships in 0.7.0, which is not released yet.

## Run

`hz` is a link to your engine checkout at the project root (it is not part of this repository).

```bash
hz/hzbuild run                       # host: a server and a local client in one process
hz/hzbuild run -- --server           # a dedicated server
hz/hzbuild run -- --join <ticket>    # a client joining a server
```

WASD walks. The panel in the top-left shows the server's player count and tick times, and this client's snapshot rate and round trip.

`--bots` turns the binary into a load generator that connects many simulated players to a server (native platforms). Its flags are read in `src/net_crowd/nc_bots.c`.

## Layout

- `src/net_crowd/`: the demo (simulation, bots, map, rendering, recording)
- `tests/`: the project's tests (`hz/hzbuild test run`)
- `assets/net_crowd/`: the player characters
- `gameplay_blocks/`: the engine's gameplay module, vendored
