#include <catch2/catch.hpp>
#include "slic3r/Utils/CommunityFilamentAPI.hpp"
#include <chrono>
#include <thread>
#include <atomic>

TEST_CASE("Test CommunityFilamentAPI async fetch", "[CommunityFilamentAPI]") {
    std::atomic<bool> callback_fired{false};
    Slic3r::CommunityFilamentProfile received_profile;
    
    // Call the mock async API with a UID that simulates a PLA profile
    Slic3r::CommunityFilamentAPI::fetch_profile_by_nfc("04:XX:YY:ZZ:PLA", 
        [&](const Slic3r::CommunityFilamentProfile& profile) {
            received_profile = profile;
            callback_fired = true;
        }
    );
    
    // Wait for the async callback (mock takes 500ms)
    for (int i = 0; i < 20; ++i) {
        if (callback_fired) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    REQUIRE(callback_fired == true);
    REQUIRE(received_profile.is_valid == true);
    REQUIRE(received_profile.filament_type == "PLA");
    REQUIRE(received_profile.nozzle_temp_min == 190.0f);
    REQUIRE(received_profile.max_volumetric_speed == 15.0f);
}
