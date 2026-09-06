#pragma once

#include "Mesh.hpp"

namespace OmarchyFX {

    // Time is in milliseconds, distances in logical pixels.
    struct SElasticParams {
        // The lattice points are damped harmonic oscillators anchored to their
        // rest positions. Stating the spring as a period and a damping ratio
        // rather than as a stiffness keeps the two things you actually care
        // about — how fast it snaps back, and how much it rings — independent
        // of each other.
        float period  = 180.F; // ms for one full oscillation
        float damping = 0.55F; // damping ratio: 1 is critical, below that rings

        // How much stiffer the leading edge is than the trailing one, 0 to 0.95.
        // This is what turns a lag into a stretch: with no tilt every point lags
        // by the same amount and the window merely trails, rigid.
        float tilt = 0.65F;

        // How much of the bulk lag survives into what gets drawn, 0 to 1. At 0
        // the window stays put on its own rect and only squashes and stretches;
        // at 1 it also trails behind the animation and overshoots the target.
        float follow = 0.55F;

        // How far a control point may be dragged from its rest position, in
        // pixels. The spring stiffens towards this limit rather than clipping
        // at it, so a fast animation saturates gracefully — and it does
        // saturate: Hyprland moves a swapped window at better than 10 px/ms,
        // far quicker than a spring soft enough to visibly ring can follow.
        float maxStretch = 65.F;

        // Speed at which the tilt is fully applied, so a slow reflow stretches
        // less than a fast swap.
        float refSpeed = 4.F; // px/ms

        int   tessellation = 20; // render mesh quads per axis
    };

    // Five presets, least to most rubbery.
    SElasticParams elasticPreset(int level);

    // Rubber-band physics for windows whose geometry is being animated by
    // something other than the pointer — a tiled swap, a layout reflow, a
    // keyboard resize.
    //
    // The same 4x4 control lattice as the wobble, but a different mechanism.
    // Every point is a damped spring anchored to its own rest position on the
    // window rect. Nothing is grabbed and no point is pulled by a neighbour:
    // the deformation comes purely from inertia. When Hyprland animates the
    // rect out from under the lattice, the points cannot follow instantly, so
    // they trail; when the rect stops, they overshoot and settle.
    //
    // A uniform lattice of identical springs would only ever lag as a rigid
    // block. The stretch comes from tilting the spring stiffness along the
    // direction of travel — the leading edge is anchored hard and keeps up, the
    // trailing edge is anchored softly and drags behind. A resize needs no tilt
    // to deform: there the rest positions themselves move by different amounts.
    class CElasticModel {
      public:
        // Snaps the lattice flat onto rect and starts a run.
        void arm(const CBox& rect);

        bool running() const {
            return m_running;
        }

        // Once per frame, before the integration substeps: tracks how fast the
        // window rect is travelling, which is what sets the direction and the
        // depth of the stretch. `animating` says whether Hyprland is animating
        // the geometry; a big jump while it is not is a teleport, not a move.
        void     advance(const CBox& rect, float frameDtMs, bool animating);

        // One integration pass, in fixed substeps so the effect looks the same
        // at 60 and 240 Hz.
        void     step(float dtMs, const SElasticParams& params, const CBox& rect);

        // True once the lattice is back on the rect and can be dropped.
        bool     settled() const;

        // Displacement of every control point from its rest position, with the
        // params' `follow` share of the bulk lag applied.
        SLattice offsets(const SElasticParams& params) const;

        // Conservative bounds of the deformed surface.
        CBox     controlBounds(const SElasticParams& params) const;

      private:
        SLattice m_origin{};
        SLattice m_position{};
        SLattice m_velocity{};

        // Smoothed velocity of the window rect itself, px/ms. Smoothed because
        // a single frame's delta is noisy at high refresh rates, and this sets
        // which way the window stretches.
        Vector2D m_travel{};
        Vector2D m_lastCenter{};

        bool     m_running = false;
        bool     m_seeded  = false;
    };
}
