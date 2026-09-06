#include "ElasticModel.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace OmarchyFX;
using Hyprutils::Memory::sc;

// A rect that moves further than this in one millisecond while nothing is
// animating it has been teleported — moved to another workspace, pulled onto
// another monitor — and the lattice is snapped rather than stretched across the
// gap.
static constexpr double TELEPORT_SPEED = 6.0; // px/ms

// Time constant of the low-pass on the rect's velocity. Short, because the
// animations this reacts to are themselves only a few hundred milliseconds.
static constexpr double TRAVEL_TAU = 25.0; // ms

// How much stiffer the spring gets at full stretch. Bounding it is what keeps
// the fixed-step integration stable: the effective frequency of the stiffest
// preset stays well inside what one substep can carry.
static constexpr double MAX_PROGRESSION = 4.0;

// Below these the lattice is back on the rect and the window is left alone.
static constexpr double STOP_OFFSET   = 0.35; // px
static constexpr double STOP_VELOCITY = 0.02; // px/ms

SElasticParams OmarchyFX::elasticPreset(int level) {
    static constexpr float PERIOD[5]  = {110.F, 145.F, 180.F, 235.F, 310.F};
    static constexpr float DAMPING[5] = {0.80F, 0.65F, 0.55F, 0.45F, 0.35F};
    static constexpr float TILT[5]    = {0.35F, 0.50F, 0.65F, 0.78F, 0.88F};
    static constexpr float FOLLOW[5]  = {0.30F, 0.42F, 0.55F, 0.68F, 0.80F};
    static constexpr float STRETCH[5] = {30.F, 45.F, 65.F, 90.F, 130.F};

    const int      L = std::clamp(level, 0, 4);

    SElasticParams params;
    params.period  = PERIOD[L];
    params.damping = DAMPING[L];
    params.tilt    = TILT[L];
    params.follow  = FOLLOW[L];
    params.maxStretch = STRETCH[L];

    return params;
}

static Vector2D rectCenter(const CBox& rect) {
    return Vector2D{rect.x + rect.w / 2.0, rect.y + rect.h / 2.0};
}

void CElasticModel::arm(const CBox& rect) {
    latticeOrigins(rect, m_origin);

    m_position = m_origin;
    m_velocity.fill(Vector2D{});

    m_travel     = {};
    m_lastCenter = rectCenter(rect);

    m_running = true;
    m_seeded  = true;
}

void CElasticModel::advance(const CBox& rect, float frameDtMs, bool animating) {
    const auto CENTER = rectCenter(rect);

    if (!m_seeded || frameDtMs <= 0.F) {
        m_lastCenter = CENTER;
        m_seeded     = true;
        return;
    }

    const auto   DELTA = CENTER - m_lastCenter;
    const double SPEED = DELTA.size() / frameDtMs;

    m_lastCenter = CENTER;

    // Cheap insurance. Nothing here should be able to produce a non-finite
    // position any more, but one that escaped would be handed to the renderer
    // as NaN vertices, and a window that vanishes is a worse bug than a wobble
    // that skips.
    for (const auto& p : m_position) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            arm(rect);
            return;
        }
    }

    if (!animating && SPEED > TELEPORT_SPEED) {
        arm(rect);
        return;
    }

    // Exponential low-pass, written against the frame time so the smoothing is
    // the same at any refresh rate.
    const double ALPHA = 1.0 - std::exp(-sc<double>(frameDtMs) / TRAVEL_TAU);
    m_travel += (DELTA / frameDtMs - m_travel) * ALPHA;
}

