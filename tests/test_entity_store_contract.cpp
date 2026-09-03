#include "common/entity_store_contract.hpp"
#include "common/message_tags.hpp"

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

    save.changes[0].fields[1].data = value::text("not-a-number");
    assert(validate(save).find("numeric") != std::string::npos);
}
