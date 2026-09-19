#pragma once

#include "Mesh.hpp"

namespace OmarchyFX {

    // Time is in milliseconds, distances in logical pixels.
    struct SPulseParams {
        // How far the edges swell out at the peak of the pulse.
        float amount = 9.F; // px

        // The swell is one damped oscillation: it overshoots to `amount`, dips
        // back through the rect and settles. Period and damping ratio, as with
        // the elastic effect, so speed and ringing stay independent.
        float period  = 260.F; // ms for one full oscillation
        float damping = 0.55F; // 1 is critical, lower rings more

        // How much rounder than a plain scale the swell is. At 0 the window
        // scales uniformly about its centre; at 1 the edge midpoints push out
        // and the corners stay put, so the sides bow like a cushion.
        float bulge = 0.35F;

        int   tessellation = 20; // render mesh quads per axis
    };

    // Five presets, least to most lively.
    SPulseParams pulsePreset(int level);

    // A single swell when a window becomes active: the lattice pushes outward
    // from the centre, overshoots, and rings down onto the rect. One scalar
    // oscillator drives every control point, so this is the cheapest thing in
    // the plugin — there is no per-point state at all.
    //
    // Unlike the other two models nothing external moves the rect during the
    // effect; if something does, the origins are re-read every step and the
    // swell simply rides along.
    class CPulseModel {
      public:
        // Starts a pulse from the rest position with the impulse that peaks at
        // params.amount, given the params' period and damping.
        void     arm(const CBox& rect, const SPulseParams& params);

        // One integration pass, in fixed substeps.
        void     step(float dtMs, const SPulseParams& params, const CBox& rect);

        // True once the swell has rung down and the lattice can be dropped.
        bool     settled() const;

        // Displacement of every control point from its rest position.
        SLattice offsets(const SPulseParams& params) const;

        // Conservative bounds of the deformed surface.
        CBox     controlBounds(const SPulseParams& params) const;

      private:
        SLattice m_origin{};
        CBox     m_rect{};

        // The one oscillator: how far the edges currently sit off the rect, and
        // how fast that is changing.
        double   m_x = 0; // px
        double   m_v = 0; // px/ms
    };
}
