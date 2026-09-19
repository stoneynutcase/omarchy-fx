#include "PulseModel.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace OmarchyFX;
using Hyprutils::Memory::sc;

// Below these the swell is invisible and the window is left alone.
static constexpr double STOP_OFFSET   = 0.3;  // px
static constexpr double STOP_VELOCITY = 0.02; // px/ms

SPulseParams OmarchyFX::pulsePreset(int level) {
    static constexpr float AMOUNT[5]  = {4.F, 6.F, 9.F, 13.F, 18.F};
    static constexpr float PERIOD[5]  = {180.F, 220.F, 260.F, 300.F, 340.F};
    static constexpr float DAMPING[5] = {0.90F, 0.70F, 0.55F, 0.42F, 0.32F};

    const int    L = std::clamp(level, 0, 4);

    SPulseParams params;
    params.amount  = AMOUNT[L];
    params.period  = PERIOD[L];
    params.damping = DAMPING[L];

    return params;
}

static double omegaOf(const SPulseParams& params) {
    return 2.0 * std::numbers::pi / std::max(sc<double>(params.period), 1.0);
}

void CPulseModel::arm(const CBox& rect, const SPulseParams& params) {
    latticeOrigins(rect, m_origin);
    m_rect = rect;

    // Start on the rect with a kick, not off the rect with a jump: a window
    // that snaps out and springs back reads as a glitch, one that swells out
    // and settles reads as a pulse.
    //
    // For an underdamped oscillator released from x = 0 with velocity v0, the
    // first peak is x = (v0 / w) * exp(-z * phi / sqrt(1 - z^2)) with
    // phi = atan2(sqrt(1 - z^2), z). Invert that so the peak lands on `amount`
    // whatever the damping.
    const double W   = omegaOf(params);
    const double Z   = std::clamp(sc<double>(params.damping), 0.05, 0.95);
    const double S   = std::sqrt(1.0 - Z * Z);
    const double PHI = std::atan2(S, Z);

    m_x = 0;
    m_v = std::max(sc<double>(params.amount), 0.0) * W * std::exp(Z * PHI / S);
}

void CPulseModel::step(float dtMs, const SPulseParams& params, const CBox& rect) {
    latticeOrigins(rect, m_origin);
    m_rect = rect;

    const double W    = omegaOf(params);
    const double DAMP = 2.0 * std::clamp(sc<double>(params.damping), 0.0, 4.0) * W;
    const double DT   = dtMs;

    const double ACC = -W * W * m_x - DAMP * m_v;

    m_v += ACC * DT;
    m_x += m_v * DT;

    if (!std::isfinite(m_x) || !std::isfinite(m_v)) {
        m_x = 0;
        m_v = 0;
    }
}

bool CPulseModel::settled() const {
    return std::abs(m_x) < STOP_OFFSET && std::abs(m_v) < STOP_VELOCITY;
}

SLattice CPulseModel::offsets(const SPulseParams& params) const {
    const auto   CENTER = Vector2D{m_rect.x + m_rect.w / 2.0, m_rect.y + m_rect.h / 2.0};
    const auto   HALF   = Vector2D{std::max(m_rect.w, 1.0) / 2.0, std::max(m_rect.h, 1.0) / 2.0};
    const double BULGE  = std::clamp(sc<double>(params.bulge), 0.0, 1.0);

    SLattice     out;

    for (int k = 0; k < Lattice::COUNT; ++k) {
        // Where the point sits, -1..1 on each axis. Scaling by m_x moves the
        // outer edges by exactly m_x pixels, the inner points by a third.
        const auto   REL   = (m_origin[k] - CENTER) / HALF;

        // Corners carry less than edge midpoints, which is what bows the sides
        // instead of merely scaling the rectangle.
        const double SHAPE = 1.0 - BULGE * std::abs(REL.x * REL.y);

        out[k] = REL * (m_x * SHAPE);
    }

    return out;
}

CBox CPulseModel::controlBounds(const SPulseParams& params) const {
    SLattice points = offsets(params);

    for (int k = 0; k < Lattice::COUNT; ++k) {
        points[k] += m_origin[k];
    }

    return latticeBounds(points);
}
