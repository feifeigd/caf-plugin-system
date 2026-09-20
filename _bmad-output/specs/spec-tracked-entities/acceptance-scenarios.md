# Acceptance Scenarios

These are planned acceptance checks, not executed tests.

| ID | Capability | Scenario and required evidence |
| --- | --- | --- |
| A01 | CAP-1 | Full and projected hydration emit no requests; same-value assignment stays clean. |
| A02 | CAP-1 | Scalar assignment and += export final values without manual dirty flags. |
| A03 | CAP-1 | Nested object mutation, map insert/erase, vector append/remove/reorder export the correct complete embedded field. |
| A04 | CAP-1 | Mutable-reference escape is prevented; handles remain safe after vector relocation and fail after removal. |
| A05 | CAP-2 | New entity exports strict insert; duplicate key conflicts without overwriting existing data. |
| A06 | CAP-2 | Update of a missing entity fails without insertion; new-then-erased before submission emits nothing. |
| A07 | CAP-2 | Persisted deletion exports a fieldless delete with version; wrong version rolls back the batch. |
| A08 | CAP-3 | Owned item creation, count edit, and erase address item records; parent embedded data is not substituted. |
| A09 | CAP-4 | Currency update plus item insert commit atomically; injected second-operation failure leaves both unchanged. |
| A10 | CAP-4 | Mixed partitions/stores and duplicate conflicting identities fail before dispatch; empty work returns no request. |
| A11 | CAP-5 | Value 20 is sent, then changed to 21; acknowledgement confirms only 20 and next update sends 21. |
| A12 | CAP-5 | A -> B sent -> A local produces a subsequent A update after B succeeds. |
| A13 | CAP-5 | Insert sent then local erase waits for outcome: success yields a later delete; proven rollback cancels; unknown stays pending. |
| A14 | CAP-5 | Commit acknowledgement loss replays exactly the same request ID/payload and returns the original insert/delete result. |
| A15 | CAP-5 | Duplicate/late replies do not clear later edits; conflict preserves local data and prevents blind new writes. |
| A16 | CAP-1, CAP-2 | NULL differs from omitted insert fields; partial loads never overwrite unseen fields; database defaults remain unknown until loaded. |
| A17 | CAP-3 | Erasing a member retains its deletion tombstone; clearing a partial collection does not delete unloaded records. |
| A18 | CAP-4, CAP-5 | Repeat strict CRUD and replay/rollback scenarios on each supported adapter, using disposable databases and existing test harnesses. |
| A19 | CAP-5 | Bounded shutdown with an unresolved request reports incomplete persistence and never falsely confirms clean state. |

## Suggested implementation sequence

1. Extend strict CRUD contract, validation, result correlation, idempotency format and adapter implementations, preserving legacy callers.
2. Add tracked scalar/embedded values, identity attachment and known-field masks; verify mutation interception.
3. Add lifecycle and frozen UnitOfWork batch state; verify acknowledgement, uncertainty and conflict cases.
4. Add owned entity collections and integrated player/item examples, then backend acceptance coverage.

Resolve affected policy questions before implementing root cascades, cross-player transfer, or durable recovery. This sequence is not a generated stories.yaml or an implementation authorization.
