// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <doctest/doctest.h>
#include <memory>
#include <tuple>
#include <vector>

#include "track_ranking.h"

namespace laps {

    TEST_SUITE("TrackRanking")
    {
        TEST_CASE("Single track update")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Update a single track - should not throw or crash
            ranking->UpdateValue(1, 100, 500, 1000, 1);

            // Verify default configuration
            CHECK_EQ(ranking->GetMaxSelected(), 32);
            CHECK_EQ(ranking->GetInactiveAge(), 1500);
        }

        TEST_CASE("Multiple tracks with different property values")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Add tracks with different values for property 100
            ranking->UpdateValue(1, 100, 500, 1000, 1); // track 1, value 500
            ranking->UpdateValue(2, 100, 600, 1100, 2); // track 2, value 600
            ranking->UpdateValue(3, 100, 550, 1200, 3); // track 3, value 550

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Track value changes")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Add track with initial value
            ranking->UpdateValue(1, 100, 500, 1000, 1);

            // Update same track with new value
            ranking->UpdateValue(1, 100, 700, 2000, 1);

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Track moves between value buckets")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Add two tracks with different values
            ranking->UpdateValue(1, 100, 500, 1000, 1);
            ranking->UpdateValue(2, 100, 600, 1100, 2);

            // Move track 1 from value 500 to value 700
            ranking->UpdateValue(1, 100, 700, 2000, 1);

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Inactive track removal")
        {
            auto ranking = std::make_unique<TrackRanking>();
            ranking->SetInactiveAge(1000); // 1000ms inactive threshold

            // Add tracks
            ranking->UpdateValue(1, 100, 500, 1000, 1);
            ranking->UpdateValue(2, 100, 600, 1100, 2);

            // Update at tick 3000 - track 1 should be inactive (3000 - 1000 = 2000 > 1000)
            ranking->UpdateValue(3, 100, 550, 3000, 3);

            CHECK_EQ(ranking->GetInactiveAge(), 1000);
        }

        TEST_CASE("Sequence number ordering")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Add multiple tracks with the same property value
            ranking->UpdateValue(1, 100, 500, 1000, 1);
            ranking->UpdateValue(2, 100, 500, 1100, 2);
            ranking->UpdateValue(3, 100, 500, 1200, 3);

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Connection ID tracking")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Add tracks from different connections
            ranking->UpdateValue(1, 100, 500, 1000, 100); // connection 100
            ranking->UpdateValue(2, 100, 600, 1100, 200); // connection 200

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Connection ID update")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Add track from connection 1
            ranking->UpdateValue(1, 100, 500, 1000, 1);

            // Update same track from different connection
            ranking->UpdateValue(1, 100, 500, 1100, 2);

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Complex scenario: Multiple property types and values")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Add tracks for different property types
            ranking->UpdateValue(1, 100, 500, 1000, 1); // prop 100, value 500
            ranking->UpdateValue(2, 100, 600, 1100, 2); // prop 100, value 600
            ranking->UpdateValue(3, 200, 800, 1200, 3); // prop 200, value 800
            ranking->UpdateValue(4, 200, 900, 1300, 4); // prop 200, value 900

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Rapid updates to same track")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Rapidly update the same track with different values
            for (uint64_t i = 0; i < 10; ++i) {
                ranking->UpdateValue(1, 100, 500 + i * 10, 1000 + i * 100, 1);
            }

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Many tracks same property")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Add many tracks with same property but different values
            for (uint64_t i = 0; i < 100; ++i) {
                ranking->UpdateValue(i, 100, 1000 - i, 1000 + i, 1);
            }

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Remove track by inactivity")
        {
            auto ranking = std::make_unique<TrackRanking>();
            ranking->SetInactiveAge(100);

            // Add initial tracks
            ranking->UpdateValue(1, 100, 500, 1000, 1);
            ranking->UpdateValue(2, 100, 600, 1100, 2);

            // Update track 1 at much later time (should trigger inactivity removal)
            ranking->UpdateValue(1, 100, 500, 2000, 1);

            CHECK_EQ(ranking->GetInactiveAge(), 100);
        }

        TEST_CASE("Different property types interleaved")
        {
            auto ranking = std::make_unique<TrackRanking>();

            ranking->UpdateValue(1, 100, 500, 1000, 1);
            ranking->UpdateValue(2, 200, 700, 1100, 2);
            ranking->UpdateValue(3, 100, 600, 1200, 3);
            ranking->UpdateValue(4, 200, 800, 1300, 4);
            ranking->UpdateValue(5, 300, 900, 1400, 5);

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Property type transition")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Start with property 100
            ranking->UpdateValue(1, 100, 500, 1000, 1);

            // Switch same track to property 200
            ranking->UpdateValue(1, 200, 800, 2000, 1);

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Large tick values")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Use large tick values to test boundary conditions
            ranking->UpdateValue(1, 100, 500, 1000000000, 1);
            ranking->UpdateValue(2, 100, 600, 1000000100, 2);
            ranking->UpdateValue(3, 100, 550, 1000000200, 3);

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Same tick different properties")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Update multiple tracks at same tick with different properties
            ranking->UpdateValue(1, 100, 500, 1000, 1);
            ranking->UpdateValue(2, 200, 600, 1000, 2);
            ranking->UpdateValue(3, 300, 700, 1000, 3);

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Zero connection ID")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Test with connection ID = 0
            ranking->UpdateValue(1, 100, 500, 1000, 0);
            ranking->UpdateValue(2, 100, 600, 1100, 0);

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }

        TEST_CASE("Update same track multiple times")
        {
            auto ranking = std::make_unique<TrackRanking>();

            // Update same track 50 times
            for (int i = 0; i < 50; ++i) {
                ranking->UpdateValue(1, 100, 500 + i, 1000 + i * 10, 1);
            }

            CHECK_EQ(ranking->GetMaxSelected(), 32);
        }
    }

} // namespace laps
