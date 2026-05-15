#pragma once
// Lightweight heuristic “spaghetti / chaos” detector for live-view frames.
// This is not a trained neural network (no Obico weights bundled). It compares
// successive downscaled luminance frames and flags sudden texture explosion.
// Intended to be replaceable by an ONNX/TFLite pipeline later.

#include <cstdint>
#include <vector>

namespace Slic3r {

struct Rgb8View {
    const uint8_t* data{ nullptr };
    int            width{ 0 };
    int            height{ 0 };
    int            stride_bytes{ 0 }; ///< bytes per row (>= width*3)
};

class PrintFailureImageHeuristic {
public:
    explicit PrintFailureImageHeuristic(int downsample_max_side = 160);

    void reset();

    /// Returns anomaly score in [0,1]. Higher = more sudden visual chaos vs previous frame.
    float analyze(const Rgb8View& frame);

private:
    int                    m_max_side;
    std::vector<uint8_t> m_prev_gray; ///< downscaled W*H
    int                    m_small_w{ 0 };
    int                    m_small_h{ 0 };
};

} // namespace Slic3r
