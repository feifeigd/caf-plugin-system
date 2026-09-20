#include "common/entity_store_contract.hpp"
#include "common/message_tags.hpp"

#include <caf/binary_serializer.hpp>
#include <caf/binary_deserializer.hpp>
#include <cassert>
#include <string>

using namespace caf_plugin_system::entity_store;

namespace {

entity_ref order(std::string partition = "order-20260903-42") {
    return entity_ref{
        .store = "commerce",
        .partition = std::move(partition),
        .entity = "order",
        .key = {{"order_id", value::text("20260903-42")}},
    };
}

} // namespace

int main() {
    static_assert(caf::type_id<load_request>::value == 278);
    static_assert(caf::type_id<save_request>::value == 280);

    load_request load{
        .target = order(),
        .fields = {.all_fields = false, .names = {"status", "paid_amount"}},
    };
    assert(validate(load).empty());
    load.fields.names.push_back("paid_amount");
    assert(validate(load).find("duplicate") != std::string::npos);

    save_request save{
        .request_id = "payment-callback-20260903-42",
        .changes = {
            entity_patch{
                .target = order(),
                .fields = {
                    {patch_op::set, "status", value::text("paid")},
                    {patch_op::increment, "paid_amount", value::decimal("99.50")},
                },
                .check_version = true,
                .expected_version = 7,
            },
            entity_patch{
                .target = entity_ref{
                    .store = "commerce",
                    .partition = "order-20260903-42",
                    .entity = "payment",
                    .key = {
                        {"payment_id", value::text("pay-9001")},
                        {"order_id", value::text("20260903-42")},
                    },
                },
                .fields = {
                    {patch_op::set, "status", value::text("succeeded")},
                },
                .create_if_missing = true,
            },
        },
    };
    assert(validate(save).empty());

    save.changes[1].target.partition = "order-20260903-99";
    assert(validate(save).find("same partition") != std::string::npos);
    save.changes[1].target.partition = "order-20260903-42";

    save.changes[0].fields.push_back(
        {patch_op::set, "order_id", value::text("another-order")});
    assert(validate(save).find("key field") != std::string::npos);
    save.changes[0].fields.pop_back();

    auto strict = save;
    strict.changes.resize(1);
    auto& change = strict.changes.front();
    change.operation = entity_operation::delete_entity;
    change.fields.clear();
    assert(validate(strict).empty());
    caf::byte_buffer request_bytes;
    caf::binary_serializer request_writer{request_bytes};
    assert(request_writer.apply(strict));
    save_request decoded_request;
    caf::binary_deserializer request_reader{request_bytes};
    assert(request_reader.apply(decoded_request));
    assert(decoded_request.changes[0].operation == entity_operation::delete_entity);
    assert(decoded_request.changes[0].expected_version == 7);
    save_result reply;
    reply.committed = true;
    reply.entities.push_back({order(), 0, entity_operation::delete_entity});
    caf::byte_buffer reply_bytes;
    caf::binary_serializer reply_writer{reply_bytes};
    assert(reply_writer.apply(reply));
    save_result decoded_reply;
    caf::binary_deserializer reply_reader{reply_bytes};
    assert(reply_reader.apply(decoded_reply));
    assert(decoded_reply.entities[0].operation == entity_operation::delete_entity);
    assert(decoded_reply.entities[0].version == 0);
    auto duplicated = strict;
    duplicated.changes.push_back(change);
    assert(validate(duplicated).find("duplicate") != std::string::npos);
    change.check_version = false;
    assert(!validate(strict).empty());
    change.operation = entity_operation::insert;
    change.expected_version = 0;
    assert(validate(strict).empty()); // Key-only insert permits backend defaults.
    change.create_if_missing = true;
    assert(!validate(strict).empty());
    change.create_if_missing = false;
    change.operation = static_cast<entity_operation>(255);
    assert(!validate(strict).empty());
    change.operation = entity_operation::insert;
    change.fields = {{patch_op::increment, "paid_amount", value::signed_integer(1)}};
    assert(!validate(strict).empty());

    save.changes[0].fields[1].data = value::text("not-a-number");
    assert(validate(save).find("numeric") != std::string::npos);
}
