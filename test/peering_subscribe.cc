#include <doctest/doctest.h>

#include "peering/subscribe_info.h"

#include <iostream>

TEST_CASE("Serialize Subscribe Info")
{
    using namespace laps::peering;

    SubscribeInfo subscribe_info;
    subscribe_info.id = 0x123456;
    subscribe_info.source_node_id = 0xff00aabbcc;

    FullNameHash full_name_hash;
    full_name_hash.name = 0x9000;
    full_name_hash.namespace_tuples.push_back(0x1);
    full_name_hash.namespace_tuples.push_back(0x90000001);
    full_name_hash.namespace_tuples.push_back(0x14);
    full_name_hash.namespace_tuples.push_back(0xaa0bb0cc0dd0ee);

    subscribe_info.full_name = full_name_hash;

    auto net_data = subscribe_info.Serialize(false);

    CHECK_EQ(net_data.size(), 57);

    SubscribeInfo decoded_si(net_data);

    CHECK_EQ(subscribe_info.id, decoded_si.id);
    CHECK_EQ(subscribe_info.source_node_id, decoded_si.source_node_id);

    CHECK_EQ(subscribe_info.full_name.namespace_tuples.size(), decoded_si.full_name.namespace_tuples.size());

    for (size_t i = 0; i < subscribe_info.full_name.namespace_tuples.size(); ++i) {
        CHECK_EQ(subscribe_info.full_name.namespace_tuples[i], decoded_si.full_name.namespace_tuples[i]);
    }

    CHECK_EQ(subscribe_info.full_name.name, decoded_si.full_name.name);
}

