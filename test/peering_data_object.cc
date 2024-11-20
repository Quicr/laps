#include <doctest/doctest.h>

#include "peering/messages/data_object.h"

#include <iostream>

using namespace laps::peering;

TEST_CASE("Serialize Data Object Datagram")
{
    std::string test_data = "This is a test data payload";
    std::vector<uint8_t> data(test_data.begin(), test_data.end());

    DataObject data_object;
    data_object.type = DataObjectType::kDatagram;
    data_object.sns_id = 0x1234;
    data_object.track_full_name_hash = 0xabcdef;
    data_object.data_length = test_data.size();
    data_object.data = data;

    auto net_data = data_object.Serialize();

    CHECK_EQ(net_data.size(), 41);

    DataObject decoded(net_data);

    CHECK_EQ(data_object.type, decoded.type);
    CHECK_EQ(data_object.sns_id, decoded.sns_id);
    CHECK_EQ(data_object.track_full_name_hash, decoded.track_full_name_hash);
    CHECK_EQ(data_object.data_length, decoded.data_length);

    std::vector<uint8_t> decoded_data { decoded.data.begin(), decoded.data.end() };
    CHECK_EQ(data, decoded_data);
}

TEST_CASE("Serialize Data Object new stream")
{
    std::string test_data = "This is a test data payload";
    std::vector<uint8_t> data(test_data.begin(), test_data.end());

    DataObject data_object;
    data_object.type = DataObjectType::kNewStream;
    data_object.sns_id = 0x1234;
    data_object.priority = 100;
    data_object.ttl = 5000;
    data_object.track_full_name_hash = 0xabcdef;
    data_object.data_length = test_data.size();
    data_object.data = data;

    auto net_data = data_object.Serialize();

    CHECK_EQ(net_data.size(), 44);

    DataObject decoded(net_data);

    CHECK_EQ(data_object.type, decoded.type);
    CHECK_EQ(data_object.sns_id, decoded.sns_id);
    CHECK_EQ(data_object.track_full_name_hash, decoded.track_full_name_hash);
    CHECK_EQ(data_object.priority, decoded.priority);
    CHECK_EQ(data_object.ttl, decoded.ttl);
    CHECK_EQ(data_object.data_length, decoded.data_length);

    std::vector<uint8_t> decoded_data { decoded.data.begin(), decoded.data.end() };
    CHECK_EQ(data, decoded_data);
}

TEST_CASE("Serialize Data Object existing stream")
{
    std::string test_data = "This is a test data payload";
    std::vector<uint8_t> data(test_data.begin(), test_data.end());

    DataObject data_object;
    data_object.type = DataObjectType::kExistingStream;
    data_object.sns_id = 0x1234;
    data_object.priority = 100;
    data_object.ttl = 5000;
    data_object.track_full_name_hash = 0xabcdef;
    data_object.data_length = test_data.size();
    data_object.data = data;

    auto net_data = data_object.Serialize();

    CHECK_EQ(net_data.size(), 29);

    DataObject decoded(net_data);

    CHECK_EQ(data_object.type, decoded.type);
    CHECK_EQ(data_object.data_length, decoded.data_length);

    std::vector<uint8_t> decoded_data { decoded.data.begin(), decoded.data.end() };
    CHECK_EQ(data, decoded_data);
}
