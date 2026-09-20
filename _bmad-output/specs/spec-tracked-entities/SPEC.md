---
id: SPEC-tracked-entities
companions:
  - interface-design.md
  - acceptance-scenarios.md
sources: []
---

# Tracked Entities

Status: design draft; open policy questions remain. No implementation is implied.

## Why

Business code needs to modify entities and nested collections without manually constructing persistence patches. Entity state must produce insert, update, and delete requests while preserving edits made during asynchronous saves. Inventory items are independent database records.

## Capabilities

- **CAP-1**
  - **intent:** Track scalar, nested-object, array, and map mutations automatically.
  - **success:** Assignment, compound assignment, insertion, removal, and nested edits are exported without manual dirty marking; hydration produces no writes.
- **CAP-2**
  - **intent:** Persist the complete entity lifecycle with strict creation, modification, and deletion semantics.
  - **success:** New entities insert; persisted dirty entities update; persisted deleted entities delete; never-submitted entities created and removed locally produce no request.
- **CAP-3**
  - **intent:** Manage related entities as independently persisted records through owned collections.
  - **success:** Adding, editing, and erasing an inventory item generates operations on that item rather than replacing a player JSON field.
- **CAP-4**
  - **intent:** Export a unit of related changes as an atomic persistence request.
  - **success:** A player currency update and item insertion commit together or neither commits; mixed store/partition batches are rejected locally.
- **CAP-5**
  - **intent:** Preserve unsaved work across acknowledgements, failures, and ambiguous outcomes.
  - **success:** A late acknowledgement never clears a newer edit; ambiguous retries retain the original ID and payload; duplicate replies have no additional effect.

## Constraints

- Integrate through EntityStore; business entities must not generate backend SQL or access backend connections.
- Extend the current contract, serialization, validators, and SQLite/MySQL/PostgreSQL/MongoDB/Redis adapters together for strict CRUD. Field erase is not entity deletion.
- A request remains limited to one store and partition; no cross-partition transaction is promised.
- Request preparation is not persistence confirmation. Only a correlated committed success advances the confirmed state.
- Distinguish unloaded, absent/default, explicit NULL, and assigned fields; updates must not overwrite unobserved projected fields.

## Non-goals

- Automatic database schema migrations, transparent unrestricted mutable references, and distributed transactions.
- An automatic timer-based save scheduler or guaranteed recovery after process loss in this first design.

## Success signal

Demonstrate player purchase, nested mutation, inventory deletion, insert-in-flight modification, lost acknowledgement replay, and version conflict with the acceptance scenarios. Persistence exports must require no business-authored field patches and must not lose or duplicate confirmed changes.

## Assumptions

- C++20 typed wrappers provide assignment-like syntax; a UnitOfWork is confined to its owning actor.
- IDs are assigned before insertion and remain immutable; saves are explicitly prepared.
- Collections declared owned use erase-as-physical-delete semantics. Ordinary reference removal does not imply deletion.
- Embedded value changes replace their mapped top-level field; independent entities produce their own CRUD operations.
- One unresolved batch per UnitOfWork; player-owned records use the player ID as partition.

## Open Questions

- Confirm assignment-like typed fields and erase-as-delete for declared-owned collections before API implementation.
- Should player deletion cascade to all inventory records, including unloaded records? Until decided, reject automatic root deletion with dependent collections.
- Is cross-player transfer required in v1? It crosses the proposed partition boundary and needs a separate consistency design.
- Must pending requests survive process loss or plugin hot reload? This draft covers live-process retries only; persistence/recovery of the journal needs an explicit requirement.
