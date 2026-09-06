// SPDX-License-Identifier: GPL-2.0-or-later
//
// The wobble physics here is a port of KWin's wobblywindows effect:
// https://github.com/KDE/kwin/tree/master/src/plugins/wobblywindows
// SPDX-FileCopyrightText: 2008 Cedric Borgese <cedric.borgese@gmail.com>

#pragma once

#include <hyprland/src/helpers/math/Math.hpp>

#include <array>

namespace OmarchyFX {

    // The parameters of KWin's wobblywindows effect, same names and same units.
    // Time is in milliseconds, distances in logical pixels.
    struct SWobblyParams {
        float stiffness  = 0.10F;
        float drag       = 0.85F;
        float moveFactor = 0.10F;

        float minVelocity      = 0.F;
        float maxVelocity      = 1000.F;
        float stopVelocity     = 0.5F;
        float minAcceleration  = 0.F;
        float maxAcceleration  = 1000.F;
        float stopAcceleration = 0.5F;

        int   tessellation = 20; // render mesh quads per axis
    };

    // KWin's five "wobbliness" presets, 0 (least) to 4 (most).
    SWobblyParams wobblyPreset(int level);

    // A port of KWin's wobblywindows physics.
    //
    // The simulation runs on a fixed 4x4 lattice of control points, which the
    // renderer reads as the control net of a bicubic Bezier surface. That is
    // what keeps the deformation smooth: a coarse lattice carries the physics,
    // and the surface between the points is a cubic, not a fold.
    //
    // Points are held together only by springs to their neighbours; nothing
    // pulls a point towards where the window says it should be. The one
    // exception is the "constrained" point — the one the cursor grabbed, which
    // is pulled towards its own rest position and drags the whole net with it.
    // That single anchor is also what lets the lattice find its way back to the
    // window rect after the button is released, so it is deliberately never
    // cleared.
    //
    // After each pass the acceleration and then the velocity fields are run
    // through a 3x3 mean filter (KWin's heightRingLinearMean). Without it the
    // motion stays bunched up around the grabbed point; with it the wobble
    // spreads across the whole window.
    class CWobblyModel {
      public:
        static constexpr int GRID  = 4;
        static constexpr int COUNT = GRID * GRID;

        // Snaps the lattice flat onto rect and forgets all motion.
        void reset(const CBox& rect);

        // Constrains the lattice point nearest the cursor.
        void grab(const CBox& rect, const Vector2D& cursor, bool resizing);

        void release();

        bool grabbed() const {
            return m_grabbed;
        }

        bool wobbling() const {
            return m_wobbling;
        }

        // True once the window may be left alone again.
        bool finished() const {
            return !m_grabbed && !m_wobbling;
        }

        // During a resize, an edge only starts wobbling once it has actually
        // moved away from where the resize began.
        void noteGeometry(const CBox& rect);

        // One integration pass. dtMs is clamped to KWin's 10 ms step by the caller.
        void     step(float dtMs, const SWobblyParams& params, const CBox& rect);

        // position - origin for control point (i, j).
        Vector2D controlOffset(int i, int j) const;

        // Conservative bounds of the deformed surface: a Bezier surface stays
        // inside the convex hull of its control net.
        CBox     controlBounds() const;

      private:
        void     computeOrigins(const CBox& rect);
        void     computeAccelerations(const SWobblyParams& params);
        void     smooth(std::array<Vector2D, COUNT>& data);
        void     applyEdgeLocks();

        static int index(int i, int j) {
            return j * GRID + i;
        }

        std::array<Vector2D, COUNT> m_origin{};
        std::array<Vector2D, COUNT> m_position{};
        std::array<Vector2D, COUNT> m_velocity{};
        std::array<Vector2D, COUNT> m_acceleration{};
        std::array<Vector2D, COUNT> m_buffer{};
        std::array<bool, COUNT>     m_constraint{};

        double                      m_xLength = 0, m_yLength = 0;

        bool                        m_grabbed  = false;
        bool                        m_wobbling = false;

        bool                        m_wobbleTop = true, m_wobbleLeft = true, m_wobbleRight = true, m_wobbleBottom = true;
        CBox                        m_resizeOriginalRect;
    };
}
