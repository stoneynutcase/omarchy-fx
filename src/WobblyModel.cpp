// SPDX-License-Identifier: GPL-2.0-or-later
//
// The wobble physics here is a port of KWin's wobblywindows effect:
// https://github.com/KDE/kwin/tree/master/src/plugins/wobblywindows
// SPDX-FileCopyrightText: 2008 Cedric Borgese <cedric.borgese@gmail.com>

#include "WobblyModel.hpp"

#include <algorithm>
#include <cmath>

using namespace OmarchyFX;
using Hyprutils::Memory::sc;

SWobblyParams OmarchyFX::wobblyPreset(int level) {
    // KWin's pset[0..4]: stiffness, drag, moveFactor. Everything else is shared.
    static constexpr float STIFFNESS[5] = {0.15F, 0.10F, 0.06F, 0.03F, 0.01F};
    static constexpr float DRAG[5]      = {0.80F, 0.85F, 0.90F, 0.92F, 0.97F};
    static constexpr float MOVE[5]      = {0.10F, 0.10F, 0.10F, 0.20F, 0.25F};

    const int             L = std::clamp(level, 0, 4);

    SWobblyParams         params;
    params.stiffness  = STIFFNESS[L];
    params.drag       = DRAG[L];
    params.moveFactor = MOVE[L];

    return params;
}

static void fixVectorBounds(Vector2D& vec, double min, double max) {
    if (std::abs(vec.x) < min)
        vec.x = 0.0;
    else if (std::abs(vec.x) > max)
        vec.x = vec.x > 0.0 ? max : -max;

    if (std::abs(vec.y) < min)
        vec.y = 0.0;
    else if (std::abs(vec.y) > max)
        vec.y = vec.y > 0.0 ? max : -max;
}

void CWobblyModel::computeOrigins(const CBox& rect) {
    m_xLength = rect.w / (GRID - 1.0);
    m_yLength = rect.h / (GRID - 1.0);

    latticeOrigins(rect, m_origin);
}

void CWobblyModel::reset(const CBox& rect) {
    computeOrigins(rect);

    m_position = m_origin;
    m_velocity.fill(Vector2D{});
    m_acceleration.fill(Vector2D{});
    m_constraint.fill(false);

    m_grabbed   = false;
    m_wobbling  = false;
    m_wobbleTop = m_wobbleLeft = m_wobbleRight = m_wobbleBottom = true;
}

void CWobblyModel::grab(const CBox& rect, const Vector2D& cursor, bool resizing) {
    computeOrigins(rect);

    const int I  = std::clamp(sc<int>((cursor.x - rect.x) / std::max(m_xLength, 1.0) + 0.5), 0, GRID - 1);
    const int J  = std::clamp(sc<int>((cursor.y - rect.y) / std::max(m_yLength, 1.0) + 0.5), 0, GRID - 1);

    m_constraint[index(I, J)] = true;
    m_grabbed                 = true;
    m_wobbling                = true;

    if (resizing) {
        // Nothing wobbles until an edge has actually been dragged.
        m_wobbleTop = m_wobbleLeft = m_wobbleRight = m_wobbleBottom = false;
        m_resizeOriginalRect                                        = rect;
    } else
        m_wobbleTop = m_wobbleLeft = m_wobbleRight = m_wobbleBottom = true;
}

void CWobblyModel::release() {
    // The constrained point stays constrained: it is the only thing pulling the
    // lattice back onto the window rect once the cursor lets go.
    m_grabbed = false;
}

void CWobblyModel::noteGeometry(const CBox& rect) {
    if (rect.y != m_resizeOriginalRect.y)
        m_wobbleTop = true;
    if (rect.x != m_resizeOriginalRect.x)
        m_wobbleLeft = true;
    if (rect.x + rect.w != m_resizeOriginalRect.x + m_resizeOriginalRect.w)
        m_wobbleRight = true;
    if (rect.y + rect.h != m_resizeOriginalRect.y + m_resizeOriginalRect.h)
        m_wobbleBottom = true;
}