void CElasticModel::step(float dtMs, const SElasticParams& params, const CBox& rect) {
    latticeOrigins(rect, m_origin);

    const double OMEGA = 2.0 * std::numbers::pi / std::max(sc<double>(params.period), 1.0);
    const double DAMP  = 2.0 * std::clamp(sc<double>(params.damping), 0.0, 4.0) * OMEGA;
    const double LIMIT = std::max(sc<double>(params.maxStretch), 1.0);
    const double DT    = dtMs;

    // The tilt only makes sense while the window is actually travelling, and it
    // has to fade out with the speed: once the rect has stopped, every point
    // must be anchored the same or the lattice would settle off-centre.
    const double SPEED = m_travel.size();
    Vector2D     dir   = SPEED > 1e-4 ? m_travel / SPEED : Vector2D{};
    const double TILT  = std::clamp(sc<double>(params.tilt), 0.0, 0.95) * std::clamp(SPEED / std::max(sc<double>(params.refSpeed), 0.01), 0.0, 1.0);

    const auto   CENTER = rectCenter(rect);
    const auto   HALF   = Vector2D{std::max(rect.w, 1.0) / 2.0, std::max(rect.h, 1.0) / 2.0};

    // Pull a point back inside the limit, dropping the part of its velocity
    // that is pushing further out so it does not keep grinding into the stop.
    const auto confine = [&](int k) {
        const auto   X   = m_position[k] - m_origin[k];
        const double MAG = X.size();

        if (MAG <= LIMIT)
            return X;

        const auto   N  = X / MAG;
        const double VN = m_velocity[k].x * N.x + m_velocity[k].y * N.y;

        if (VN > 0)
            m_velocity[k] -= N * VN;

        m_position[k] = m_origin[k] + N * LIMIT;
        return N * LIMIT;
    };

    for (int k = 0; k < Lattice::COUNT; ++k) {
        // Confine before asking for a force, not only after. Hyprland covers
        // most of a swap in the first frame of the animation, which leaves the
        // lattice hundreds of pixels behind its rest position — and a spring
        // asked for the force at that extension would fling the point clean
        // across to the opposite stop.
        const auto   X   = confine(k);
        const double MAG = X.size();

        // Where this point sits along the direction of travel, -1 at the
        // trailing edge and +1 at the leading one.
        const auto   REL  = (m_origin[k] - CENTER) / HALF;
        const double LEAD = std::clamp(REL.x * dir.x + REL.y * dir.y, -1.0, 1.0);

        // Progressive spring: the closer a point is dragged to the limit, the
        // harder it pulls back, so the deformation saturates instead of running
        // away when a window is flung across the screen. The progression is
        // bounded, and so therefore is the effective spring frequency, which is
        // what keeps the fixed substep stable — an unbounded stiffness here
        // blows the integrator up within a handful of frames.
        const double REACH = std::min(MAG / LIMIT, 1.0);
        const double GAIN  = (1.0 + TILT * LEAD) * (1.0 + (MAX_PROGRESSION - 1.0) * REACH * REACH);

        const auto   ACC = X * (-OMEGA * OMEGA * GAIN) - m_velocity[k] * DAMP;

        m_velocity[k] += ACC * DT;
        m_position[k] += m_velocity[k] * DT;

        confine(k);
    }
}

bool CElasticModel::settled() const {
    for (int k = 0; k < Lattice::COUNT; ++k) {
        if ((m_position[k] - m_origin[k]).size() > STOP_OFFSET || m_velocity[k].size() > STOP_VELOCITY)
            return false;
    }

    return true;
}

SLattice CElasticModel::offsets(const SElasticParams& params) const {
    SLattice out;
    Vector2D mean;

    for (int k = 0; k < Lattice::COUNT; ++k) {
        out[k] = m_position[k] - m_origin[k];
        mean += out[k];
    }

    mean /= sc<double>(Lattice::COUNT);

    // Take back the share of the bulk lag the user did not ask for. Doing it
    // here rather than in the physics keeps the simulation itself untouched:
    // `follow` changes what is drawn, not how the springs behave.
    const auto DROP = mean * (1.0 - std::clamp(sc<double>(params.follow), 0.0, 1.0));

    for (auto& o : out) {
        o -= DROP;
    }

    return out;
}

CBox CElasticModel::controlBounds(const SElasticParams& params) const {
    SLattice points = offsets(params);

    for (int k = 0; k < Lattice::COUNT; ++k) {
        points[k] += m_origin[k];
    }

    return latticeBounds(points);
}
