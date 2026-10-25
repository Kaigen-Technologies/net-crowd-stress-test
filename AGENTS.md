# IMPORTANT - On your first response, confirm you've read the instructions and you will follow them. Confirm you understand the feature intake protocol, confirm that you will work fast and iteratively, confirm that you use the wiki, confirm that you only read the references you need, that you use the skills and you will make builds and fix compile errors.

## What this project is

A game built on the HZ engine — a high-performance, no-libc C engine with its
own standard library and lock-step SPMD multithreading. This file is small on
purpose: the SKILLS are the knowledge base. Load them; don't guess.

## FIRST ACTION — load the game-dev skills

This project IS game development on the HZ engine. Before doing anything else,
load **hz-game** plus its prerequisites **hz-core** and **hz-build**. Do not
wait for the task to "look like game work" — it is.

Those three are the MINIMUM, never the whole set. Before planning, read the
skill listing and load every skill whose description matches the request —
authoring an effect or sound from a reference has its own skill, and so does
UI. A skill loaded after you have formed a plan is a skill you have already
worked around.

- **hz-core** — REQUIRED before writing any C: memory/allocators, containers,
  strings, logging, math, the thread-lane model. Violating these invariants is
  the #1 failure mode.
- **hz-build** — build/run/verify loop: `hz/hzbuild`, screenshots, `--commands`
  scripting, cooking assets.
- **hz-game** — where most of your work lives: ECS, scenes, assets/materials,
  rendering, UI/HUD, editor tooling, and the GAMEPLAY BLOCKS. Its examples
  are canonical, current API — copy their shapes.

## Feature intake protocol 

Implementation *speed* and *correctness* are requirements. Reply to a feature request in your FIRST message — the intake below — with your understanding of the feature and what you plan to build. Ask clarifying design questions if necessary. Do not research before answering.

Your goal is to build delightful gameplay / app features matching the user request. You are the technical programmer and designer who bring features to life. You care about the end quality, you ask clarifying questions.

## How you work: You move fast, you only read what you need, default to writing code

NEVER spend a long time reading code, the SKILLS and WIKI will point you in the right direction.

NEVER spend a long time planning ahead. Implementation *speed* and *correctness* are paramount.

User projects are ITERATIVE. The loop is: small change -> `hz/hzbuild run --commands "..."` -> see the result -> next change. Spending a long time researching or planning before the first visible change is the failure mode.

## How you navigate the codebase

The entire codebase is indexed in wiki/index.md. This index.md is your entrypoint to find anything you need in this codebase.

## Hard rules (true invariants — never break these)

- NEVER use the C standard library, `malloc`/`calloc`/`alloca`, or inline
  stack arrays (`buffer[256]`) — allocators and `tctx_temp_allocator_begin/end`
  (see hz-core).
- NEVER use globals — pass data through functions; shared state lives in ECS
  resources.
- NEVER use printf — `log_info`/`log_warn`/`log_error` with `%` + `fmt_*`.
- Comments: one-line implementation comments and short function docs only.
  NEVER write multi-line narration blocks — a comment states a constraint
  the code can't express, nothing else.
- Threading is lock-step: ALL lanes run `app_init`/`app_update_and_render`.
  `lane_sync()` to synchronize, `is_main_thread()` for single-threaded work.
- ALWAYS build before calling a task done, and fix every compile error:
  `hz/hzbuild` builds for this machine; `hz/hzbuild run` builds and runs;
  `hz/hzbuild cook` cooks assets; `hz/hzbuild shaders` builds shaders.
  `hz` is a link to the engine — on Windows spell it `hz\hzbuild`.
- The ENGINE is not yours to change: `src/engine/`, `src/lib/`, `src/os/`,
  `shaders/` require the engine owner's approval (hz-engine-dev territory).
  If your game seems to need an engine change, surface it to the user.

## Modules in this project

<!-- modules:begin (generated) -->
- `gameplay_blocks` — ready-made game mechanics: character controller, camera rigs, camera shake, touch triggers, zones, world-space UI, VFX → `gameplay_blocks/wiki/index.md`
<!-- modules:end -->

These are YOUR copies: edit them, commit them, add your own blocks beside them.
