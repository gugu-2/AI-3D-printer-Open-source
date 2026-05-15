#pragma once
// PrintHistoryDialog.hpp
// Full-featured dialog showing print history table + analytics summary.

#ifndef slic3r_PrintHistoryDialog_hpp_
#define slic3r_PrintHistoryDialog_hpp_

#include "GUI_Utils.hpp"
#include "PrintHistory.hpp"

#include <wx/wx.h>
#include <wx/dataview.h>
#include <wx/statline.h>
#include <wx/srchctrl.h>

namespace Slic3r {
namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// Analytics summary panel (top of dialog)
// ─────────────────────────────────────────────────────────────────────────────

class PrintHistoryStatsPanel : public wxPanel {
public:
    explicit PrintHistoryStatsPanel(wxWindow* parent);
    void refresh();

private:
    wxStaticText* m_lbl_total_prints   { nullptr };
    wxStaticText* m_lbl_success_rate   { nullptr };
    wxStaticText* m_lbl_total_filament { nullptr };
    wxStaticText* m_lbl_total_cost     { nullptr };
    wxStaticText* m_lbl_total_hours    { nullptr };
};

// ─────────────────────────────────────────────────────────────────────────────
// Main dialog
// ─────────────────────────────────────────────────────────────────────────────

class PrintHistoryDialog : public DPIDialog {
public:
    explicit PrintHistoryDialog(wxWindow* parent);
    ~PrintHistoryDialog() override = default;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build_ui();
    void populate_list(const std::string& filter = "");
    void on_delete_selected(wxCommandEvent&);
    void on_clear_all(wxCommandEvent&);
    void on_search(wxCommandEvent&);
    void on_export_csv(wxCommandEvent&);
    void on_item_activated(wxDataViewEvent&);

    PrintHistoryStatsPanel* m_stats_panel  { nullptr };
    wxDataViewListCtrl*     m_list         { nullptr };
    wxSearchCtrl*           m_search       { nullptr };
    wxButton*               m_btn_delete   { nullptr };
    wxButton*               m_btn_clear    { nullptr };
    wxButton*               m_btn_export   { nullptr };
    wxButton*               m_btn_close    { nullptr };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_PrintHistoryDialog_hpp_
