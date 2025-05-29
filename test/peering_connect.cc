#include <doctest/doctest.h>

#include "peering/messages/connect.h"
#include "peering/messages/connect_response.h"

#include <iostream>

TEST_CASE("Serialize Connect")
{
    using namespace laps::peering;

    Connect connect;
    connect.mode = PeerMode::kBoth;
    connect.node_info.type = NodeType::kEdge;
    connect.node_info.id = NodeId().Value("12:34");
    connect.node_info.contact = "localhost:1234";
    connect.node_info.latitude = 47.6482974;
    connect.node_info.longitude = -122.5327124;

    connect.node_info.path.push_back({ NodeId().Value("1:1"), 54321 });
    connect.node_info.path.push_back({ NodeId().Value("2:2"), 12345 });

    auto net_data = connect.Serialize();

    CHECK_EQ(net_data.size(), connect.SizeBytes() + kCommonHeadersSize);
    CHECK_EQ(net_data.size(), 80);

    Connect decoded_c({ net_data.begin() + kCommonHeadersSize, net_data.end() });

    CHECK_EQ(decoded_c.mode, PeerMode::kBoth);
    CHECK_EQ(connect.node_info.id, decoded_c.node_info.id);

    CHECK_EQ(connect.node_info.type, decoded_c.node_info.type);
    CHECK_EQ(connect.node_info.contact, decoded_c.node_info.contact);
    CHECK_EQ(connect.node_info.longitude, decoded_c.node_info.longitude);
    CHECK_EQ(connect.node_info.latitude, decoded_c.node_info.latitude);
}

TEST_CASE("Serialize Connect Response")
{
    using namespace laps::peering;

    ConnectResponse connect_resp;
    connect_resp.error = ProtocolError::kNoError;

    connect_resp.node_info = NodeInfo();
    connect_resp.node_info->type = NodeType::kEdge;
    connect_resp.node_info->id = NodeId().Value("50:60");
    connect_resp.node_info->contact = "relay.m10x.org:33435";
    connect_resp.node_info->latitude = 47.6482900;
    connect_resp.node_info->longitude = -122.5327100;

    connect_resp.node_info->path.push_back({ NodeId().Value("1:99"), 54321 });

    auto net_data = connect_resp.Serialize();

    CHECK_EQ(net_data.size(), connect_resp.SizeBytes() + kCommonHeadersSize);
    CHECK_EQ(net_data.size(), 71);

    ConnectResponse decoded_cr({ net_data.begin() + kCommonHeadersSize, net_data.end() });

    CHECK_EQ(connect_resp.error, ProtocolError::kNoError);
    CHECK_EQ(connect_resp.node_info->id, decoded_cr.node_info->id);

    CHECK_EQ(connect_resp.node_info->type, decoded_cr.node_info->type);
    CHECK_EQ(connect_resp.node_info->contact, decoded_cr.node_info->contact);
    CHECK_EQ(connect_resp.node_info->longitude, decoded_cr.node_info->longitude);
    CHECK_EQ(connect_resp.node_info->latitude, decoded_cr.node_info->latitude);
}

TEST_CASE("Serialize Connect Response With Error")
{
    using namespace laps::peering;

    ConnectResponse connect_resp;
    connect_resp.error = ProtocolError::kConnectError;

    auto net_data = connect_resp.Serialize();

    CHECK_EQ(net_data.size(), 9);

    ConnectResponse decoded_cr({ net_data.begin() + kCommonHeadersSize, net_data.end() });

    CHECK_EQ(connect_resp.error, ProtocolError::kConnectError);
}

TEST_CASE("Serialize Connect Response Bad Serialize")
{
    using namespace laps::peering;

    ConnectResponse connect_resp;
    connect_resp.error = ProtocolError::kNoError;

    CHECK_THROWS(connect_resp.Serialize());
}