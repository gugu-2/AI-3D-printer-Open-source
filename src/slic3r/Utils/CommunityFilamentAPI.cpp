#include "CommunityFilamentAPI.hpp"

#include <thread>
#include <chrono>

namespace Slic3r {

void CommunityFilamentAPI::fetch_profile_by_nfc(const std::string& nfc_uid, 
                                                std::function<void(const CommunityFilamentProfile&)> callback)
{
    // In a real implementation, this would use libcurl (via Slic3r::Http)
    // to query a REST API, e.g., https://api.quantumforge.net/v1/nfc/profiles?uid=XXXX
    
    // For now, we simulate network latency and return a mock profile
    std::thread([nfc_uid, callback]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        CommunityFilamentProfile profile;
        profile.is_valid = true;
        profile.manufacturer = "Prusament (Mock via NFC)";
        profile.filament_type = "PETG";
        profile.color_hex = "#FF5500";
        profile.nozzle_temp_min = 230.0f;
        profile.nozzle_temp_max = 250.0f;
        profile.bed_temp = 85.0f;
        profile.max_volumetric_speed = 12.0f;
        
        // If the UID looks like PLA, mock a PLA profile
        if (nfc_uid.find("PLA") != std::string::npos) {
            profile.manufacturer = "Polymaker (Mock via NFC)";
            profile.filament_type = "PLA";
            profile.color_hex = "#00FF00";
            profile.nozzle_temp_min = 190.0f;
            profile.nozzle_temp_max = 220.0f;
            profile.bed_temp = 60.0f;
            profile.max_volumetric_speed = 15.0f;
        }

        if (callback) {
            callback(profile);
        }
    }).detach();
}

} // namespace Slic3r
