
#include <doctest/doctest.h>
#include <random>

#include "peering/info_base.h"

using namespace std::string_literals;

std::vector<quicr::FullTrackName> GenerateFullTrackNames(int count)
{
    std::srand(std::time({}));
    std::vector<quicr::FullTrackName> full_names;

    for (int i=0; i < count; i++) {
        const int rvalue = std::rand();
        const auto name = std::to_string(rvalue);
        full_names.push_back(
          { quicr::messages::TrackNamespace{ "first"s, "second"s, "third"s, "final namespace tuple"s, "r=" + name },
            { name.begin(), name.end() } });
    }

    return full_names;
}

TEST_CASE("Add/Remove Announces")
{
    using namespace laps::peering;

    const auto full_names = GenerateFullTrackNames(30);
    std::shared_ptr<InfoBase> ib = std::make_shared<InfoBase>();

    // Add to info base
    for (const auto& fn : full_names) {
        const auto fn_hash = quicr::TrackHash(fn);
        AnnounceInfo ai;
        ai.fullname_hash = fn_hash.track_fullname_hash;
        ai.name_space = fn.name_space,
        ai.name = fn.name;
        ai.source_node_id = 0x12345678;

        ib->AddAnnounce(ai);
    }

    CHECK_EQ(ib->prefix_lookup_announces_.size(), 5);
    for (const auto& [_, v]: ib->prefix_lookup_announces_) {
        CHECK_EQ(v.size(), 30);
    }

    // Remove first and last 5
    for (int i=0; i < 5; i++) {
        AnnounceInfo ai;
        ai.source_node_id = 0x12345678;

        auto fn = full_names.at(i);
        auto fn_hash = quicr::TrackHash(fn);

        ai.fullname_hash = fn_hash.track_fullname_hash;
        ai.name_space = fn.name_space,
        ai.name = fn.name;

        ib->RemoveAnnounce(ai);

        fn = full_names.at(full_names.size() - (i+1));
        fn_hash = quicr::TrackHash(fn);

        ai.fullname_hash = fn_hash.track_fullname_hash;
        ai.name_space = fn.name_space,
        ai.name = fn.name;

        ib->RemoveAnnounce(ai);
    }

    CHECK_EQ(ib->prefix_lookup_announces_.size(), 5);
    for (const auto& [_, v]: ib->prefix_lookup_announces_) {
        CHECK_EQ(v.size(), 20);
    }


    // Remove from info base
    for (const auto& fn : full_names) {
        const auto fn_hash = quicr::TrackHash(fn);
        AnnounceInfo ai;
        ai.fullname_hash = fn_hash.track_fullname_hash;
        ai.name_space = fn.name_space,
        ai.name = fn.name;
        ai.source_node_id = 0x12345678;

        ib->RemoveAnnounce(ai);
    }

    CHECK(ib->prefix_lookup_announces_.empty());
}
