#pragma once
// QuickSettingsPresets.hpp
// One-click quality presets: Draft / Standard / Fine / Ultra Fine.
// Applies a curated set of print settings overrides without changing the
// underlying saved preset — acts as a temporary "quality mode" layer.

#ifndef slic3r_QuickSettingsPresets_hpp_
#define slic3r_QuickSettingsPresets_hpp_

#include <string>
#include <vector>
#include <map>

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// Quality level enum
// ─────────────────────────────────────────────────────────────────────────────

enum class QuickQualityLevel {
    Draft,      // Fast, low quality — 0.3 mm layers, 60% infill, high speed
    Standard,   // Balanced — 0.2 mm layers, 15% infill, normal speed
    Fine,       // High quality — 0.15 mm layers, 20% infill, reduced speed
    UltraFine,  // Maximum quality — 0.08 mm layers, 25% infill, slow speed
    Custom,     // User-defined (no override applied)
};

// ─────────────────────────────────────────────────────────────────────────────
// A single override entry
// ─────────────────────────────────────────────────────────────────────────────

struct QuickSettingOverride {
    std::string key;
    std::string value;  // serialized as string; parsed by DynamicPrintConfig
};

// ─────────────────────────────────────────────────────────────────────────────
// Manager
// ─────────────────────────────────────────────────────────────────────────────

class QuickSettingsPresetsManager {
public:
    static QuickSettingsPresetsManager& instance();

    // Returns the display name for a level
    static std::string level_name(QuickQualityLevel level);

    // Returns the overrides for a given level
    static std::vector<QuickSettingOverride> overrides_for(QuickQualityLevel level);

    // Apply overrides to a DynamicPrintConfig (non-destructive — saves originals)
    // Returns true if any setting was changed.
    bool apply_level(QuickQualityLevel level, DynamicPrintConfig& config);

    // Restore the config to the state before the last apply_level call
    void restore_original(DynamicPrintConfig& config);

    QuickQualityLevel current_level() const { return m_current_level; }

    // Persist the chosen level in AppConfig
    void save_level(QuickQualityLevel level);
    QuickQualityLevel load_level();

private:
    QuickSettingsPresetsManager() = default;
    QuickQualityLevel m_current_level = QuickQualityLevel::Custom;
    std::map<std::string, std::string> m_saved_originals;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_QuickSettingsPresets_hpp_
