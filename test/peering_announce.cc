#include <doctest/doctest.h>

#include "peering/messages/announce_info.h"

using namespace std::string_literals;

TEST_CASE("Serialize Announce Info")
{
    using namespace laps::peering;

    AnnounceInfo announce_info;
    announce_info.source_node_id = 0xff00aabbcc;

    announce_info.name_space =
      quicr::messages::TrackNamespace{ "abc"s, "12345"s, "third tuple"s, "now the final tuple"s };
    announce_info.name = quicr::messages::TrackName{ 0, 1, 2, 3, 4, 5, 6, 7, 8 };

    auto net_data = announce_info.Serialize(false);

    CHECK_EQ(net_data.size(), 66);

    AnnounceInfo decoded_ai(net_data);

    CHECK_EQ(announce_info.source_node_id, decoded_ai.source_node_id);

    CHECK(announce_info.name_space == decoded_ai.name_space);
    CHECK(announce_info.name == decoded_ai.name);
}
