#include <doctest/doctest.h>

#include "peering/messages/data_header.h"

#include <iostream>

using namespace laps::peering;

TEST_CASE("Serialize Data Header Datagram")
{
    std::string test_data = "This is a test data payload";
    std::vector<uint8_t> data(test_data.begin(), test_data.end());

    DataHeader data_hdr;
    data_hdr.type = DataType::kDatagram;
    data_hdr.sns_id = 0x1234;
    data_hdr.track_full_name_hash = 0xabcdef;

    auto net_data = data_hdr.Serialize();

    CHECK_EQ(net_data.size(), 14);

    DataHeader decoded(net_data);

    CHECK_EQ(data_hdr.type, decoded.type);
    CHECK_EQ(data_hdr.sns_id, decoded.sns_id);
    CHECK_EQ(data_hdr.track_full_name_hash, decoded.track_full_name_hash);
}

TEST_CASE("Serialize Data Header new stream")
{
    DataHeader data_header;
    data_header.type = DataType::kNewStream;
    data_header.sns_id = 0x1234;
    data_header.priority = 100;
    data_header.ttl = 5000;
    data_header.track_full_name_hash = 0xabcdef;

    auto net_data = data_header.Serialize();

    CHECK_EQ(net_data.size(), 19);

    DataHeader decoded(net_data);

    CHECK_EQ(data_header.type, decoded.type);
    CHECK_EQ(data_header.sns_id, decoded.sns_id);
    CHECK_EQ(data_header.track_full_name_hash, decoded.track_full_name_hash);
    CHECK_EQ(data_header.priority, decoded.priority);
    CHECK_EQ(data_header.ttl, decoded.ttl);
}

TEST_CASE("Serialize Data Header existing stream")
{
    std::string test_data = "This is a test data payload";
    std::vector<uint8_t> data(test_data.begin(), test_data.end());

    DataHeader data_header;
    data_header.type = DataType::kExistingStream;
    data_header.sns_id = 0x1234;
    data_header.priority = 100;
    data_header.ttl = 5000;
    data_header.track_full_name_hash = 0xabcdef;

    auto net_data = data_header.Serialize();

    CHECK_EQ(net_data.size(), 2);

    DataHeader decoded(net_data);

    CHECK_EQ(data_header.type, decoded.type);
}
