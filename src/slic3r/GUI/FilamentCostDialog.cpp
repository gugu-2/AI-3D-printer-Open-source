// FilamentCostDialog.cpp
// Filament cost calculator — settings dialog + inline summary panel.

#include "FilamentCostDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"

#include <wx/sizer.h>
#include <wx/listctrl.h>
#include <wx/textdlg.h>
#include <wx/msgdlg.h>
#include <wx/statline.h>

#include <boost/log/trivial.hpp>
#include <sstream>
#include <algorithm>
#include <unordered_map>

namespace Slic3r {
namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// FilamentCostCalculator
// ─────────────────────────────────────────────────────────────────────────────

double FilamentCostCalculator::compute_cost(double weight_g, double cost_per_kg_usd)
{
    return (weight_g / 1000.0) * cost_per_kg_usd;
}

double FilamentCostCalculator::estimate_co2_g(double weight_g,
                                               const std::string& filament_type)
{
    // kg CO2 per kg filament (rough lifecycle estimates)
    static const std::unordered_map<std::string, double> co2_factors = {
        {"PLA",   2.5},
        {"PETG",  3.5},
        {"ABS",   4.0},
        {"ASA",   4.0},
        {"TPU",   3.8},
        {"PA",    5.0},
        {"PC",    5.5},
        {"HIPS",  3.8},
        {"PVA",   3.0},
        {"NYLON", 5.0},
    };

    double factor = 3.0; // default
    for (const auto& kv : co2_factors) {
        // Case-insensitive prefix match
        std::string ft = filament_type;
        std::transform(ft.begin(), ft.end(), ft.begin(), ::toupper);
        if (ft.find(kv.first) != std::string::npos) {
            factor = kv.second;
            break;
        }
    }
    return weight_g * factor; // grams CO2
}

std::vector<FilamentCostEntry> FilamentCostCalculator::load_cost_table()
{
    std::vector<FilamentCostEntry> table;
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg) {
        // Return sensible defaults
        table = {
            {"PLA",   25.0},
            {"PETG",  28.0},
            {"ABS",   22.0},
            {"TPU",   35.0},
            {"PA",    45.0},
        };
        return table;
    }

    // Stored as "filament_cost_<TYPE>" = "<value>"
    static const std::vector<std::string> default_types = {
        "PLA", "PETG", "ABS", "ASA", "TPU", "PA", "PC", "HIPS", "PVA", "NYLON"
    };
    static const std::unordered_map<std::string, double> defaults = {
        {"PLA", 25.0}, {"PETG", 28.0}, {"ABS", 22.0}, {"ASA", 24.0},
        {"TPU", 35.0}, {"PA",   45.0}, {"PC",  40.0}, {"HIPS", 20.0},
        {"PVA", 50.0}, {"NYLON", 45.0}
    };

    for (const auto& t : default_types) {
        std::string key = "filament_cost_" + t;
        std::string val = cfg->get("app", key);
        double cost = defaults.count(t) ? defaults.at(t) : 25.0;
        if (!val.empty()) {
            try { cost = std::stod(val); } catch (...) {}
        }
        table.push_back({t, cost});
    }
    return table;
}

void FilamentCostCalculator::save_cost_table(const std::vector<FilamentCostEntry>& table)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg) return;
    for (const auto& e : table) {
        std::string key = "filament_cost_" + e.filament_type;
        cfg->set("app", key, std::to_string(e.cost_per_kg));
    }
    cfg->save();
}

double FilamentCostCalculator::cost_per_kg_for(const std::string& filament_type)
{
    auto table = load_cost_table();
    std::string ft = filament_type;
    std::transform(ft.begin(), ft.end(), ft.begin(), ::toupper);
    for (const auto& e : table) {
        std::string et = e.filament_type;
        std::transform(et.begin(), et.end(), et.begin(), ::toupper);
        if (ft.find(et) != std::string::npos || et.find(ft) != std::string::npos)
            return e.cost_per_kg;
    }
    return 25.0; // default
}

// ─────────────────────────────────────────────────────────────────────────────
// FilamentCostDialog
// ─────────────────────────────────────────────────────────────────────────────

FilamentCostDialog::FilamentCostDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Filament Cost Settings"),
                wxDefaultPosition, wxSize(FromDIP(480), FromDIP(400)),
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    m_entries = FilamentCostCalculator::load_cost_table();
    build_ui();
    populate_table();
    Centre();
}

