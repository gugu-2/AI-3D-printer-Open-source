#pragma once
// PrintFailureNotifier.hpp
// Monitors print progress and alerts the user when a potential failure is
// detected: stalled progress, temperature anomaly, layer-count mismatch, or
// sudden live-view chaos (heuristic spaghetti detector hook).
// Works with both local (USB/LAN) and cloud-connected printers via the
// existing DeviceManager / MachineObject infrastructure.

#ifndef slic3r_PrintFailureNotifier_hpp_
#define slic3r_PrintFailureNotifier_hpp_

#include "libslic3r/PrintFailureImageDetector.hpp"

#include <string>
#include <functional>
#include <chrono>
#include <wx/timer.h>
#include <wx/event.h>

class wxImage;

namespace Slic3r {
namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// Failure event types
// ─────────────────────────────────────────────────────────────────────────────

enum class PrintFailureType {
    ProgressStall,        // Progress % hasn't changed in N minutes
    TemperatureAnomaly, // Nozzle/bed temp deviates > threshold from target
    LayerCountMismatch, // Reported layer count jumped unexpectedly
    PrintStopped,       // Printer reports stopped/error state
    VisualSpaghetti,    // Live-view heuristic suggests spaghetti / severe motion chaos
};

struct PrintFailureEvent {
    PrintFailureType type;
    std::string      description;
    std::string      printer_id;
    std::time_t      detected_at;
};

// ─────────────────────────────────────────────────────────────────────────────
// Notifier settings
// ─────────────────────────────────────────────────────────────────────────────

struct PrintFailureNotifierSettings {
    bool   enabled                    = true;
    int    stall_timeout_minutes      = 5;    // alert if no progress for N min
    double temp_deviation_threshold_c = 15.0; // °C deviation to trigger alert
    bool   show_desktop_notification  = true;
    bool   play_sound                 = false;
    bool   auto_pause_on_failure      = false;

    // Live-view (webcam / LAN stream) heuristic — same integration point as Obico-style ML, without bundled weights.
    bool   webcam_detection_enabled = false;
    float  webcam_sensitivity       = 0.55f; // score threshold in [0,1]
    int    webcam_consecutive_hits  = 4;     // consecutive frames above threshold
    bool   webcam_auto_pause        = true;
    int    webcam_min_interval_ms   = 2000;  // rate-limit analysis

    static PrintFailureNotifierSettings load();
    void save() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Notifier (singleton, owns a wxTimer)
// ─────────────────────────────────────────────────────────────────────────────

class PrintFailureNotifier : public wxEvtHandler {
public:
    static PrintFailureNotifier& instance();

    // Call once after GUI is initialized
    void init();

    // Start/stop monitoring a specific printer
    void start_monitoring(const std::string& printer_id);
    void stop_monitoring();

    bool is_monitoring() const { return m_monitoring; }
    const std::string& monitored_printer_id() const { return m_printer_id; }

    PrintFailureNotifierSettings& settings() { return m_settings; }
    const PrintFailureNotifierSettings& settings() const { return m_settings; }

    // Register a callback for when a failure is detected
    using FailureCallback = std::function<void(const PrintFailureEvent&)>;
    void set_failure_callback(FailureCallback cb) { m_callback = std::move(cb); }

    // Reset internal state (call when a new print starts)
    void reset_state();

    /// Feed decoded RGB live-view frames (wxImage RGB24). No-op when disabled.
    void submit_liveview_wx_image(const wxImage& image);

private:
    PrintFailureNotifier() = default;
    ~PrintFailureNotifier() = default;
    PrintFailureNotifier(const PrintFailureNotifier&) = delete;
    PrintFailureNotifier& operator=(const PrintFailureNotifier&) = delete;

    void on_timer(wxTimerEvent& evt);
    void check_progress_stall();
    void check_temperature_anomaly();
    void fire_event(PrintFailureType type, const std::string& description);
    void show_notification(const PrintFailureEvent& evt);

    PrintFailureImageHeuristic           m_image_heuristic;
    int                                  m_webcam_streak{ 0 };
    bool                                 m_webcam_alerted{ false };
    bool                                 m_webcam_rate_init{ false };
    std::chrono::steady_clock::time_point m_last_webcam_eval{};

    wxTimer*                     m_timer{ nullptr };
    bool                         m_monitoring{ false };
    std::string                  m_printer_id;
    PrintFailureNotifierSettings m_settings;
    FailureCallback              m_callback;

    // State tracking
    double      m_last_progress_pct{ -1.0 };
    std::time_t m_last_progress_time{ 0 };
    int         m_last_layer{ -1 };
    bool        m_alerted_stall{ false };
    bool        m_alerted_temp{ false };
};

// ─────────────────────────────────────────────────────────────────────────────
// Settings dialog
// ─────────────────────────────────────────────────────────────────────────────

class PrintFailureNotifierSettingsDialog; // forward decl

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_PrintFailureNotifier_hpp_
