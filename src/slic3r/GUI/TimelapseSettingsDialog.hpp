#pragma once
// TimelapseSettingsDialog.hpp
// Configure automatic time-lapse G-code injection at layer changes.
// The injected G-code parks the nozzle, triggers a camera shutter command,
// then resumes — producing a clean per-layer time-lapse without manual setup.

#ifndef slic3r_TimelapseSettingsDialog_hpp_
#define slic3r_TimelapseSettingsDialog_hpp_

#include "GUI_Utils.hpp"
#include <wx/wx.h>
#include <wx/spinctrl.h>
#include <string>

namespace Slic3r {
namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// Settings data (persisted in AppConfig)
// ─────────────────────────────────────────────────────────────────────────────

struct TimelapseInjectorSettings {
    bool        enabled             = false;
    std::string park_gcode;         // G-code to park nozzle before shot
    std::string trigger_gcode;      // G-code to trigger camera (e.g. M240)
    std::string resume_gcode;       // G-code after shot (optional)
    double      park_x             = 0.0;   // mm
    double      park_y             = 0.0;   // mm
    double      park_z_lift        = 2.0;   // mm lift before parking
    double      dwell_ms           = 500.0; // dwell after trigger (ms)
    bool        retract_before_park = true;
    int         every_n_layers     = 1;     // inject every N layers

    static TimelapseInjectorSettings load();
    void save() const;

    // Build the complete injection G-code block for a given layer
    std::string build_injection_gcode(int layer_num,
                                       double current_z,
                                       double retract_length_mm,
                                       double retract_speed_mm_s) const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Settings dialog
// ─────────────────────────────────────────────────────────────────────────────

class TimelapseSettingsDialog : public DPIDialog {
public:
    explicit TimelapseSettingsDialog(wxWindow* parent);
    ~TimelapseSettingsDialog() override = default;

    TimelapseInjectorSettings get_settings() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build_ui();
    void on_save(wxCommandEvent&);
    void on_toggle_enabled(wxCommandEvent&);
    void on_preset_selected(wxCommandEvent&);
    void update_controls_state();

    wxCheckBox*   m_chk_enabled       { nullptr };
    wxTextCtrl*   m_txt_park_gcode    { nullptr };
    wxTextCtrl*   m_txt_trigger_gcode { nullptr };
    wxTextCtrl*   m_txt_resume_gcode  { nullptr };
    wxSpinCtrlDouble* m_spin_park_x   { nullptr };
    wxSpinCtrlDouble* m_spin_park_y   { nullptr };
    wxSpinCtrlDouble* m_spin_z_lift   { nullptr };
    wxSpinCtrlDouble* m_spin_dwell    { nullptr };
    wxCheckBox*   m_chk_retract       { nullptr };
    wxSpinCtrl*   m_spin_every_n      { nullptr };
    wxChoice*     m_choice_preset     { nullptr };
    wxButton*     m_btn_save          { nullptr };
    wxButton*     m_btn_cancel        { nullptr };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_TimelapseSettingsDialog_hpp_
