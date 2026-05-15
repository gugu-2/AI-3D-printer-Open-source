#pragma once
// ============================================================================
// AcousticMonitor.hpp  —  Real-time acoustic print failure detection
// ============================================================================
// Analyses audio from the printer's microphone (or a companion app) to detect
// anomalous sounds that indicate print failures:
//
//   • Layer delamination  — a sharp crack/pop sound
//   • Grinding / skipping — rhythmic grinding at motor frequency
//   • Nozzle clog         — high-pitched squealing or sudden silence
//   • Spaghetti           — irregular tapping as filament hits the bed
//   • Normal printing     — baseline reference
//
// Architecture
// ─────────────
// The monitor runs in a background thread.  Audio samples arrive via
// push_samples() (called from a platform audio callback or a network socket
// receiving data from a companion app).  The monitor:
//
//   1. Buffers samples into overlapping frames of FRAME_SIZE samples.
//   2. Computes a compact feature vector per frame:
//        • RMS energy
//        • Zero-crossing rate
//        • Spectral centroid (via a simple DFT)
//        • Spectral flux (frame-to-frame spectral change)
//        • Peak frequency bin
//        • Crest factor (peak / RMS)
//   3. Classifies the frame using a lightweight threshold-based classifier
//      (no ML library required).
//   4. Applies temporal smoothing to reduce false positives.
//   5. Fires a callback when an anomaly is detected.
//
// The classifier thresholds are calibrated against a synthetic dataset of
// known-good and known-bad print sounds.  Users can recalibrate by running
// a short baseline recording.
//
// Thread safety
// ─────────────
// push_samples() is safe to call from any thread.
// The anomaly callback is invoked from the monitor's internal thread.
// ============================================================================

#ifndef slic3r_AcousticMonitor_hpp_
#define slic3r_AcousticMonitor_hpp_

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>

namespace Slic3r {
namespace Acoustic {

// ============================================================================
// Constants
// ============================================================================

static constexpr int SAMPLE_RATE   = 44100; // Hz
static constexpr int FRAME_SIZE    = 2048;  // samples per analysis frame
static constexpr int HOP_SIZE      = 512;   // samples between frames (75% overlap)
static constexpr int FEATURE_DIM   = 6;     // feature vector size
static constexpr int SMOOTH_FRAMES = 8;     // temporal smoothing window

// ============================================================================
// Anomaly types
// ============================================================================

enum class AnomalyType {
    Normal,
    LayerDelamination,  // crack/pop — sudden high-energy transient
    GrindingSkipping,   // rhythmic grinding — periodic spectral peaks
    NozzleClog,         // high-pitched squeal or sudden silence
    SpaghettiDetected,  // irregular tapping
    UnknownAnomaly
};

inline const char* anomaly_name(AnomalyType t) {
    switch (t) {
    case AnomalyType::Normal:             return "Normal";
    case AnomalyType::LayerDelamination:  return "Layer delamination";
    case AnomalyType::GrindingSkipping:   return "Grinding / skipping";
    case AnomalyType::NozzleClog:         return "Nozzle clog";
    case AnomalyType::SpaghettiDetected:  return "Spaghetti detected";
    default:                              return "Unknown anomaly";
    }
}

// ============================================================================
// Feature vector
// ============================================================================

struct AudioFeatures {
    float rms;              // root-mean-square energy
    float zcr;              // zero-crossing rate (crossings per sample)
    float spectral_centroid;// weighted mean frequency (Hz)
    float spectral_flux;    // frame-to-frame spectral change
    float peak_freq_hz;     // frequency of the dominant spectral peak (Hz)
    float crest_factor;     // peak / RMS
};

// ============================================================================
// Anomaly event
// ============================================================================

struct AnomalyEvent {
    AnomalyType type;
    float       confidence;   // [0, 1]
    double      timestamp_s;  // seconds since monitoring started
    AudioFeatures features;
    std::string description;
};

// ============================================================================
// Classifier thresholds (calibratable)
// ============================================================================

struct ClassifierThresholds {
    // Delamination: sudden high-energy transient
    float delam_crest_min      = 8.0f;   // crest factor threshold
    float delam_rms_min        = 0.05f;  // minimum RMS to avoid noise triggers

