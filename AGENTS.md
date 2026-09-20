<!-- bmad:context -->
<!-- Verified 2026-09-08 against 81a307caf0a1b85d3e05f27b17588c8a8e2da51a. Managed by bmad-project-context. -->

## caf-plugin-system

C++20 CAF plugin framework. Technical documentation lives in `docs/`;
BMad planning artifacts belong in `_bmad-output/`.

## Where things are

- Follow application lifecycle wiring in `src/core/__main__.cpp`.
- For plugin lifecycle changes, inspect `src/core/plugin/plugin_manager.cpp`.
- For entity storage changes, read `docs/entity-store.md` and
  `include/common/entity_store_contract.hpp`.
- When maintaining the CAF development skill, follow
  `.agents/skills/caf-plugin-development/AGENTS.md`.

## Running and verifying

- Keep runtime configurations separate: CMake stages binaries into
  `run/Debug` and `run/Release`; do not assume binaries live directly in `run/`.
- For Windows CTest runs, specify the configuration:
  `ctest --test-dir out/build/windows-x64 -C Debug --output-on-failure`.
  There is no Windows test preset in `CMakePresets.json`.

## Conventions that differ from defaults

- Route shutdown requests through `shutdown_mgr`. Register persistent
  application actors for shutdown rather than leaving them alive during
  actor-system destruction.
- Use `include/services/logging_service.hpp` logging macros.
  The shared core library owns the logger; plugins do not need per-module
  logger injection. This supersedes older static-library notes.
- Define framework message IDs in `include/common/message_tags.def`.
  Follow its explicit pre-release versus post-release ID policy; older
  unconditional append-only guidance does not describe the current policy.
- Initialize runtime message metadata before constructing an actor system;
  compile-time type IDs alone are insufficient.

## Known pitfalls

- Load replacement plugins from a new path; loading the same DLL path can
  reuse cached code. Hot reload cannot introduce unregistered message IDs.
- Preserve proxy quiescence and snapshot barriers during replacement.
  Do not destroy a retired actor's plugin instance before its down message
  or unload DLL code while CAF references can still call it.
- Give shutdown requests bounded timeouts and error handlers so failure
  advances the shutdown chain instead of hanging it.
- Do not hold a scoped_actor in the blocking console-input thread.
  Preserve the weak actor address and upgrade it only when sending.

<!-- /bmad:context -->
