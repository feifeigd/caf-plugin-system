# CAF 1.1 typed_actor migration — verified findings

All findings verified by **compilation + runtime** in `tests/typed_smoke/` (tests/typed_smoke_plugin.dll + typed_smoke.exe, 6/6 PASS) on CAF 1.1 / MSVC / vcpkg, 2026-08. The test doubles as the reference implementation for the migration.

## Verdict

Feasible. Proxy/registry/hot-reload layers stay untyped (transparent forwarding verified working). Estimate ~3.5–5 days: service-contract typed aliases + lifecycle signature template (0.5d), 3 plugins + v2 (1–1.5d), main.cpp caller adaptation (0.5d), registry/PM small changes (0.5d), default-handler private-protocol handling (0.5–1d), full hot-reload/unload/shutdown regression (0.5–1d).

## The 5 hard constraints (each cost a compile error)

### 1. spawn derives handle type from the factory signature
```cpp
// WRONG (C2661 in actor_storage.hpp):
sys.spawn<smoke_service::impl>(smoke_impl);
// RIGHT (infer_handle_from_fun_t):
auto typed = sys.spawn(smoke_impl);   // factory: behavior_type(smoke_service::pointer)
```

### 2. typed ↔ untyped requires explicit actor_cast
```cpp
caf::actor raw = caf::actor_cast<caf::actor>(typed);          // typed → untyped (DLL export)
auto svc = caf::actor_cast<smoke_service>(raw);               // untyped → typed (caller)
```

### 3. typed actor cannot self->send() to an untyped actor
`send_type_check.hpp` static_assert: "statically typed actors can only send() to other statically typed actors; use anon_send() or request()".
```cpp
// WRONG inside a typed handler:
self->send(coordinator, drain_atom{}, self->address());   // coordinator is caf::actor
// RIGHT:
caf::anon_send(coordinator, drain_atom{}, self->address());
```
Impact: every existing plugin handler that does `self->send(plugin_mgr, ...)` (untyped kernel actors) must become anon_send/request.

### 4. typed request compile-time rejects out-of-signature messages
`requester.hpp` static_assert "receiver does not accept given message" + cascade (C2794/C2938/C3203/C2825). A caller CANNOT `request(typed_actor, ..., private_atom{})` — private protocols (plugin_envelope, plugin-private atoms) are impossible as typed requests.
Working paths (verified):
- request via the **untyped proxy** — proxy doesn't check signatures, forwards via delegate; the typed impl's `set_default_handler` catches the out-of-signature message and its `caf::make_message(...)` response routes back correctly through the delegate.
- `anon_send` for fire-and-forget.

### 5. `caf::sec::unhandled_message` does not exist in CAF 1.1
Use `caf::sec::invalid_request` (sec.hpp has invalid_request, actor_died, ...).

## Design rules for the migration

- **Lifecycle messages MUST be in the typed signature** (init/drain/save_state/restore_state/shutdown). save_state is a request from PluginManager — out-of-signature = caller-side compile error AND PM's request would fail at runtime. Plan a shared `plugin_lifecycle` typed alias (the 5 lifecycle signatures) and splice it into every service contract.
- Default handler is the designated home for plugin-private messages; accept the "typed purity loss" there.
- `extern "C"` factory returning `caf::actor` triggers MSVC C4190 (harmless) — suppress once in the shared contract header: `#pragma warning(disable : 4190)`.
- The untyped proxy (`spawn_service_proxy`) needs NO changes: delegate forwarding works for typed targets; responses route to original requesters.

## typed_smoke test structure (reusable skeleton)

- `typed_smoke_common.hpp`: contract header — `smoke_service = caf::typed_actor<lifecycle..., caf::result<std::string>(std::string)>` + `extern "C" TYPED_SMOKE_API caf::actor create_smoke_service(caf::actor_system&)`.
- `typed_smoke_plugin.cpp`: DLL side — typed impl (lifecycle handlers + echo + default handler for health_check_atom), exports factory returning actor_cast'd untyped handle.
- `typed_smoke_main.cpp`: exe side — `caf::core::init_global_meta_objects()` + `app_meta::init()` (project pattern, see main.cpp), cast to typed, direct request, via-untyped-proxy request, save_state request, out-of-signature via proxy, shutdown.
- CMake: SHARED lib + exe, both link `caf_plugin_core`; `add_test(NAME typed_smoke ...)`.
