#pragma once
#include "common/entity_store_contract.hpp"

// Reused by real SQLite/MySQL/PostgreSQL/MongoDB/Redis integration harnesses.
namespace caf_plugin_system::entity_store::test {
template<class Save, class Load, class Check>
bool strict_crud(save_request seed, Save save, Load load, Check check) {
    using op = entity_operation;
    seed.request_id = "strict-insert";
    auto& inserted = seed.changes.front();
    inserted.target.key.front().data.text_value += "-strict";
    inserted.operation = op::insert;
    inserted.create_if_missing = false;
    inserted.check_version = false;
    inserted.expected_version = 0;
    const auto target = inserted.target;
    auto created = save(seed);
    if (!check(created.committed && created.entities.size() == 1
               && created.entities[0].version == 1 && created.entities[0].operation == op::insert,
               "strict insert")) return false;
    auto replay = save(seed);
    if (!check(replay.committed && replay.entities.size() == 1 && replay.entities[0].version == 1
               && replay.entities[0].operation == op::insert, "strict insert replay")) return false;
    auto duplicate = seed; duplicate.request_id = "strict-duplicate";
    if (!check(save(duplicate).code == result_code::conflict, "strict insert must not overwrite")) return false;

    auto update = seed; update.request_id = "strict-update";
    auto& changed = update.changes.front();
    changed.operation = op::update; changed.check_version = true; changed.expected_version = 1;
    changed.fields = {{patch_op::set, "status", value::text("strict-updated")}};
    auto updated = save(update);
    if (!check(updated.committed && updated.entities.size() == 1 && updated.entities[0].version == 2
               && updated.entities[0].operation == op::update, "strict update")) return false;
    auto alternate = update; alternate.changes[0].operation = op::legacy_patch;
    if (!check(save(alternate).code == result_code::conflict, "operation is part of idempotency identity")) return false;
    auto missing = update; missing.request_id = "strict-update-missing";
    missing.changes[0].target.key.front().data.text_value += "-absent";
    if (!check(save(missing).code == result_code::not_found, "strict update must not insert")) return false;

    auto remove = update; remove.request_id = "strict-delete";
    remove.changes[0].operation = op::delete_entity;
    remove.changes[0].fields.clear(); remove.changes[0].expected_version = 2;
    auto stale = remove; stale.request_id = "strict-delete-stale";
    stale.changes[0].expected_version = 1;
    if (!check(save(stale).code == result_code::conflict && load(target).version == 2,
               "stale delete preserves row")) return false;

    auto failed_insert = seed; failed_insert.request_id = "strict-insert-rollback";
    failed_insert.changes[0].target.key.front().data.text_value += "-rollback";
    auto conflict = update.changes[0]; conflict.expected_version = 999;
    failed_insert.changes.push_back(conflict);
    if (!check(!save(failed_insert).committed
               && load(failed_insert.changes[0].target).code == result_code::not_found,
               "insert rolls back with failed batch")) return false;

    auto other = seed; other.request_id = "strict-other";
    other.changes[0].target.key.front().data.text_value += "-other";
    if (!check(save(other).committed, "second strict insert")) return false;
    auto rollback = remove; rollback.request_id = "strict-delete-rollback";
    auto bad = other.changes[0]; bad.operation = op::update;
    bad.check_version = true; bad.expected_version = 999;
    rollback.changes.push_back(bad);
    if (!check(!save(rollback).committed && load(target).version == 2,
               "delete rolls back with failed batch")) return false;
    auto removed = save(remove);
    if (!check(removed.committed && removed.entities.size() == 1 && removed.entities[0].operation == op::delete_entity
               && removed.entities[0].version == 0 && load(target).code == result_code::not_found,
               "strict delete")) return false;
    auto deleted_replay = save(remove);
    if (!check(deleted_replay.committed && deleted_replay.entities.size() == 1 && deleted_replay.entities[0].operation == op::delete_entity,
               "delete replay after row removed")) return false;
    auto missing_delete = remove; missing_delete.request_id = "strict-delete-missing";
    if (!check(save(missing_delete).code == result_code::not_found, "delete missing row")) return false;
    auto recreate = seed; recreate.request_id = "strict-recreate";
    if (!check(save(recreate).committed, "reinsert deleted key")) return false;
    return check(save(remove).committed && load(target).version == 1,
                 "old delete replay must not delete reinserted row");
}
} // namespace caf_plugin_system::entity_store::test
