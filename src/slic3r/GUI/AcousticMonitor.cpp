// ============================================================================
// AcousticMonitor.cpp  —  Real-time acoustic print failure detection
// ============================================================================
#include "AcousticMonitor.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <sstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Slic3r {
namespace Acoustic {

// ============================================================================
// DFT-based spectrum (Cooley-Tukey radix-2 iterative FFT)
// ============================================================================

// Bit-reversal permutation
static void bit_reverse(std::vector<float>& re, std::vector<float>& im)
{
    const int N = int(re.size());
    for (int i = 1, j = 0; i < N; ++i) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[size_t(i)], re[size_t(j)]);
            std::swap(im[size_t(i)], im[size_t(j)]);
        }
    }
}

// In-place radix-2 FFT (N must be power of 2)
static void fft(std::vector<float>& re, std::vector<float>& im)
{
    const int N = int(re.size());
    bit_reverse(re, im);
    for (int len = 2; len <= N; len <<= 1) {
        const float ang = float(-2.0 * M_PI / double(len));
        const float wr0 = std::cos(ang), wi0 = std::sin(ang);
        for (int i = 0; i < N; i += len) {
            float wr = 1.f, wi = 0.f;
            for (int j = 0; j < len / 2; ++j) {
                const float ur = re[size_t(i + j)],           ui = im[size_t(i + j)];
                const float vr = re[size_t(i + j + len/2)] * wr - im[size_t(i + j + len/2)] * wi;
                const float vi = re[size_t(i + j + len/2)] * wi + im[size_t(i + j + len/2)] * wr;
                re[size_t(i + j)]           = ur + vr;
                im[size_t(i + j)]           = ui + vi;
                re[size_t(i + j + len/2)]   = ur - vr;
                im[size_t(i + j + len/2)]   = ui - vi;
                const float new_wr = wr * wr0 - wi * wi0;
                wi = wr * wi0 + wi * wr0;
                wr = new_wr;
            }
        }
    }
}

void AcousticMonitor::compute_spectrum(const float* frame, int N,
                                        std::vector<float>& mag)
{
    // Apply Hann window
    std::vector<float> re(size_t(N)), im(size_t(N), 0.f);
    for (int i = 0; i < N; ++i) {
        const float w = 0.5f * (1.f - std::cos(float(2.0 * M_PI * i / (N - 1))));
        re[size_t(i)] = frame[i] * w;
    }
    fft(re, im);
    mag.resize(size_t(N / 2));
    for (int i = 0; i < N / 2; ++i)
        mag[size_t(i)] = std::sqrt(re[size_t(i)] * re[size_t(i)] + im[size_t(i)] * im[size_t(i)]) / float(N);
}

// ============================================================================
// Feature extraction
// ============================================================================

AudioFeatures AcousticMonitor::compute_features(const float* frame,
                                                  int          frame_size,
                                                  int          sample_rate,
                                                  const float* prev_spectrum,
                                                  int          spectrum_size)
{
    AudioFeatures f{};

    // RMS
    double rms_sq = 0.;
    for (int i = 0; i < frame_size; ++i)
        rms_sq += double(frame[i]) * double(frame[i]);
    f.rms = float(std::sqrt(rms_sq / double(frame_size)));

    // Zero-crossing rate
    int zcr = 0;
    for (int i = 1; i < frame_size; ++i)
        if ((frame[i] >= 0.f) != (frame[i - 1] >= 0.f))
            ++zcr;
    f.zcr = float(zcr) / float(frame_size);

    // Crest factor
    float peak = 0.f;
    for (int i = 0; i < frame_size; ++i)
        peak = std::max(peak, std::abs(frame[i]));
    f.crest_factor = (f.rms > 1e-10f) ? (peak / f.rms) : 0.f;

    // Spectrum
    std::vector<float> mag;
    compute_spectrum(frame, frame_size, mag);
    const int spec_size = int(mag.size());

    // Spectral centroid
    double num = 0., den = 0.;
    for (int i = 0; i < spec_size; ++i) {
        const double freq = double(i) * double(sample_rate) / double(frame_size);
        num += freq * double(mag[size_t(i)]);
        den += double(mag[size_t(i)]);
    }
    f.spectral_centroid = (den > 1e-20) ? float(num / den) : 0.f;

    // Peak frequency
    int peak_bin = 0;
    float peak_mag = 0.f;
    for (int i = 0; i < spec_size; ++i) {
        if (mag[size_t(i)] > peak_mag) {
            peak_mag = mag[size_t(i)];
            peak_bin = i;
        }
    }
    f.peak_freq_hz = float(peak_bin) * float(sample_rate) / float(frame_size);

    // Spectral flux
    if (prev_spectrum && spectrum_size == spec_size) {
        double flux = 0.;
        for (int i = 0; i < spec_size; ++i) {
            const float diff = mag[size_t(i)] - prev_spectrum[i];
            flux += double(diff > 0.f ? diff : 0.f); // half-wave rectified
        }
        f.spectral_flux = float(flux / double(spec_size));
    }

    return f;
}

