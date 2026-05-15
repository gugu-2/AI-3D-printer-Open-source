// PrintFailureNotifier.cpp
// Print failure detection and notification system.

#include "PrintFailureNotifier.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "DeviceManager.hpp"
#include "NotificationManager.hpp"
#include "MainFrame.hpp"
#include "libslic3r/AppConfig.hpp"

#include <wx/msgdlg.h>
#include <wx/notifmsg.h>
#include <wx/image.h>

#include <boost/log/trivial.hpp>
#include <ctime>
#include <cmath>

namespace Slic3r {
namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// Settings load/save
// ─────────────────────────────────────────────────────────────────────────────

PrintFailureNotifierSettings PrintFailureNotifierSettings::load()
{
    PrintFailureNotifierSettings s;
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg) return s;

    auto get_b = [&](const std::string& key, bool def) -> bool {
        std::string v = cfg->get("failure_notifier", key);
        if (v.empty()) return def;
        return v == "1" || v == "true";
    };
    auto get_i = [&](const std::string& key, int def) -> int {
        std::string v = cfg->get("failure_notifier", key);
        if (v.empty()) return def;
        try { return std::stoi(v); } catch (...) { return def; }
    };
    auto get_d = [&](const std::string& key, double def) -> double {
        std::string v = cfg->get("failure_notifier", key);
        if (v.empty()) return def;
        try { return std::stod(v); } catch (...) { return def; }
    };
    auto get_f = [&](const std::string& key, float def) -> float {
        std::string v = cfg->get("failure_notifier", key);
        if (v.empty()) return def;
        try { return float(std::stod(v)); } catch (...) { return def; }
    };

    s.enabled                    = get_b("enabled", true);
    s.stall_timeout_minutes      = get_i("stall_timeout_minutes", 5);
    s.temp_deviation_threshold_c = get_d("temp_deviation_threshold_c", 15.0);
    s.show_desktop_notification  = get_b("show_desktop_notification", true);
    s.play_sound                 = get_b("play_sound", false);
    s.auto_pause_on_failure      = get_b("auto_pause_on_failure", false);

    s.webcam_detection_enabled = get_b("webcam_detection_enabled", false);
    s.webcam_sensitivity       = get_f("webcam_sensitivity", 0.55f);
    s.webcam_consecutive_hits    = get_i("webcam_consecutive_hits", 4);
    s.webcam_auto_pause          = get_b("webcam_auto_pause", true);
    s.webcam_min_interval_ms     = get_i("webcam_min_interval_ms", 2000);
    return s;
}

void PrintFailureNotifierSettings::save() const
{
    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg) return;
    cfg->set("failure_notifier", "enabled", enabled ? "1" : "0");
    cfg->set("failure_notifier", "stall_timeout_minutes", std::to_string(stall_timeout_minutes));
    cfg->set("failure_notifier", "temp_deviation_threshold_c", std::to_string(temp_deviation_threshold_c));
    cfg->set("failure_notifier", "show_desktop_notification", show_desktop_notification ? "1" : "0");
    cfg->set("failure_notifier", "play_sound", play_sound ? "1" : "0");
    cfg->set("failure_notifier", "auto_pause_on_failure", auto_pause_on_failure ? "1" : "0");

    cfg->set("failure_notifier", "webcam_detection_enabled", webcam_detection_enabled ? "1" : "0");
    cfg->set("failure_notifier", "webcam_sensitivity", std::to_string(webcam_sensitivity));
    cfg->set("failure_notifier", "webcam_consecutive_hits", std::to_string(webcam_consecutive_hits));
    cfg->set("failure_notifier", "webcam_auto_pause", webcam_auto_pause ? "1" : "0");
    cfg->set("failure_notifier", "webcam_min_interval_ms", std::to_string(webcam_min_interval_ms));
    cfg->save();
}

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

