// PrintHistoryDialog.cpp
// Full-featured print history viewer with analytics and CSV export.

#include "PrintHistoryDialog.hpp"
#include "PrintHistory.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"

#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/textdlg.h>

#include <fstream>
#include <sstream>
#include <iomanip>

namespace Slic3r {
namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// PrintHistoryStatsPanel
// ─────────────────────────────────────────────────────────────────────────────

PrintHistoryStatsPanel::PrintHistoryStatsPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxFlexGridSizer(2, 5, FromDIP(8), FromDIP(24));
    sizer->AddGrowableCol(0); sizer->AddGrowableCol(1);
    sizer->AddGrowableCol(2); sizer->AddGrowableCol(3);
    sizer->AddGrowableCol(4);

    auto make_label = [&](const wxString& title) -> wxStaticText* {
        auto* lbl_title = new wxStaticText(this, wxID_ANY, title);
        wxFont f = lbl_title->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        lbl_title->SetFont(f);
        sizer->Add(lbl_title, 0, wxALIGN_CENTER_HORIZONTAL);
        auto* lbl_val = new wxStaticText(this, wxID_ANY, "—",
                                          wxDefaultPosition, wxDefaultSize,
                                          wxALIGN_CENTRE_HORIZONTAL);
        sizer->Add(lbl_val, 0, wxALIGN_CENTER_HORIZONTAL);
        return lbl_val;
    };

    // Row 1: titles
    auto* t1 = new wxStaticText(this, wxID_ANY, _L("Total Prints"));
    auto* t2 = new wxStaticText(this, wxID_ANY, _L("Success Rate"));
    auto* t3 = new wxStaticText(this, wxID_ANY, _L("Filament Used"));
    auto* t4 = new wxStaticText(this, wxID_ANY, _L("Total Cost"));
    auto* t5 = new wxStaticText(this, wxID_ANY, _L("Print Hours"));
    for (auto* t : {t1, t2, t3, t4, t5}) {
        wxFont f = t->GetFont(); f.SetWeight(wxFONTWEIGHT_BOLD); t->SetFont(f);
        sizer->Add(t, 0, wxALIGN_CENTER_HORIZONTAL);
    }

    m_lbl_total_prints   = new wxStaticText(this, wxID_ANY, "0");
    m_lbl_success_rate   = new wxStaticText(this, wxID_ANY, "0%");
    m_lbl_total_filament = new wxStaticText(this, wxID_ANY, "0 g");
    m_lbl_total_cost     = new wxStaticText(this, wxID_ANY, "$0.00");
    m_lbl_total_hours    = new wxStaticText(this, wxID_ANY, "0 h");

    for (auto* l : {m_lbl_total_prints, m_lbl_success_rate,
                    m_lbl_total_filament, m_lbl_total_cost, m_lbl_total_hours}) {
        sizer->Add(l, 0, wxALIGN_CENTER_HORIZONTAL);
    }

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(sizer, 0, wxEXPAND | wxALL, FromDIP(12));
    SetSizer(outer);
}

void PrintHistoryStatsPanel::refresh()
{
    auto& mgr = PrintHistoryManager::instance();
    m_lbl_total_prints->SetLabel(
        wxString::Format("%zu", mgr.records().size()));
    m_lbl_success_rate->SetLabel(
        wxString::Format("%.1f%%", mgr.success_rate_pct()));
    m_lbl_total_filament->SetLabel(
        wxString::Format("%.1f g", mgr.total_filament_used_g()));
    m_lbl_total_cost->SetLabel(
        wxString::Format("$%.2f", mgr.total_filament_cost()));
    m_lbl_total_hours->SetLabel(
        wxString::Format("%.1f h", mgr.total_print_hours()));
    Layout();
}

// ─────────────────────────────────────────────────────────────────────────────
// PrintHistoryDialog
// ─────────────────────────────────────────────────────────────────────────────

PrintHistoryDialog::PrintHistoryDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Print History"),
                wxDefaultPosition, wxSize(FromDIP(900), FromDIP(600)),
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    build_ui();
    populate_list();
    Centre();
}

