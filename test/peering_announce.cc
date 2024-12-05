#include <doctest/doctest.h>

#include "peering/messages/announce_info.h"

#include <iostream>

TEST_CASE("Serialize Announce Info")
{
    using namespace laps::peering;

    AnnounceInfo announce_info;
    announce_info.source_node_id = 0xff00aabbcc;

    FullNameHash full_name_hash;
    full_name_hash.name_hash = 0x9000;
    full_name_hash.namespace_tuples.push_back(0x1);
    full_name_hash.namespace_tuples.push_back(0x90000001);
    full_name_hash.namespace_tuples.push_back(0x14);
    full_name_hash.namespace_tuples.push_back(0xaa0bb0cc0dd0ee);

    announce_info.full_name = full_name_hash;

    auto net_data = announce_info.Serialize(false);

    CHECK_EQ(net_data.size(), 49);

    AnnounceInfo decoded_ai(net_data);

    CHECK_EQ(announce_info.source_node_id, decoded_ai.source_node_id);

    CHECK_EQ(announce_info.full_name.namespace_tuples.size(), decoded_ai.full_name.namespace_tuples.size());

    for (size_t i = 0; i < announce_info.full_name.namespace_tuples.size(); ++i) {
        CHECK_EQ(announce_info.full_name.namespace_tuples[i], decoded_ai.full_name.namespace_tuples[i]);
    }

    CHECK_EQ(announce_info.full_name.name_hash, decoded_ai.full_name.name_hash);
}

