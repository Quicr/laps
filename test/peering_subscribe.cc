#include <doctest/doctest.h>

#include "peering/messages/subscribe_info.h"

#include <span>
#include <stdexcept>
#include <string>

namespace {
    laps::peering::SubscribeInfo MakeSubscribeInfo(bool with_new_group)
    {
        using namespace laps::peering;

        SubscribeInfo subscribe_info;
        subscribe_info.source_node_id = 0xff00aabbcc;

        quicr::TrackHash track_hash({});
        track_hash.track_name_hash = 0x9000;
        track_hash.track_namespace_hash = 0xaabbcc;
        track_hash.track_fullname_hash = 0x1234567;
        subscribe_info.track_hash = track_hash;

        subscribe_info.name_space = quicr::TrackNamespace(std::string("namespace"));
        subscribe_info.name = { 't', 'r', 'a', 'c', 'k' };
        subscribe_info.priority = 10;
        subscribe_info.delivery_timeout = 5000;
        subscribe_info.expires = 1000;
        subscribe_info.forward = 1;
        if (with_new_group) {
            subscribe_info.new_group_request_id = 42;
        }

        return subscribe_info;
    }

    void CheckRoundTrip(laps::peering::SubscribeInfo& subscribe_info)
    {
        using namespace laps::peering;

        auto net_data = subscribe_info.Serialize(false, false, true);

        CHECK_EQ(net_data.size(), subscribe_info.SizeBytes());
        CHECK_EQ(subscribe_info.seq, 1);

        SubscribeInfo decoded_si(net_data);

        CHECK_EQ(subscribe_info.seq, decoded_si.seq);
        CHECK_EQ(subscribe_info.source_node_id, decoded_si.source_node_id);
        CHECK_EQ(subscribe_info.track_hash.track_namespace_hash, decoded_si.track_hash.track_namespace_hash);
        CHECK_EQ(subscribe_info.track_hash.track_name_hash, decoded_si.track_hash.track_name_hash);
        CHECK_EQ(subscribe_info.track_hash.track_fullname_hash, decoded_si.track_hash.track_fullname_hash);
        CHECK_EQ(subscribe_info.name_space, decoded_si.name_space);
        CHECK_EQ(subscribe_info.name, decoded_si.name);
        CHECK_EQ(subscribe_info.priority, decoded_si.priority);
        CHECK_EQ(subscribe_info.delivery_timeout, decoded_si.delivery_timeout);
        CHECK_EQ(subscribe_info.expires, decoded_si.expires);
        CHECK_EQ(subscribe_info.forward, decoded_si.forward);
        CHECK_EQ(subscribe_info.new_group_request_id, decoded_si.new_group_request_id);
    }
}

TEST_CASE("Serialize Subscribe Info")
{
    auto subscribe_info = MakeSubscribeInfo(true);
    CheckRoundTrip(subscribe_info);
}

TEST_CASE("Serialize Subscribe Info without new group request")
{
    auto subscribe_info = MakeSubscribeInfo(false);
    CheckRoundTrip(subscribe_info);
    CHECK_FALSE(subscribe_info.new_group_request_id.has_value());
}

TEST_CASE("Subscribe Info truncated input throws")
{
    using namespace laps::peering;
    const std::vector<uint8_t> truncated{ 0x00, 0x01 };
    CHECK_THROWS_AS(SubscribeInfo(std::span<const uint8_t>{ truncated }), std::out_of_range);
}