PrintFailureNotifier& PrintFailureNotifier::instance()
{
    static PrintFailureNotifier s;
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────────

void PrintFailureNotifier::init()
{
    m_settings = PrintFailureNotifierSettings::load();

    if (!m_timer) {
        m_timer = new wxTimer(this);
        Bind(wxEVT_TIMER, &PrintFailureNotifier::on_timer, this);
    }
    BOOST_LOG_TRIVIAL(info) << "PrintFailureNotifier: initialized";
}

// ─────────────────────────────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────────────────────────────

void PrintFailureNotifier::start_monitoring(const std::string& printer_id)
{
    if (!m_settings.enabled) return;
    m_printer_id = printer_id;
    m_monitoring = true;
    reset_state();
    // Poll every 30 seconds
    if (m_timer) m_timer->Start(30000);
    BOOST_LOG_TRIVIAL(info) << "PrintFailureNotifier: monitoring " << printer_id;
}

void PrintFailureNotifier::stop_monitoring()
{
    m_monitoring = false;
    if (m_timer) m_timer->Stop();
    m_printer_id.clear();
    BOOST_LOG_TRIVIAL(info) << "PrintFailureNotifier: stopped";
}

void PrintFailureNotifier::reset_state()
{
    m_last_progress_pct  = -1.0;
    m_last_progress_time = std::time(nullptr);
    m_last_layer         = -1;
    m_alerted_stall      = false;
    m_alerted_temp       = false;
    m_image_heuristic.reset();
    m_webcam_streak   = 0;
    m_webcam_alerted  = false;
    m_webcam_rate_init = false;
    m_last_webcam_eval = std::chrono::steady_clock::time_point{};
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer callback — runs every 30 s
// ─────────────────────────────────────────────────────────────────────────────

void PrintFailureNotifier::on_timer(wxTimerEvent& /*evt*/)
{
    if (!m_monitoring || !m_settings.enabled) return;

    check_progress_stall();
    check_temperature_anomaly();
}

// ─────────────────────────────────────────────────────────────────────────────
// Progress stall detection
// ─────────────────────────────────────────────────────────────────────────────

void PrintFailureNotifier::check_progress_stall()
{
    if (m_alerted_stall) return;

    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm) return;

    MachineObject* obj = dm->get_selected_machine();
    if (!obj) return;

    // Only check while actively printing
    if (!obj->is_in_printing()) return;

    double progress = obj->mc_print_percent;

    if (m_last_progress_pct < 0.0) {
        // First sample
        m_last_progress_pct  = progress;
        m_last_progress_time = std::time(nullptr);
        return;
    }

    if (progress > m_last_progress_pct + 0.1) {
        // Progress is moving — reset stall timer
        m_last_progress_pct  = progress;
        m_last_progress_time = std::time(nullptr);
        return;
    }

    // Progress hasn't moved — check elapsed time
    std::time_t now    = std::time(nullptr);
    double      elapsed_min = static_cast<double>(now - m_last_progress_time) / 60.0;

    if (elapsed_min >= m_settings.stall_timeout_minutes) {
        m_alerted_stall = true;
        fire_event(PrintFailureType::ProgressStall,
                   "Print progress has not changed for " + std::to_string(static_cast<int>(elapsed_min)) +
                       " minutes. The print may have failed or stalled.");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Temperature anomaly detection
// ─────────────────────────────────────────────────────────────────────────────

void PrintFailureNotifier::check_temperature_anomaly()
{
    if (m_alerted_temp) return;

    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm) return;

    MachineObject* obj = dm->get_selected_machine();
    if (!obj) return;

    if (!obj->is_in_printing()) return;

    // Check nozzle temperature
    double nozzle_actual = obj->nozzle_temp;
    double nozzle_target = obj->nozzle_temp_target;

    if (nozzle_target > 0.0) {
        double deviation = std::abs(nozzle_actual - nozzle_target);
        if (deviation > m_settings.temp_deviation_threshold_c) {
            m_alerted_temp = true;
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "Nozzle temperature anomaly: target %.0f°C, actual %.0f°C (deviation %.0f°C).",
                          nozzle_target, nozzle_actual, deviation);
            fire_event(PrintFailureType::TemperatureAnomaly, buf);
        }
    }
}

void PrintFailureNotifier::submit_liveview_wx_image(const wxImage& image)
{
    if (!m_settings.enabled || !m_settings.webcam_detection_enabled || m_webcam_alerted)
        return;

    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm)
        return;
    MachineObject* obj = dm->get_selected_machine();
    if (!obj || !obj->is_in_printing())
        return;
    if (!image.IsOk() || image.GetWidth() < 32 || image.GetHeight() < 32)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_webcam_rate_init) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_webcam_eval).count();
        if (ms < m_settings.webcam_min_interval_ms)
            return;
    }
    m_webcam_rate_init = true;
    m_last_webcam_eval = now;

    Rgb8View view;
    view.data         = image.GetData();
    view.width        = image.GetWidth();
    view.height       = image.GetHeight();
    view.stride_bytes = view.width * 3;

    const float score = m_image_heuristic.analyze(view);
    if (score >= m_settings.webcam_sensitivity) {
        if (++m_webcam_streak >= m_settings.webcam_consecutive_hits) {
            m_webcam_alerted = true;
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Live-view anomaly score %.2f (heuristic). Enable only if false positives are acceptable; "
                          "replace with an ONNX/TFLite detector for Obico-class accuracy.",
                          score);
            fire_event(PrintFailureType::VisualSpaghetti, buf);
            m_webcam_streak = 0;
        }
    } else {
        m_webcam_streak = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Fire event
// ─────────────────────────────────────────────────────────────────────────────

void PrintFailureNotifier::fire_event(PrintFailureType type, const std::string& description)
{
    PrintFailureEvent evt;
    evt.type        = type;
    evt.description = description;
    evt.printer_id  = m_printer_id;
    evt.detected_at = std::time(nullptr);

    BOOST_LOG_TRIVIAL(warning) << "PrintFailureNotifier: " << description;

    // Show notification
    show_notification(evt);

    // User callback
    if (m_callback) m_callback(evt);

    // Auto-pause if configured
    const bool want_pause = (type == PrintFailureType::VisualSpaghetti) ? m_settings.webcam_auto_pause : m_settings.auto_pause_on_failure;
    if (want_pause) {
        DeviceManager* dm = wxGetApp().getDeviceManager();
        if (dm) {
            MachineObject* obj = dm->get_selected_machine();
            if (obj && obj->is_in_printing() && obj->can_pause()) {
                obj->command_task_pause();
                BOOST_LOG_TRIVIAL(info) << "PrintFailureNotifier: auto-paused print";
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Show notification
// ─────────────────────────────────────────────────────────────────────────────

void PrintFailureNotifier::show_notification(const PrintFailureEvent& evt)
{
    wxString title   = _L("Print Failure Detected");
    wxString message = evt.description;

    // Try desktop notification first
    if (m_settings.show_desktop_notification) {
#if wxUSE_NOTIFICATION_MESSAGE
        wxNotificationMessage notif(title, message);
        notif.SetFlags(wxICON_WARNING);
        if (notif.Show(8)) return; // shown for 8 seconds
#endif
    }

    // Fallback: in-app notification via NotificationManager
    NotificationManager* nm = wxGetApp().notification_manager();
    if (nm) {
        nm->push_notification(NotificationType::PrintFailure, NotificationManager::NotificationLevel::WarningNotificationLevel,
                             title.ToStdString() + ": " + message.ToStdString());
        return;
    }

    // Last resort: message box (non-blocking via CallAfter)
    wxGetApp().CallAfter([title, message]() { wxMessageBox(message, title, wxOK | wxICON_WARNING); });
}

} // namespace GUI
} // namespace Slic3r
