# Interface and State Design

Proposed interfaces, not existing APIs. Derived from the decision log; policy defaults remain assumptions in SPEC.md. Current-source baseline: 81a307caf0a1b85d3e05f27b17588c8a8e2da51a.

## Ownership and mapping

- A UnitOfWork owns an identity map keyed by store, entity name, and canonical primary key. Reject duplicate attachment with conflicting state. Routing partition is immutable while attached.
- Entities describe key fields, writable fields, version metadata, embedded codecs, and owned relationships once. Loaded records are attached as clean with their known-field mask and database version.
- Tracked values and collections report mutations to stable owner tokens, not addresses of movable vector elements. Detached/deleted handles fail deterministically.
- Do not expose mutable raw references, pointers, or iterators that bypass tracking. Nested access yields tracked handles; read access is const. Container algorithms must use supported mutating operations.
- Owned item membership sets its owner foreign key. Removing a persisted owned member retains a deletion tombstone in the UnitOfWork after the collection entry disappears.
- Collections may be partially loaded. clear/erase operate on known members only; bulk deletion of unloaded children is not inferred. Loading a collection initially accepts records supplied by explicit load/query code; automatic relationship querying is not included.

## Proposed usage

```cpp
UnitOfWork unit{store, player_partition};
auto player = unit.attach<Player>(player_load_result);
unit.attach_owned(player.items, item_load_results);

player.gold -= 100;
auto item = player.items.emplace(item_id);
item.template_id = template_id;
item.count = 1;
player.position.x = 100;

PrepareResult prepared = unit.prepare_save();
// empty: nothing to send; busy: an earlier batch remains unresolved
// ready: batch.request is an immutable EntityStore save request
send_to_entity_store(prepared.batch.request);

unit.apply_save_result(prepared.batch.token, reply);
```

Use tracked map lookup with explicit existence semantics: operator[] on an owned entity collection must not silently create a database entity. Use emplace for creation and at/checked lookup for access. Example player.items[id].count is permitted only as checked lookup syntax.

prepare_save creates one frozen batch and moves its captured generations into pending state. retry_pending returns exactly that request. Cancellation is permitted only with proof the batch was never dispatched; losing a network acknowledgement is not proof.

## Export rules

| Local state | Export |
| --- | --- |
| New, never submitted | Strict insert with key and assigned non-key fields |
| Loaded and unchanged | None |
| Persisted with changed fields | Strict update with dirty fields and confirmed version |
| Persisted and marked deleted | Strict delete with key and confirmed version; no field patches |
| New then erased before any submission | None |
| Any entity covered by an unresolved batch | No new batch; retain subsequent local edits |

Track field mutation generations plus confirmed values. Coalesce repeated assignments into their latest value. Compound numeric assignment exports a set operation with optimistic version checking; do not translate += into a backend increment unless a separate explicit operation-log API is designed. Embedded objects and value containers serialize their changed top-level field. Value-array reorder is a field replacement; owned-entity ordering is persisted only if an order field is explicitly mapped.

Omitted insert fields remain omitted so database defaults can apply. Empty updates are suppressed. Reject editing an unloaded nested structure; it must first be loaded, or explicitly replaced as a complete known value. Do not invent values for database defaults that are absent from the acknowledgement; reload when business code needs them.

## Pending batch state

| Event | Required action |
| --- | --- |
| prepare_save | Freeze payload, request ID, entity operations, field generations, and submitted values |
| Local mutation during insert/update | Keep a later generation in live state; do not modify the batch |
| Matching committed success | Advance baseline to submitted values and returned versions; recompute remaining dirtiness from live values |
| Insert success after local erase | Transition to persisted, pending-delete; next batch deletes using the returned version |
| Update success after local erase | Preserve deletion intent and use the newly confirmed version |
| Delete success | Finalize deletion; invalidate handles and retain enough completed-token state to ignore duplicate replies |
| Known full rollback | Preserve live edits; classify error before retry/rebase; never report clean |
| Conflict | Preserve edits, block dependent exports, and require explicit reload/merge or discard |
| Unknown outcome / transport timeout | Keep batch unresolved; retry/reconcile exact ID and payload; do not create a new ID |
| Duplicate or mismatched reply | Ignore duplicates; reject unrelated tokens; malformed success remains unresolved |

A field can change A -> B (submitted) -> A while pending. Success establishes B as the database baseline, so the local A remains dirty. Merely comparing the new value to the pre-submit baseline loses this update.

Deleting an entity makes it read-only immediately. Resurrection is not implicit. After confirmed rollback of a never-created insert, a later local deletion can cancel that insert; after an unknown result it cannot.

## Contract extension

Current boundary: include/common/entity_store_contract.hpp. Current entity_patch has create_if_missing but no entity-level operation; validate(save_request) rejects empty fields. Existing save_result carries committed entities and versions but no explicit operation outcome.

Propose a per-change operation enum: legacy_patch, insert, update, delete_entity. Keep legacy_patch as the compatibility default for existing source callers; new tracked entities emit only strict operations. Reject contradictory create_if_missing flags on strict operations.

- Insert: require absence; reject duplicate keys; allow empty non-key fields when schema defaults permit key-only insertion. Return the created version.
- Update: require existence, expected-version match, and nonempty writable changes. No implicit creation.
- Delete: require existence and expected-version match, accept no field patches. A repeat using a different request ID against an absent row is not success; an identical replay returns the original successful result.
- Results: correlate per-change outcomes by stable change index plus target, with operation and optional committed version; deletion has no fabricated live version.
- Validate operation values, operation-specific fields, duplicate targets, key immutability, and one store/partition. Emit at most one operation per target per batch.
- Include operation, version preconditions, and payload in persisted idempotency identity. Deduplication must precede existence/version checks so insert/delete replays work after state changes.
- Preserve transaction rollback semantics in every adapter. Use deterministic relationship ordering for inserts/deletes and reject unsupported dependency cycles before sending.
- Updating CAF inspect layouts is a coordinated wire upgrade, not mixed-binary compatibility. Existing deduplication records may require format versioning; inspect their encoding before implementation.

## Lifecycle boundaries

Do not destroy a UnitOfWork with unresolved writes during ordinary shutdown or plugin replacement. Drain within the existing bounded lifecycle; failure to resolve is reported, not treated as success. Durable handoff/recovery of pending batches is an open requirement, not guaranteed by in-memory tracking.

A same-partition ownership change can be represented as a foreign-key update if both collections participate in one UnitOfWork. Never implement transfer as delete followed by insert. Cross-player transfer with player-based partitions is outside this atomic save boundary and must be designed separately.