void PrintHistoryDialog::build_ui()
{
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    // ── Stats panel ──────────────────────────────────────────────────────────
    m_stats_panel = new PrintHistoryStatsPanel(this);
    main_sizer->Add(m_stats_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    // ── Search bar ───────────────────────────────────────────────────────────
    auto* search_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_search = new wxSearchCtrl(this, wxID_ANY, wxEmptyString,
                                 wxDefaultPosition, wxSize(FromDIP(280), -1));
    m_search->SetDescriptiveText(_L("Search by project or printer…"));
    search_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Filter:")),
                      0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    search_sizer->Add(m_search, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(search_sizer, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    // ── List control ─────────────────────────────────────────────────────────
    m_list = new wxDataViewListCtrl(this, wxID_ANY,
                                     wxDefaultPosition, wxDefaultSize,
                                     wxDV_ROW_LINES | wxDV_FULL_ROW_ON_ACTIVATE);

    m_list->AppendTextColumn(_L("Date"),         wxDATAVIEW_CELL_INERT, FromDIP(130));
    m_list->AppendTextColumn(_L("Project"),      wxDATAVIEW_CELL_INERT, FromDIP(160));
    m_list->AppendTextColumn(_L("Printer"),      wxDATAVIEW_CELL_INERT, FromDIP(120));
    m_list->AppendTextColumn(_L("Filament"),     wxDATAVIEW_CELL_INERT, FromDIP(80));
    m_list->AppendTextColumn(_L("Used (g)"),     wxDATAVIEW_CELL_INERT, FromDIP(70));
    m_list->AppendTextColumn(_L("Cost"),         wxDATAVIEW_CELL_INERT, FromDIP(70));
    m_list->AppendTextColumn(_L("Duration"),     wxDATAVIEW_CELL_INERT, FromDIP(80));
    m_list->AppendTextColumn(_L("Layers"),       wxDATAVIEW_CELL_INERT, FromDIP(60));
    m_list->AppendTextColumn(_L("Result"),       wxDATAVIEW_CELL_INERT, FromDIP(80));

    main_sizer->Add(m_list, 1, wxEXPAND | wxALL, FromDIP(8));

    // ── Buttons ──────────────────────────────────────────────────────────────
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_delete  = new wxButton(this, wxID_ANY, _L("Delete Selected"));
    m_btn_clear   = new wxButton(this, wxID_ANY, _L("Clear All"));
    m_btn_export  = new wxButton(this, wxID_ANY, _L("Export CSV"));
    m_btn_close   = new wxButton(this, wxID_CANCEL, _L("Close"));

    btn_sizer->Add(m_btn_delete,  0, wxRIGHT, FromDIP(6));
    btn_sizer->Add(m_btn_clear,   0, wxRIGHT, FromDIP(6));
    btn_sizer->Add(m_btn_export,  0, wxRIGHT, FromDIP(6));
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(m_btn_close,   0);
    main_sizer->Add(btn_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    SetSizer(main_sizer);

    // ── Events ───────────────────────────────────────────────────────────────
    m_btn_delete->Bind(wxEVT_BUTTON, &PrintHistoryDialog::on_delete_selected, this);
    m_btn_clear->Bind(wxEVT_BUTTON,  &PrintHistoryDialog::on_clear_all,       this);
    m_btn_export->Bind(wxEVT_BUTTON, &PrintHistoryDialog::on_export_csv,      this);
    m_search->Bind(wxEVT_SEARCH,     &PrintHistoryDialog::on_search,          this);
    m_search->Bind(wxEVT_TEXT,       &PrintHistoryDialog::on_search,          this);
    m_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,
                 &PrintHistoryDialog::on_item_activated, this);
}

void PrintHistoryDialog::populate_list(const std::string& filter)
{
    m_list->DeleteAllItems();
    const auto& records = PrintHistoryManager::instance().records();

    // Newest first
    for (int i = static_cast<int>(records.size()) - 1; i >= 0; --i) {
        const auto& r = records[i];

        // Apply filter
        if (!filter.empty()) {
            std::string haystack = r.project_name + " " + r.printer_name;
            std::string needle   = filter;
            // Case-insensitive
            std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
            std::transform(needle.begin(),   needle.end(),   needle.begin(),   ::tolower);
            if (haystack.find(needle) == std::string::npos) continue;
        }

        wxVector<wxVariant> row;
        row.push_back(wxVariant(r.start_time_str()));
        row.push_back(wxVariant(r.project_name));
        row.push_back(wxVariant(r.printer_name));
        row.push_back(wxVariant(r.filament_type));
        row.push_back(wxVariant(wxString::Format("%.1f", r.filament_used_g)));
        row.push_back(wxVariant(wxString::Format("$%.2f", r.filament_cost)));

        // Duration
        double mins = r.duration_minutes();
        wxString dur;
        if (mins >= 60.0)
            dur = wxString::Format("%.1f h", mins / 60.0);
        else
            dur = wxString::Format("%.0f min", mins);
        row.push_back(wxVariant(dur));

        row.push_back(wxVariant(wxString::Format("%d", r.total_layers)));
        row.push_back(wxVariant(r.outcome_str()));
        m_list->AppendItem(row);
    }

    m_stats_panel->refresh();
}

void PrintHistoryDialog::on_delete_selected(wxCommandEvent&)
{
    int row = m_list->GetSelectedRow();
    if (row == wxNOT_FOUND) {
        wxMessageBox(_L("Please select a record to delete."),
                     _L("No Selection"), wxOK | wxICON_INFORMATION, this);
        return;
    }

    // Map visible row back to record id (newest-first order)
    const auto& records = PrintHistoryManager::instance().records();
    int actual_idx = static_cast<int>(records.size()) - 1 - row;
    if (actual_idx < 0 || actual_idx >= static_cast<int>(records.size())) return;

    std::string id = records[actual_idx].id;
    PrintHistoryManager::instance().delete_record(id);
    populate_list(m_search->GetValue().ToStdString());
}

void PrintHistoryDialog::on_clear_all(wxCommandEvent&)
{
    int answer = wxMessageBox(
        _L("Are you sure you want to delete all print history records? This cannot be undone."),
        _L("Clear All History"),
        wxYES_NO | wxICON_WARNING, this);
    if (answer != wxYES) return;

    PrintHistoryManager::instance().clear_all();
    populate_list();
}

void PrintHistoryDialog::on_search(wxCommandEvent&)
{
    populate_list(m_search->GetValue().ToStdString());
}

void PrintHistoryDialog::on_export_csv(wxCommandEvent&)
{
    wxFileDialog dlg(this, _L("Export Print History as CSV"),
                     wxEmptyString, "print_history.csv",
                     "CSV files (*.csv)|*.csv",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;

    std::ofstream f(dlg.GetPath().ToStdString());
    if (!f.is_open()) {
        wxMessageBox(_L("Could not open file for writing."),
                     _L("Export Error"), wxOK | wxICON_ERROR, this);
        return;
    }

    f << "Date,Project,Printer,Filament Type,Filament Color,"
         "Used (g),Cost ($),Duration (min),Layer Height (mm),Layers,Result,Notes\n";

    for (const auto& r : PrintHistoryManager::instance().records()) {
        f << std::quoted(r.start_time_str()) << ","
          << std::quoted(r.project_name)     << ","
          << std::quoted(r.printer_name)     << ","
          << std::quoted(r.filament_type)    << ","
          << std::quoted(r.filament_color)   << ","
          << std::fixed << std::setprecision(2) << r.filament_used_g << ","
          << r.filament_cost                 << ","
          << r.duration_minutes()            << ","
          << r.layer_height_mm               << ","
          << r.total_layers                  << ","
          << std::quoted(r.outcome_str())    << ","
          << std::quoted(r.notes)            << "\n";
    }

    wxMessageBox(wxString::Format(_L("Exported %zu records to CSV."),
                                  PrintHistoryManager::instance().records().size()),
                 _L("Export Complete"), wxOK | wxICON_INFORMATION, this);
}

void PrintHistoryDialog::on_item_activated(wxDataViewEvent& evt)
{
    // Show detail popup for the activated row
    int row = m_list->GetSelectedRow();
    if (row == wxNOT_FOUND) return;

    const auto& records = PrintHistoryManager::instance().records();
    int actual_idx = static_cast<int>(records.size()) - 1 - row;
    if (actual_idx < 0 || actual_idx >= static_cast<int>(records.size())) return;

    const auto& r = records[actual_idx];
    wxString detail = wxString::Format(
        _L("Project: %s\n"
           "Printer: %s\n"
           "Date: %s\n"
           "Filament: %s (%s)\n"
           "Used: %.1f g  |  Cost: $%.2f\n"
           "Duration: %.1f min\n"
           "Layer height: %.2f mm  |  Layers: %d\n"
           "Result: %s\n"
           "Notes: %s"),
        r.project_name, r.printer_name,
        r.start_time_str(),
        r.filament_type, r.filament_color,
        r.filament_used_g, r.filament_cost,
        r.duration_minutes(),
        r.layer_height_mm, r.total_layers,
        r.outcome_str(),
        r.notes.empty() ? "—" : r.notes);

    wxMessageBox(detail, _L("Print Record Detail"), wxOK | wxICON_INFORMATION, this);
}

void PrintHistoryDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