    // Grinding: periodic spectral peaks at motor frequencies
    float grind_flux_min       = 0.15f;  // spectral flux threshold
    float grind_centroid_max   = 3000.f; // Hz — grinding is low-frequency

    // Nozzle clog: high-pitched squeal
    float clog_centroid_min    = 8000.f; // Hz
    float clog_rms_min         = 0.02f;

    // Silence (clog variant): very low RMS during active print
    float silence_rms_max      = 0.002f;

    // Spaghetti: irregular tapping — high ZCR with moderate RMS
    float spag_zcr_min         = 0.15f;
    float spag_rms_min         = 0.01f;
    float spag_rms_max         = 0.08f;

    // Minimum consecutive anomalous frames before firing event
    int   min_consecutive       = 3;
};

// ============================================================================
// Monitor configuration
// ============================================================================

struct MonitorConfig {
    int    sample_rate         = SAMPLE_RATE;
    int    frame_size          = FRAME_SIZE;
    int    hop_size            = HOP_SIZE;
    int    smooth_frames       = SMOOTH_FRAMES;
    bool   enabled             = true;
    bool   verbose             = false;
    ClassifierThresholds thresholds;
};

// ============================================================================
// Acoustic monitor
// ============================================================================

class AcousticMonitor {
public:
    using AnomalyCallback = std::function<void(const AnomalyEvent&)>;

    explicit AcousticMonitor(const MonitorConfig& cfg = MonitorConfig{});
    ~AcousticMonitor();

    // Start/stop the background processing thread
    void start();
    void stop();
    bool is_running() const { return m_running.load(); }

    // Push raw audio samples (float, normalised to [-1, 1]).
    // Thread-safe; can be called from an audio callback.
    void push_samples(const float* samples, int count);

    // Register callback for anomaly events (called from monitor thread)
    void set_anomaly_callback(AnomalyCallback cb);

    // Calibrate baseline from the next N seconds of audio
    void calibrate_baseline(float duration_s = 5.f);

    // Reset state (call when starting a new print)
    void reset();

    // Access the last computed features (for UI display)
    AudioFeatures last_features() const;

    // Access configuration
    const MonitorConfig& config() const { return m_cfg; }
    MonitorConfig&       config()       { return m_cfg; }

    // Compute features from a frame of samples (public for testing)
    static AudioFeatures compute_features(const float* frame,
                                           int          frame_size,
                                           int          sample_rate,
                                           const float* prev_spectrum,
                                           int          spectrum_size);

    // Classify features into an anomaly type
    AnomalyType classify(const AudioFeatures& feat) const;

    // Compute confidence score for a classification
    float confidence(AnomalyType type, const AudioFeatures& feat) const;

private:
    MonitorConfig    m_cfg;
    AnomalyCallback  m_callback;

    // Audio ring buffer
    mutable std::mutex       m_buf_mutex;
    std::deque<float>        m_buffer;

    // Processing thread
    std::thread              m_thread;
    std::atomic<bool>        m_running{false};
    std::atomic<bool>        m_stop_requested{false};

    // State
    mutable std::mutex       m_state_mutex;
    AudioFeatures            m_last_features{};
    std::vector<float>       m_prev_spectrum;
    double                   m_start_time_s{0.0};
    double                   m_samples_processed{0.0};

    // Temporal smoothing
    std::deque<AnomalyType>  m_recent_types;
    int                      m_consecutive_count{0};
    AnomalyType              m_last_fired{AnomalyType::Normal};

    // Calibration
    bool                     m_calibrating{false};
    float                    m_calibration_remaining_s{0.f};
    std::vector<AudioFeatures> m_calibration_samples;

    void processing_loop();
    void process_frame(const float* frame);

    // Simple DFT magnitude spectrum (no FFT library needed for FRAME_SIZE=2048)
    static void compute_spectrum(const float* frame, int N, std::vector<float>& mag);
};

} // namespace Acoustic
} // namespace Slic3r

#endif // slic3r_AcousticMonitor_hpp_
