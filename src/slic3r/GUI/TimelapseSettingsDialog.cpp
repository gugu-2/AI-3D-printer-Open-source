// TimelapseSettingsDialog.cpp
// Time-lapse G-code injector — settings dialog and G-code builder.

#include "TimelapseSettingsDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"

#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/statbox.h>
#include <wx/msgdlg.h>
#include <wx/tooltip.h>

#include <sstream>
#include <iomanip>

namespace Slic3r {
namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// TimelapseInjectorSettings — load / save / build
// ─────────────────────────────────────────────────────────────────────────────

TimelapseInjectorSettings TimelapseInjectorSettings::load()
{
    TimelapseInjectorSettings s;
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg) return s;

    auto get_d = [&](const std::string& key, double def) -> double {
        std::string v = cfg->get("timelapse", key);
        if (v.empty()) return def;
        try { return std::stod(v); } catch (...) { return def; }
    };
    auto get_i = [&](const std::string& key, int def) -> int {
        std::string v = cfg->get("timelapse", key);
        if (v.empty()) return def;
        try { return std::stoi(v); } catch (...) { return def; }
    };
    auto get_b = [&](const std::string& key, bool def) -> bool {
        std::string v = cfg->get("timelapse", key);
        if (v.empty()) return def;
        return v == "1" || v == "true";
    };
    auto get_s = [&](const std::string& key, const std::string& def) -> std::string {
        std::string v = cfg->get("timelapse", key);
        return v.empty() ? def : v;
    };

    s.enabled              = get_b("enabled",              false);
    s.park_x               = get_d("park_x",               0.0);
    s.park_y               = get_d("park_y",               0.0);
    s.park_z_lift          = get_d("park_z_lift",          2.0);
    s.dwell_ms             = get_d("dwell_ms",             500.0);
    s.retract_before_park  = get_b("retract_before_park",  true);
    s.every_n_layers       = get_i("every_n_layers",       1);
    s.park_gcode           = get_s("park_gcode",           "G1 X{park_x} Y{park_y} F9000");
    s.trigger_gcode        = get_s("trigger_gcode",        "M240");
    s.resume_gcode         = get_s("resume_gcode",         "");

    return s;
}

void TimelapseInjectorSettings::save() const
{
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg) return;

    cfg->set("timelapse", "enabled",             enabled ? "1" : "0");
    cfg->set("timelapse", "park_x",              std::to_string(park_x));
    cfg->set("timelapse", "park_y",              std::to_string(park_y));
    cfg->set("timelapse", "park_z_lift",         std::to_string(park_z_lift));
    cfg->set("timelapse", "dwell_ms",            std::to_string(dwell_ms));
    cfg->set("timelapse", "retract_before_park", retract_before_park ? "1" : "0");
    cfg->set("timelapse", "every_n_layers",      std::to_string(every_n_layers));
    cfg->set("timelapse", "park_gcode",          park_gcode);
    cfg->set("timelapse", "trigger_gcode",       trigger_gcode);
    cfg->set("timelapse", "resume_gcode",        resume_gcode);
    cfg->save();
}

