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
    data_object.group_id = 0x9001;
    data_object.sub_group_id = 0x5001;
    data_object.data_length = test_data.size();
    data_object.data = data;

    auto net_data = data_object.Serialize();

    CHECK_EQ(net_data.size(), 50);

    DataObject decoded(net_data);

    CHECK_EQ(data_object.type, decoded.type);
    CHECK_EQ(data_object.sns_id, decoded.sns_id);
    CHECK_EQ(data_object.track_full_name_hash, decoded.track_full_name_hash);
    CHECK_EQ(data_object.group_id, decoded.group_id);
    CHECK_EQ(data_object.sub_group_id, decoded.sub_group_id);
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
    data_object.group_id = 0x9001;
    data_object.sub_group_id = 0x5001;
    data_object.track_full_name_hash = 0xabcdef;
    data_object.data_length = test_data.size();
    data_object.data = data;

    auto net_data = data_object.Serialize();

    CHECK_EQ(net_data.size(), 55);

    DataObject decoded(net_data);

    CHECK_EQ(data_object.type, decoded.type);
    CHECK_EQ(data_object.sns_id, decoded.sns_id);
    CHECK_EQ(data_object.track_full_name_hash, decoded.track_full_name_hash);
    CHECK_EQ(data_object.group_id, decoded.group_id);
    CHECK_EQ(data_object.sub_group_id, decoded.sub_group_id);
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

    CHECK_EQ(net_data.size(), 30);

    DataObject decoded(net_data);

    CHECK_EQ(data_object.type, decoded.type);
    CHECK_EQ(data_object.data_length, decoded.data_length);

    std::vector<uint8_t> decoded_data { decoded.data.begin(), decoded.data.end() };
    CHECK_EQ(data, decoded_data);
}

TEST_CASE("Serialize Data Object new stream append")
{
    std::string test_data;

    for (int i=0; i < 20'000; i++) {
        test_data.push_back('a');
    }
    test_data.push_back('z');

    std::vector<uint8_t> data(test_data.begin(), test_data.end());

    DataObject data_object;
    data_object.type = DataObjectType::kNewStream;
    data_object.sns_id = 0x1234;
    data_object.priority = 100;
    data_object.ttl = 5000;
    data_object.group_id = 0x9001;
    data_object.sub_group_id = 0x5001;
    data_object.track_full_name_hash = 0xabcdef;
    data_object.data_length = test_data.size();
    data_object.data = data;

    auto net_data = data_object.Serialize();

    CHECK_EQ(net_data.size(), 20032);


    std::vector net_data_part1(net_data.begin(), net_data.begin() + 2030);
    std::vector net_data_part2(net_data.begin() + 2030, net_data.begin() + 7000);
    std::vector net_data_part3(net_data.begin() + 7000, net_data.begin() + 15000);
    std::vector net_data_part4(net_data.begin() + 15000, net_data.end());

    DataObject decoded(net_data_part1);
    CHECK_EQ(decoded.SizeBytes(), 2030);
    CHECK_EQ(decoded.header_len, 31);

    decoded.AppendData(net_data_part2);
    CHECK_EQ(decoded.SizeBytes(), 7000);

    decoded.AppendData(net_data_part3);
    CHECK_EQ(decoded.SizeBytes(), 15000);

    decoded.AppendData(net_data_part4);
    CHECK_EQ(decoded.SizeBytes(), 20032);

    CHECK_EQ(data_object.type, decoded.type);
    CHECK_EQ(data_object.sns_id, decoded.sns_id);
    CHECK_EQ(data_object.track_full_name_hash, decoded.track_full_name_hash);
    CHECK_EQ(data_object.group_id, decoded.group_id);
    CHECK_EQ(data_object.sub_group_id, decoded.sub_group_id);
    CHECK_EQ(data_object.priority, decoded.priority);
    CHECK_EQ(data_object.ttl, decoded.ttl);
    CHECK_EQ(data_object.data_length, decoded.data_length);

    std::vector<uint8_t> decoded_data { decoded.data.begin(), decoded.data.end() };
    CHECK_EQ(data, decoded_data);
}