#include <doctest/doctest.h>

#include "peering/messages/node_info.h"

#include <iostream>

TEST_CASE("Validate Node ID")
{
    using namespace laps::peering;

    CHECK_THROWS(NodeId().Value("1234")); // Invalid to not include a colon
    CHECK_THROWS(NodeId().Value("1.70000:1.2"));

    CHECK(51539607586 == NodeId().Value("12:34"));
    CHECK(281483566645282 == NodeId().Value("1.2:34"));
    CHECK_EQ("1234.5678:100.6109", NodeId().Value(347364508091815901));
}

TEST_CASE("Serialize Node Info")
{
    using namespace laps::peering;

    NodeInfo ni;

    ni.type = NodeType::kEdge;
    ni.id = NodeId().Value("12:34");
    ni.contact = "localhost:1234";
    ni.latitude = 47.6482974;
    ni.longitude = -122.5327124;

    ni.path.push_back({ NodeId().Value("1:1"), 54321 });
    ni.path.push_back({ NodeId().Value("2:2"), 12345 });

    std::vector<uint8_t> data;
    data.reserve(10000000);
    data << ni;

    ni.contact = "hello:1234";
    data << ni;

    auto net_data = ni.Serialize();

    CHECK_EQ(net_data.size(), 79);

    NodeInfo decoded_ni(net_data);

    CHECK_EQ(ni.id, decoded_ni.id);

    CHECK_EQ(ni.type, decoded_ni.type);
    CHECK_EQ(ni.contact, decoded_ni.contact);
    CHECK_EQ(ni.longitude, decoded_ni.longitude);
    CHECK_EQ(ni.latitude, decoded_ni.latitude);

    for (size_t i = 0; i < ni.path.size(); i++) {
        CHECK_EQ(ni.path.at(i).id, decoded_ni.path.at(i).id);
        CHECK_EQ(ni.path.at(i).srtt_us, decoded_ni.path.at(i).srtt_us);
    }
}