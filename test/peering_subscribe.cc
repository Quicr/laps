#include <doctest/doctest.h>

#include "peering/messages/subscribe_info.h"

#include <iostream>

TEST_CASE("Serialize Subscribe Info")
{
    using namespace laps::peering;

    SubscribeInfo subscribe_info;
    subscribe_info.source_node_id = 0xff00aabbcc;

    quicr::TrackHash track_hash({});
    track_hash.track_name_hash = 0x9000;
    track_hash.track_namespace_hash = 0xaabbcc;
    track_hash.track_fullname_hash = 0x1234567;

    subscribe_info.track_hash = track_hash;

    auto net_data = subscribe_info.Serialize(false);

    CHECK_EQ(net_data.size(), 38);
    CHECK_EQ(subscribe_info.seq, 1);

    SubscribeInfo decoded_si(net_data);

    CHECK_EQ(subscribe_info.source_node_id, decoded_si.source_node_id);

    CHECK_EQ(subscribe_info.track_hash.track_namespace_hash, decoded_si.track_hash.track_namespace_hash);
    CHECK_EQ(subscribe_info.track_hash.track_name_hash, decoded_si.track_hash.track_name_hash);
    CHECK_EQ(subscribe_info.track_hash.track_fullname_hash, decoded_si.track_hash.track_fullname_hash);
}

