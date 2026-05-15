#pragma once
// FilamentCostDialog.hpp
// Dialog for configuring filament cost per kg and viewing per-print cost estimates.
// Also provides a static helper to compute cost from slice data.

#ifndef slic3r_FilamentCostDialog_hpp_
#define slic3r_FilamentCostDialog_hpp_

#include "GUI_Utils.hpp"
#include <wx/wx.h>
#include <wx/spinctrl.h>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// Cost computation helpers (static, no UI dependency)
// ─────────────────────────────────────────────────────────────────────────────

struct FilamentCostEntry {
    std::string filament_type;   // e.g. "PLA", "PETG", "ABS"
    double      cost_per_kg;     // user currency per kg
};

class FilamentCostCalculator {
public:
    // Compute cost given weight in grams and cost per kg
    static double compute_cost(double weight_g, double cost_per_kg_usd);

    // Estimate CO2 equivalent in grams (rough industry average)
    // PLA ~2.5 kg CO2/kg, PETG ~3.5, ABS ~4.0, TPU ~3.8
    static double estimate_co2_g(double weight_g, const std::string& filament_type);

    // Load/save per-type cost table from app config
    static std::vector<FilamentCostEntry> load_cost_table();
    static void save_cost_table(const std::vector<FilamentCostEntry>& table);

    // Look up cost for a given filament type (returns default 25.0 if not found)
    static double cost_per_kg_for(const std::string& filament_type);
};

// ─────────────────────────────────────────────────────────────────────────────
// Settings dialog
// ─────────────────────────────────────────────────────────────────────────────

class FilamentCostDialog : public DPIDialog {
public:
    explicit FilamentCostDialog(wxWindow* parent);
    ~FilamentCostDialog() override = default;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build_ui();
    void populate_table();
    void on_save(wxCommandEvent&);
    void on_add_row(wxCommandEvent&);
    void on_remove_row(wxCommandEvent&);

    wxListCtrl*  m_list   { nullptr };
    wxButton*    m_btn_add    { nullptr };
    wxButton*    m_btn_remove { nullptr };
    wxButton*    m_btn_save   { nullptr };
    wxButton*    m_btn_cancel { nullptr };

    std::vector<FilamentCostEntry> m_entries;
};

// ─────────────────────────────────────────────────────────────────────────────
// Inline cost summary panel (embedded in slice info area)
// ─────────────────────────────────────────────────────────────────────────────

class FilamentCostSummaryPanel : public wxPanel {
public:
    explicit FilamentCostSummaryPanel(wxWindow* parent);

    // Call after slicing completes with fresh data
    void update(double weight_g,
                const std::string& filament_type,
                const std::string& filament_color);

private:
    wxStaticText* m_lbl_weight  { nullptr };
    wxStaticText* m_lbl_cost    { nullptr };
    wxStaticText* m_lbl_co2     { nullptr };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_FilamentCostDialog_hpp_