void FilamentCostDialog::build_ui()
{
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    // Description
    auto* desc = new wxStaticText(this, wxID_ANY,
        _L("Set the cost per kilogram for each filament type.\n"
           "These values are used to estimate print costs after slicing."));
    main_sizer->Add(desc, 0, wxALL, FromDIP(10));
    main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    // List
    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxLC_REPORT | wxLC_EDIT_LABELS | wxBORDER_SIMPLE);
    m_list->InsertColumn(0, _L("Filament Type"), wxLIST_FORMAT_LEFT, FromDIP(160));
    m_list->InsertColumn(1, _L("Cost per kg ($)"), wxLIST_FORMAT_RIGHT, FromDIP(140));
    main_sizer->Add(m_list, 1, wxEXPAND | wxALL, FromDIP(8));

    // Row buttons
    auto* row_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_add    = new wxButton(this, wxID_ANY, _L("+ Add"));
    m_btn_remove = new wxButton(this, wxID_ANY, _L("- Remove"));
    row_btn_sizer->Add(m_btn_add,    0, wxRIGHT, FromDIP(6));
    row_btn_sizer->Add(m_btn_remove, 0);
    main_sizer->Add(row_btn_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    // OK/Cancel
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_save   = new wxButton(this, wxID_OK,     _L("Save"));
    m_btn_cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(m_btn_save,   0, wxRIGHT, FromDIP(6));
    btn_sizer->Add(m_btn_cancel, 0);
    main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, FromDIP(8));

    SetSizer(main_sizer);

    m_btn_save->Bind(wxEVT_BUTTON,   &FilamentCostDialog::on_save,       this);
    m_btn_add->Bind(wxEVT_BUTTON,    &FilamentCostDialog::on_add_row,    this);
    m_btn_remove->Bind(wxEVT_BUTTON, &FilamentCostDialog::on_remove_row, this);
}

void FilamentCostDialog::populate_table()
{
    m_list->DeleteAllItems();
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        m_list->InsertItem(i, m_entries[i].filament_type);
        m_list->SetItem(i, 1, wxString::Format("%.2f", m_entries[i].cost_per_kg));
    }
}

void FilamentCostDialog::on_save(wxCommandEvent&)
{
    // Read back edited values from list
    m_entries.clear();
    for (int i = 0; i < m_list->GetItemCount(); ++i) {
        FilamentCostEntry e;
        e.filament_type = m_list->GetItemText(i, 0).ToStdString();
        wxString cost_str = m_list->GetItemText(i, 1);
        try { e.cost_per_kg = std::stod(cost_str.ToStdString()); }
        catch (...) { e.cost_per_kg = 25.0; }
        if (!e.filament_type.empty())
            m_entries.push_back(e);
    }
    FilamentCostCalculator::save_cost_table(m_entries);
    EndModal(wxID_OK);
}

void FilamentCostDialog::on_add_row(wxCommandEvent&)
{
    wxTextEntryDialog type_dlg(this, _L("Filament type (e.g. PLA):"),
                                _L("Add Filament Type"), "");
    if (type_dlg.ShowModal() != wxID_OK) return;
    wxString type = type_dlg.GetValue().Trim();
    if (type.IsEmpty()) return;

    wxTextEntryDialog cost_dlg(this, _L("Cost per kg ($):"),
                                _L("Add Filament Cost"), "25.00");
    if (cost_dlg.ShowModal() != wxID_OK) return;

    double cost = 25.0;
    try { cost = std::stod(cost_dlg.GetValue().ToStdString()); } catch (...) {}

    int idx = m_list->GetItemCount();
    m_list->InsertItem(idx, type);
    m_list->SetItem(idx, 1, wxString::Format("%.2f", cost));
}

void FilamentCostDialog::on_remove_row(wxCommandEvent&)
{
    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel == wxNOT_FOUND) return;
    m_list->DeleteItem(sel);
}

void FilamentCostDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Fit();
    Refresh();
}

// ─────────────────────────────────────────────────────────────────────────────
// FilamentCostSummaryPanel
// ─────────────────────────────────────────────────────────────────────────────

FilamentCostSummaryPanel::FilamentCostSummaryPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);

    auto make = [&](const wxString& label) -> wxStaticText* {
        auto* lbl = new wxStaticText(this, wxID_ANY, label);
        sizer->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
        return lbl;
    };

    m_lbl_weight = make(_L("Weight: — g"));
    m_lbl_cost   = make(_L("Cost: $—"));
    m_lbl_co2    = make(_L("CO₂: — g"));

    SetSizer(sizer);
}

void FilamentCostSummaryPanel::update(double weight_g,
                                       const std::string& filament_type,
                                       const std::string& /*filament_color*/)
{
    double cost_per_kg = FilamentCostCalculator::cost_per_kg_for(filament_type);
    double cost        = FilamentCostCalculator::compute_cost(weight_g, cost_per_kg);
    double co2         = FilamentCostCalculator::estimate_co2_g(weight_g, filament_type);

    m_lbl_weight->SetLabel(wxString::Format(_L("Weight: %.1f g"), weight_g));
    m_lbl_cost->SetLabel(wxString::Format(_L("Cost: $%.2f"), cost));
    m_lbl_co2->SetLabel(wxString::Format(_L("CO\u2082: %.0f g"), co2));

    Layout();
    GetParent()->Layout();
}

} // namespace GUI
} // namespace Slic3r