// ============================================================================
// Classifier
// ============================================================================

AnomalyType AcousticMonitor::classify(const AudioFeatures& feat) const
{
    const auto& t = m_cfg.thresholds;

    // Silence (clog variant)
    if (feat.rms < t.silence_rms_max)
        return AnomalyType::NozzleClog;

    // Delamination: sudden high-energy transient
    if (feat.crest_factor >= t.delam_crest_min && feat.rms >= t.delam_rms_min)
        return AnomalyType::LayerDelamination;

    // Nozzle clog: high-pitched squeal
    if (feat.spectral_centroid >= t.clog_centroid_min && feat.rms >= t.clog_rms_min)
        return AnomalyType::NozzleClog;

    // Grinding: high spectral flux, low centroid
    if (feat.spectral_flux >= t.grind_flux_min && feat.spectral_centroid <= t.grind_centroid_max)
        return AnomalyType::GrindingSkipping;

    // Spaghetti: irregular tapping
    if (feat.zcr >= t.spag_zcr_min && feat.rms >= t.spag_rms_min && feat.rms <= t.spag_rms_max)
        return AnomalyType::SpaghettiDetected;

    return AnomalyType::Normal;
}

float AcousticMonitor::confidence(AnomalyType type, const AudioFeatures& feat) const
{
    const auto& t = m_cfg.thresholds;
    switch (type) {
    case AnomalyType::LayerDelamination:
        return std::clamp((feat.crest_factor - t.delam_crest_min) / t.delam_crest_min, 0.f, 1.f);
    case AnomalyType::NozzleClog:
        if (feat.rms < t.silence_rms_max)
            return 0.9f;
        return std::clamp((feat.spectral_centroid - t.clog_centroid_min) / t.clog_centroid_min, 0.f, 1.f);
    case AnomalyType::GrindingSkipping:
        return std::clamp((feat.spectral_flux - t.grind_flux_min) / t.grind_flux_min, 0.f, 1.f);
    case AnomalyType::SpaghettiDetected:
        return std::clamp((feat.zcr - t.spag_zcr_min) / t.spag_zcr_min, 0.f, 1.f);
    default:
        return 0.f;
    }
}

// ============================================================================
// AcousticMonitor lifecycle
// ============================================================================

AcousticMonitor::AcousticMonitor(const MonitorConfig& cfg) : m_cfg(cfg) {}

AcousticMonitor::~AcousticMonitor() { stop(); }

void AcousticMonitor::start()
{
    if (m_running.load())
        return;
    m_stop_requested.store(false);
    m_running.store(true);
    m_start_time_s = double(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    m_thread = std::thread(&AcousticMonitor::processing_loop, this);
}

void AcousticMonitor::stop()
{
    m_stop_requested.store(true);
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false);
}

void AcousticMonitor::reset()
{
    std::lock_guard<std::mutex> lk(m_buf_mutex);
    m_buffer.clear();
    std::lock_guard<std::mutex> lk2(m_state_mutex);
    m_prev_spectrum.clear();
    m_recent_types.clear();
    m_consecutive_count = 0;
    m_last_fired = AnomalyType::Normal;
    m_samples_processed = 0.0;
}

void AcousticMonitor::push_samples(const float* samples, int count)
{
    if (!m_cfg.enabled || count <= 0)
        return;
    std::lock_guard<std::mutex> lk(m_buf_mutex);
    for (int i = 0; i < count; ++i)
        m_buffer.push_back(samples[i]);
    // Prevent unbounded growth (keep at most 4 seconds of audio)
    const size_t max_buf = size_t(m_cfg.sample_rate * 4);
    while (m_buffer.size() > max_buf)
        m_buffer.pop_front();
}

