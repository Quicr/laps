#include <doctest/doctest.h>

#include "peering/messages/node_info.h"
#include "peering/messages/subscribe_node_set.h"

#include <iostream>

TEST_CASE("Serialize Subscribe Node Set")
{
    using namespace laps::peering;

    SubscribeNodeSet sns;

    sns.id = 0x1234;
    sns.nodes.emplace(NodeId().Value("1:1"));
    sns.nodes.emplace(NodeId().Value("200:300"));

    auto net_data = sns.Serialize(false);

    CHECK_EQ(net_data.size(), 22);
    CHECK_EQ(net_data.size(), sns.SizeBytes(false));

    SubscribeNodeSet decoded(net_data);

    CHECK_EQ(sns.id, decoded.id);
    CHECK_EQ(sns.nodes.size(), decoded.nodes.size());

    CHECK_EQ(*sns.nodes.begin(), *decoded.nodes.begin());
}

