// QuickSettingsPresets.cpp
// One-click quality preset application.

#include "QuickSettingsPresets.hpp"
#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

QuickSettingsPresetsManager& QuickSettingsPresetsManager::instance()
{
    static QuickSettingsPresetsManager s;
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Level metadata
// ─────────────────────────────────────────────────────────────────────────────

std::string QuickSettingsPresetsManager::level_name(QuickQualityLevel level)
{
    switch (level) {
    case QuickQualityLevel::Draft:     return "Draft (0.3 mm)";
    case QuickQualityLevel::Standard:  return "Standard (0.2 mm)";
    case QuickQualityLevel::Fine:      return "Fine (0.15 mm)";
    case QuickQualityLevel::UltraFine: return "Ultra Fine (0.08 mm)";
    case QuickQualityLevel::Custom:    return "Custom";
    default:                           return "Unknown";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Override tables
// Each entry is a key/value pair that maps directly to PrintConfig option keys.
// Values are serialized as strings (same format as config files).
// ─────────────────────────────────────────────────────────────────────────────

std::vector<QuickSettingOverride> QuickSettingsPresetsManager::overrides_for(
    QuickQualityLevel level)
{
    switch (level) {

    case QuickQualityLevel::Draft:
        return {
            // Layer height
            {"layer_height",                    "0.3"},
            {"initial_layer_print_height",      "0.35"},
            // Speeds (mm/s)
            {"outer_wall_speed",                "80"},
            {"inner_wall_speed",                "120"},
            {"sparse_infill_speed",             "150"},
            {"top_surface_speed",               "60"},
            {"initial_layer_speed",             "30"},
            // Infill
            {"sparse_infill_density",           "15%"},
            {"sparse_infill_pattern",           "grid"},
            // Walls
            {"wall_loops",                      "2"},
            // Top/bottom
            {"top_shell_layers",                "3"},
            {"bottom_shell_layers",             "3"},
            // Supports
            {"enable_support",                  "0"},
        };

    case QuickQualityLevel::Standard:
        return {
            {"layer_height",                    "0.2"},
            {"initial_layer_print_height",      "0.25"},
            {"outer_wall_speed",                "60"},
            {"inner_wall_speed",                "90"},
            {"sparse_infill_speed",             "120"},
            {"top_surface_speed",               "50"},
            {"initial_layer_speed",             "25"},
            {"sparse_infill_density",           "15%"},
            {"sparse_infill_pattern",           "grid"},
            {"wall_loops",                      "3"},
            {"top_shell_layers",                "4"},
            {"bottom_shell_layers",             "4"},
        };

    case QuickQualityLevel::Fine:
        return {
            {"layer_height",                    "0.15"},
            {"initial_layer_print_height",      "0.2"},
            {"outer_wall_speed",                "40"},
            {"inner_wall_speed",                "60"},
            {"sparse_infill_speed",             "80"},
            {"top_surface_speed",               "35"},
            {"initial_layer_speed",             "20"},
            {"sparse_infill_density",           "20%"},
            {"sparse_infill_pattern",           "gyroid"},
            {"wall_loops",                      "3"},
            {"top_shell_layers",                "5"},
            {"bottom_shell_layers",             "4"},
            // Pressure advance / smooth
            {"smooth_coefficient",              "0"},
        };

    case QuickQualityLevel::UltraFine:
        return {
            {"layer_height",                    "0.08"},
            {"initial_layer_print_height",      "0.15"},
            {"outer_wall_speed",                "25"},
            {"inner_wall_speed",                "40"},
            {"sparse_infill_speed",             "50"},
            {"top_surface_speed",               "20"},
            {"initial_layer_speed",             "15"},
            {"sparse_infill_density",           "25%"},
            {"sparse_infill_pattern",           "gyroid"},
            {"wall_loops",                      "4"},
            {"top_shell_layers",                "6"},
            {"bottom_shell_layers",             "5"},
            {"smooth_coefficient",              "0"},
            // Ironing for ultra-smooth top surfaces
            {"ironing_type",                    "top_surfaces"},
        };

    default:
        return {};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Apply / Restore
// ─────────────────────────────────────────────────────────────────────────────

bool QuickSettingsPresetsManager::apply_level(QuickQualityLevel level,
                                               DynamicPrintConfig& config)
{
    if (level == QuickQualityLevel::Custom) {
        restore_original(config);
        m_current_level = level;
        return false;
    }

    auto overrides = overrides_for(level);
    if (overrides.empty()) return false;

    // Save originals before first application
    if (m_saved_originals.empty()) {
        for (const auto& ov : overrides) {
            const ConfigOption* opt = config.option(ov.key);
            if (opt) {
                m_saved_originals[ov.key] = opt->serialize();
            }
        }
    }

    bool changed = false;
    for (const auto& ov : overrides) {
        try {
            config.set_deserialize(ov.key, ov.value, ForwardCompatibilitySubstitutionRule::Disable);
            changed = true;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning)
                << "QuickPresets: could not apply " << ov.key
                << " = " << ov.value << ": " << e.what();
        }
    }

    m_current_level = level;
    save_level(level);
    return changed;
}

void QuickSettingsPresetsManager::restore_original(DynamicPrintConfig& config)
{
    for (const auto& kv : m_saved_originals) {
        try {
            config.set_deserialize(kv.first, kv.second,
                                   ForwardCompatibilitySubstitutionRule::Disable);
        } catch (...) {}
    }
    m_saved_originals.clear();
    m_current_level = QuickQualityLevel::Custom;
    save_level(QuickQualityLevel::Custom);
}

// ─────────────────────────────────────────────────────────────────────────────
// Persistence
// ─────────────────────────────────────────────────────────────────────────────

void QuickSettingsPresetsManager::save_level(QuickQualityLevel level)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg) return;
    cfg->set("app", "quick_quality_level", std::to_string(static_cast<int>(level)));
    cfg->save();
}

QuickQualityLevel QuickSettingsPresetsManager::load_level()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg) return QuickQualityLevel::Custom;
    std::string v = cfg->get("app", "quick_quality_level");
    if (v.empty()) return QuickQualityLevel::Custom;
    try {
        int i = std::stoi(v);
        if (i >= 0 && i <= static_cast<int>(QuickQualityLevel::Custom))
            return static_cast<QuickQualityLevel>(i);
    } catch (...) {}
    return QuickQualityLevel::Custom;
}

} // namespace GUI
} // namespace Slic3r
