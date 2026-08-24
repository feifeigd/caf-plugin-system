# CAF 1.1 Actor Semantics (mailbox / exit / promise / down_msg)

Verified against caf-plugin-system (CAF 1.1, vcpkg, MSVC) 2026-08. These semantics drove the hot-reload/unload retire design.

## Mailbox & exit message

- `send_exit` is an **ordinary queued message** (FIFO, async). Messages enqueued BEFORE the exit message are processed first; messages after it are **dropped** at termination.
- Default exit behavior: reason != normal → actor quits immediately, remaining mailbox discarded.
- **You cannot "drain the mailbox after receiving exit"** — CAF has no `on_mailbox_empty` hook, and new messages keep arriving (no way to distinguish "temporarily empty" from "forever empty"). Correct drain is upstream flow-stopping + a barrier, not post-exit draining.

## down_msg is a termination signal, NOT a resource-release signal

- `down_msg` fires during actor finalize, delivered to every monitor. Guaranteed IF you called `monitor(actor)` and the monitor is alive. Sent exactly once → no double-destroy on unload/crash races.
- The actor **object** (state, captured lambdas, mailbox leftovers, pending-response table) destructs later, refcount-driven. Out-of-signature message destructors may still execute DLL code after down_msg — one reason DLLs are never FreeLibrary'd.
- So: `destroy(instance)` belongs in the down_msg handler (never immediately after send_exit), but it is still "best effort" — see residual race below.

## Promise lifecycle on termination

- **Outgoing requests** (actor requested someone): pending callbacks are discarded on death; late responses are dropped — harmless.
- **Incoming requests** (actor owes responses): CAF **auto-delivers `sec::actor_died`** to the caller on termination — callers never hang. This is the consistency backbone of freeze-on-snapshot: unsettled async work fails the caller, caller retries against the new instance.
- **Atomicity**: a handler's response callback executes and delivers the caller response in the SAME message handling. Therefore "state mutated" ⇔ "caller received response" — there is no observable intermediate state. Snapshot-at-drain therefore captures exactly the committed state; everything unsettled is the caller's retry problem.
- Delegated promises (proxy → impl via `promise.delegate`) belong to the original requester; the dying actor is irrelevant to them.

## Why the quiesce + save_state barrier proves mailbox drain

1. quiesce ack → every call the proxy handled before the ack was **synchronously enqueued** (delegate is synchronous) to the old actor; no new forwards after ack. "该来的都来了".
2. `request save_state` enqueues AFTER all of those; FIFO processing means the response returns only after all earlier messages were handled. "来了的都处理了". Response = drain proof, independent of handler payload (empty vector still proves it).

Caveat: FIFO is per (sender, receiver) pair — the argument relies on the architecture that the impl actor only receives calls via the proxy (registry never exposes impl handles). Direct-to-impl senders break the proof.

## Freeze-on-snapshot and its residual race

Immediate `send_exit` after the snapshot drops async responses → callbacks never run → state frozen at snapshot. **Residual race**: an async response arriving in the microsecond window between "save_state processed" and "exit enqueued" still gets processed (state mutates after snapshot). To 100% eliminate: plugin calls `self->quit()` inside its own save_state handler (self-freeze). Documented trade-off, not a bug.

## Other verified behaviors

- **Empty behavior kills the actor**: an actor whose behavior is `caf::behavior{}` (even with a default handler set) terminates immediately. Any long-lived actor needs ≥1 real handler. (Real spawn_service_proxy never hit this — it has quiesce/resume/set_acl.)
- send_exit to an already-dead/terminating actor is harmless (message dropped).
- Delayed/timer messages arriving after exit are dropped — this is the desired "freeze" behavior.