void CWobblyModel::computeAccelerations(const SWobblyParams& params) {
    const double K = params.stiffness;

    for (int j = 0; j < GRID; ++j) {
        for (int i = 0; i < GRID; ++i) {
            const int K_IDX = index(i, j);

            if (m_constraint[K_IDX]) {
                m_acceleration[K_IDX] = (m_origin[K_IDX] - m_position[K_IDX]) * K;
                continue;
            }

            const auto POS   = m_position[K_IDX];
            Vector2D   accel = {};
            int        count = 0;

            // Horizontal neighbours hold the column spacing in x and the row
            // alignment in y; vertical neighbours do the mirror of that.
            if (i > 0) {
                const auto N = m_position[index(i - 1, j)];
                accel.x += (m_xLength - (POS.x - N.x)) * K;
                accel.y += (N.y - POS.y) * K;
                count++;
            }

            if (i < GRID - 1) {
                const auto N = m_position[index(i + 1, j)];
                accel.x += ((N.x - POS.x) - m_xLength) * K;
                accel.y += (N.y - POS.y) * K;
                count++;
            }

            if (j > 0) {
                const auto N = m_position[index(i, j - 1)];
                accel.y += (m_yLength - (POS.y - N.y)) * K;
                accel.x += (N.x - POS.x) * K;
                count++;
            }

            if (j < GRID - 1) {
                const auto N = m_position[index(i, j + 1)];
                accel.y += ((N.y - POS.y) - m_yLength) * K;
                accel.x += (N.x - POS.x) * K;
                count++;
            }

            m_acceleration[K_IDX] = count ? accel / sc<double>(count) : Vector2D{};
        }
    }
}

void CWobblyModel::smooth(SLattice& data) {
    // 3x3 mean where the centre carries as much weight as all its neighbours
    // together: (sum of n neighbours + n * self) / 2n.
    for (int j = 0; j < GRID; ++j) {
        for (int i = 0; i < GRID; ++i) {
            Vector2D sum   = {};
            int      count = 0;

            for (int dj = -1; dj <= 1; ++dj) {
                for (int di = -1; di <= 1; ++di) {
                    if (!di && !dj)
                        continue;

                    const int NI = i + di, NJ = j + dj;
                    if (NI < 0 || NI >= GRID || NJ < 0 || NJ >= GRID)
                        continue;

                    sum += data[index(NI, NJ)];
                    count++;
                }
            }

            const auto SELF        = data[index(i, j)];
            m_buffer[index(i, j)] = count ? (sum + SELF * sc<double>(count)) / (2.0 * count) : SELF;
        }
    }

    data = m_buffer;
}

void CWobblyModel::applyEdgeLocks() {
    // Pin every row/column except the one on the opposite side, so a locked
    // edge cannot deform along its own axis.
    if (!m_wobbleTop) {
        for (int i = 0; i < GRID; ++i)
            for (int j = 0; j < GRID - 1; ++j)
                m_position[index(i, j)].y = m_origin[index(i, j)].y;
    }

    if (!m_wobbleBottom) {
        for (int i = 0; i < GRID; ++i)
            for (int j = 1; j < GRID; ++j)
                m_position[index(i, j)].y = m_origin[index(i, j)].y;
    }

    if (!m_wobbleLeft) {
        for (int j = 0; j < GRID; ++j)
            for (int i = 0; i < GRID - 1; ++i)
                m_position[index(i, j)].x = m_origin[index(i, j)].x;
    }

    if (!m_wobbleRight) {
        for (int j = 0; j < GRID; ++j)
            for (int i = 1; i < GRID; ++i)
                m_position[index(i, j)].x = m_origin[index(i, j)].x;
    }
}

void CWobblyModel::step(float dtMs, const SWobblyParams& params, const CBox& rect) {
    computeOrigins(rect);
    computeAccelerations(params);
    smooth(m_acceleration);

    double accSum = 0;

    for (int k = 0; k < Lattice::COUNT; ++k) {
        auto acc = m_acceleration[k];
        fixVectorBounds(acc, params.minAcceleration, params.maxAcceleration);

        m_velocity[k] = acc * sc<double>(dtMs) + m_velocity[k] * sc<double>(params.drag);
        accSum += std::abs(acc.x) + std::abs(acc.y);
    }

    smooth(m_velocity);

    double velSum = 0;

    for (int k = 0; k < Lattice::COUNT; ++k) {
        fixVectorBounds(m_velocity[k], params.minVelocity, params.maxVelocity);
        m_position[k] += m_velocity[k] * sc<double>(dtMs * params.moveFactor);
        velSum += std::abs(m_velocity[k].x) + std::abs(m_velocity[k].y);
    }

    applyEdgeLocks();

    m_wobbling = !(accSum < params.stopAcceleration && velSum < params.stopVelocity);
}

SLattice CWobblyModel::offsets() const {
    SLattice out;

    for (int k = 0; k < Lattice::COUNT; ++k) {
        out[k] = m_position[k] - m_origin[k];
    }

    return out;
}

CBox CWobblyModel::controlBounds() const {
    return latticeBounds(m_position);
}