void AcousticMonitor::set_anomaly_callback(AnomalyCallback cb)
{
    m_callback = std::move(cb);
}

AudioFeatures AcousticMonitor::last_features() const
{
    std::lock_guard<std::mutex> lk(m_state_mutex);
    return m_last_features;
}

void AcousticMonitor::calibrate_baseline(float duration_s)
{
    m_calibrating = true;
    m_calibration_remaining_s = duration_s;
    m_calibration_samples.clear();
}

// ============================================================================
// Processing loop
// ============================================================================

void AcousticMonitor::processing_loop()
{
    std::vector<float> frame(size_t(m_cfg.frame_size));

    while (!m_stop_requested.load()) {
        // Wait until we have enough samples for a frame
        {
            std::lock_guard<std::mutex> lk(m_buf_mutex);
            if (int(m_buffer.size()) < m_cfg.frame_size) {
                // Not enough data yet — sleep briefly
            } else {
                // Copy frame
                for (int i = 0; i < m_cfg.frame_size; ++i)
                    frame[size_t(i)] = m_buffer[size_t(i)];
                // Advance by hop size
                for (int i = 0; i < m_cfg.hop_size && !m_buffer.empty(); ++i)
                    m_buffer.pop_front();
                // Process outside the lock
                process_frame(frame.data());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void AcousticMonitor::process_frame(const float* frame)
{
    std::vector<float> prev_spec;
    {
        std::lock_guard<std::mutex> lk(m_state_mutex);
        prev_spec = m_prev_spectrum;
    }

    const AudioFeatures feat = compute_features(
        frame, m_cfg.frame_size, m_cfg.sample_rate,
        prev_spec.empty() ? nullptr : prev_spec.data(),
        int(prev_spec.size()));

    // Update spectrum for next frame
    std::vector<float> new_spec;
    compute_spectrum(frame, m_cfg.frame_size, new_spec);

    {
        std::lock_guard<std::mutex> lk(m_state_mutex);
        m_last_features  = feat;
        m_prev_spectrum  = std::move(new_spec);
        m_samples_processed += double(m_cfg.hop_size);
    }

    // Calibration mode: collect samples, don't classify
    if (m_calibrating) {
        m_calibration_samples.push_back(feat);
        m_calibration_remaining_s -= float(m_cfg.hop_size) / float(m_cfg.sample_rate);
        if (m_calibration_remaining_s <= 0.f) {
            m_calibrating = false;
            // Could update thresholds based on calibration_samples here
        }
        return;
    }

    const AnomalyType type = classify(feat);

    // Temporal smoothing
    {
        std::lock_guard<std::mutex> lk(m_state_mutex);
        m_recent_types.push_back(type);
        if (int(m_recent_types.size()) > m_cfg.smooth_frames)
            m_recent_types.pop_front();

        // Count consecutive anomalous frames
        if (type != AnomalyType::Normal) {
            ++m_consecutive_count;
        } else {
            m_consecutive_count = 0;
        }

        // Fire event if we have enough consecutive anomalous frames
        if (m_consecutive_count >= m_cfg.thresholds.min_consecutive && type != m_last_fired) {
            m_last_fired = type;
            if (m_callback) {
                const double ts = m_samples_processed / double(m_cfg.sample_rate);
                AnomalyEvent evt;
                evt.type        = type;
                evt.confidence  = confidence(type, feat);
                evt.timestamp_s = ts;
                evt.features    = feat;

                std::ostringstream desc;
                desc << anomaly_name(type) << " detected at t=" << std::fixed
                     << std::setprecision(1) << ts << "s. "
                     << "RMS=" << std::setprecision(4) << feat.rms
                     << " ZCR=" << feat.zcr
                     << " Centroid=" << std::setprecision(0) << feat.spectral_centroid << "Hz"
                     << " Crest=" << std::setprecision(2) << feat.crest_factor;
                evt.description = desc.str();

                // Fire callback (outside the lock to avoid deadlock)
                auto cb = m_callback;
                lk.~lock_guard(); // release lock before calling callback
                cb(evt);
                return;
            }
        } else if (type == AnomalyType::Normal) {
            m_last_fired = AnomalyType::Normal;
        }
    }
}

} // namespace Acoustic
} // namespace Slic3r