std::string TimelapseInjectorSettings::build_injection_gcode(
    int    layer_num,
    double current_z,
    double retract_length_mm,
    double retract_speed_mm_s) const
{
    if (!enabled) return "";
    if (every_n_layers > 1 && (layer_num % every_n_layers) != 0) return "";

    std::ostringstream gcode;
    gcode << std::fixed << std::setprecision(3);

    gcode << "; === Timelapse injection — layer " << layer_num << " ===\n";
    gcode << "SAVE_GCODE_STATE NAME=TIMELAPSE_STATE\n";

    // Retract
    if (retract_before_park && retract_length_mm > 0.0) {
        gcode << "G1 E-" << retract_length_mm
              << " F" << (retract_speed_mm_s * 60.0) << " ; retract\n";
    }

    // Z lift
    if (park_z_lift > 0.0) {
        gcode << "G1 Z" << (current_z + park_z_lift) << " F3000 ; z-lift\n";
    }

    // Park — substitute {park_x} / {park_y} placeholders
    std::string pg = park_gcode;
    auto replace_all = [](std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    std::ostringstream px, py;
    px << std::fixed << std::setprecision(3) << park_x;
    py << std::fixed << std::setprecision(3) << park_y;
    replace_all(pg, "{park_x}", px.str());
    replace_all(pg, "{park_y}", py.str());
    gcode << pg << " ; park nozzle\n";

    // Dwell
    if (dwell_ms > 0.0) {
        gcode << "G4 P" << static_cast<int>(dwell_ms) << " ; dwell for camera\n";
    }

    // Trigger
    if (!trigger_gcode.empty()) {
        gcode << trigger_gcode << " ; camera trigger\n";
    }

    // Post-trigger dwell
    gcode << "G4 P200 ; settle\n";

    // Resume G-code
    if (!resume_gcode.empty()) {
        gcode << resume_gcode << "\n";
    }

    gcode << "RESTORE_GCODE_STATE NAME=TIMELAPSE_STATE MOVE=1\n";
    gcode << "; === End timelapse injection ===\n";

    return gcode.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// TimelapseSettingsDialog
// ─────────────────────────────────────────────────────────────────────────────

TimelapseSettingsDialog::TimelapseSettingsDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Time-lapse G-code Injector"),
                wxDefaultPosition, wxSize(FromDIP(560), FromDIP(580)),
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    build_ui();
    Centre();
}

void TimelapseSettingsDialog::build_ui()
{
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    // ── Enable toggle ────────────────────────────────────────────────────────
    m_chk_enabled = new wxCheckBox(this, wxID_ANY,
        _L("Enable automatic time-lapse G-code injection"));
    main_sizer->Add(m_chk_enabled, 0, wxALL, FromDIP(10));
    main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    // ── Preset selector ──────────────────────────────────────────────────────
    auto* preset_sizer = new wxBoxSizer(wxHORIZONTAL);
    preset_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Quick preset:")),
                      0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    wxArrayString presets;
    presets.Add(_L("Custom"));
    presets.Add(_L("Klipper (M240 + park)"));
    presets.Add(_L("Marlin (M240)"));
    presets.Add(_L("OctoPrint (@ timelapse_takephoto)"));
    presets.Add(_L("Bambu Lab (built-in timelapse)"));
    m_choice_preset = new wxChoice(this, wxID_ANY, wxDefaultPosition,
                                    wxSize(FromDIP(280), -1), presets);
    m_choice_preset->SetSelection(0);
    preset_sizer->Add(m_choice_preset, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(preset_sizer, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    // ── Park position ────────────────────────────────────────────────────────
    auto* pos_box = new wxStaticBoxSizer(wxHORIZONTAL, this, _L("Park Position"));
    auto add_spin_d = [&](wxStaticBoxSizer* sizer, const wxString& label,
                           wxSpinCtrlDouble** out, double val, double min, double max) {
        sizer->Add(new wxStaticText(this, wxID_ANY, label),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        *out = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
                                    wxDefaultPosition, wxSize(FromDIP(80), -1),
                                    wxSP_ARROW_KEYS, min, max, val, 1.0);
        sizer->Add(*out, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    };
    add_spin_d(pos_box, "X:", &m_spin_park_x, 0.0, -300.0, 300.0);
    add_spin_d(pos_box, "Y:", &m_spin_park_y, 0.0, -300.0, 300.0);
    add_spin_d(pos_box, _L("Z lift:"), &m_spin_z_lift, 2.0, 0.0, 20.0);
    main_sizer->Add(pos_box, 0, wxEXPAND | wxALL, FromDIP(8));

    // ── Timing ───────────────────────────────────────────────────────────────
    auto* timing_box = new wxStaticBoxSizer(wxHORIZONTAL, this, _L("Timing"));
    timing_box->Add(new wxStaticText(this, wxID_ANY, _L("Dwell (ms):")),
                    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_spin_dwell = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
                                         wxDefaultPosition, wxSize(FromDIP(90), -1),
                                         wxSP_ARROW_KEYS, 0.0, 5000.0, 500.0, 50.0);
    timing_box->Add(m_spin_dwell, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
    timing_box->Add(new wxStaticText(this, wxID_ANY, _L("Every N layers:")),
                    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_spin_every_n = new wxSpinCtrl(this, wxID_ANY, "1",
                                     wxDefaultPosition, wxSize(FromDIP(70), -1),
                                     wxSP_ARROW_KEYS, 1, 100, 1);
    timing_box->Add(m_spin_every_n, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(timing_box, 0, wxEXPAND | wxALL, FromDIP(8));

    // ── Retract ──────────────────────────────────────────────────────────────
    m_chk_retract = new wxCheckBox(this, wxID_ANY,
        _L("Retract filament before parking"));
    m_chk_retract->SetValue(true);
    main_sizer->Add(m_chk_retract, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    // ── G-code fields ────────────────────────────────────────────────────────
    auto add_gcode_field = [&](const wxString& label, wxTextCtrl** out,
                                const wxString& default_val,
                                const wxString& tooltip) {
        main_sizer->Add(new wxStaticText(this, wxID_ANY, label),
                        0, wxLEFT | wxTOP, FromDIP(8));
        *out = new wxTextCtrl(this, wxID_ANY, default_val,
                               wxDefaultPosition, wxSize(-1, FromDIP(40)),
                               wxTE_MULTILINE);
        (*out)->SetToolTip(tooltip);
        main_sizer->Add(*out, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    };

    add_gcode_field(_L("Park G-code ({park_x}, {park_y} are substituted):"),
                    &m_txt_park_gcode,
                    "G1 X{park_x} Y{park_y} F9000",
                    _L("G-code to move nozzle to park position. Use {park_x} and {park_y} as placeholders."));

    add_gcode_field(_L("Camera trigger G-code:"),
                    &m_txt_trigger_gcode,
                    "M240",
                    _L("G-code command to trigger the camera shutter. M240 is standard; use @ timelapse_takephoto for OctoPrint."));

    add_gcode_field(_L("Resume G-code (optional):"),
                    &m_txt_resume_gcode,
                    "",
                    _L("Optional G-code to run after the photo is taken, before resuming print."));

    main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    // ── Buttons ──────────────────────────────────────────────────────────────
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_save   = new wxButton(this, wxID_OK,     _L("Save"));
    m_btn_cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(m_btn_save,   0, wxRIGHT, FromDIP(6));
    btn_sizer->Add(m_btn_cancel, 0);
    main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, FromDIP(8));

    SetSizer(main_sizer);

    // Load saved settings
    auto s = TimelapseInjectorSettings::load();
    m_chk_enabled->SetValue(s.enabled);
    m_spin_park_x->SetValue(s.park_x);
    m_spin_park_y->SetValue(s.park_y);
    m_spin_z_lift->SetValue(s.park_z_lift);
    m_spin_dwell->SetValue(s.dwell_ms);
    m_spin_every_n->SetValue(s.every_n_layers);
    m_chk_retract->SetValue(s.retract_before_park);
    m_txt_park_gcode->SetValue(s.park_gcode);
    m_txt_trigger_gcode->SetValue(s.trigger_gcode);
    m_txt_resume_gcode->SetValue(s.resume_gcode);

    update_controls_state();

    // Events
    m_chk_enabled->Bind(wxEVT_CHECKBOX, &TimelapseSettingsDialog::on_toggle_enabled, this);
    m_choice_preset->Bind(wxEVT_CHOICE, &TimelapseSettingsDialog::on_preset_selected, this);
    m_btn_save->Bind(wxEVT_BUTTON, &TimelapseSettingsDialog::on_save, this);
}

void TimelapseSettingsDialog::update_controls_state()
{
    bool en = m_chk_enabled->GetValue();
    for (auto* w : {(wxWindow*)m_spin_park_x, m_spin_park_y, m_spin_z_lift,
                    m_spin_dwell, m_spin_every_n, m_chk_retract,
                    m_txt_park_gcode, m_txt_trigger_gcode, m_txt_resume_gcode,
                    m_choice_preset}) {
        if (w) w->Enable(en);
    }
}

void TimelapseSettingsDialog::on_toggle_enabled(wxCommandEvent&)
{
    update_controls_state();
}

void TimelapseSettingsDialog::on_preset_selected(wxCommandEvent&)
{
    int sel = m_choice_preset->GetSelection();
    switch (sel) {
    case 1: // Klipper
        m_txt_park_gcode->SetValue("G1 X{park_x} Y{park_y} F9000");
        m_txt_trigger_gcode->SetValue("M240");
        m_txt_resume_gcode->SetValue("");
        m_spin_dwell->SetValue(500);
        break;
    case 2: // Marlin
        m_txt_park_gcode->SetValue("G1 X{park_x} Y{park_y} F6000");
        m_txt_trigger_gcode->SetValue("M240");
        m_txt_resume_gcode->SetValue("");
        m_spin_dwell->SetValue(300);
        break;
    case 3: // OctoPrint
        m_txt_park_gcode->SetValue("G1 X{park_x} Y{park_y} F9000");
        m_txt_trigger_gcode->SetValue("@timelapse_takephoto");
        m_txt_resume_gcode->SetValue("");
        m_spin_dwell->SetValue(200);
        break;
    case 4: // Bambu Lab
        m_txt_park_gcode->SetValue("; Bambu handles timelapse internally");
        m_txt_trigger_gcode->SetValue("; M971 S11 C10 O0");
        m_txt_resume_gcode->SetValue("");
        m_spin_dwell->SetValue(0);
        break;
    default:
        break;
    }
}

void TimelapseSettingsDialog::on_save(wxCommandEvent&)
{
    auto s = get_settings();
    s.save();
    EndModal(wxID_OK);
}

TimelapseInjectorSettings TimelapseSettingsDialog::get_settings() const
{
    TimelapseInjectorSettings s;
    s.enabled             = m_chk_enabled->GetValue();
    s.park_x              = m_spin_park_x->GetValue();
    s.park_y              = m_spin_park_y->GetValue();
    s.park_z_lift         = m_spin_z_lift->GetValue();
    s.dwell_ms            = m_spin_dwell->GetValue();
    s.every_n_layers      = m_spin_every_n->GetValue();
    s.retract_before_park = m_chk_retract->GetValue();
    s.park_gcode          = m_txt_park_gcode->GetValue().ToStdString();
    s.trigger_gcode       = m_txt_trigger_gcode->GetValue().ToStdString();
    s.resume_gcode        = m_txt_resume_gcode->GetValue().ToStdString();
    return s;
}

void TimelapseSettingsDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
