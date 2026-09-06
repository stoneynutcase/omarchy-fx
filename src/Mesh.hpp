#pragma once

#include <hyprland/src/helpers/math/Math.hpp>

#include <array>

namespace OmarchyFX {

    // Every effect in this plugin deforms the same thing: a 4x4 net of control
    // points laid over the window frame, which CMeshTransformer reads as the
    // control net of a bicubic Bezier surface. Physics models differ; what they
    // hand the renderer does not.
    namespace Lattice {
        inline constexpr int GRID  = 4;
        inline constexpr int COUNT = GRID * GRID;

        inline constexpr int index(int i, int j) {
            return j * GRID + i;
        }
    }

    // Displacement of each control point from its rest position, in logical
    // pixels. All zeroes means "undeformed", which the renderer short-circuits.
    using SLattice = std::array<Vector2D, Lattice::COUNT>;

    // Rest positions of the lattice over rect. The last column/row lands exactly
    // on the far edge rather than accumulating the increment, so the lattice
    // matches the rect however it divides.
    inline void latticeOrigins(const CBox& rect, SLattice& out) {
        const double XLEN = rect.w / (Lattice::GRID - 1.0);
        const double YLEN = rect.h / (Lattice::GRID - 1.0);

        for (int j = 0; j < Lattice::GRID; ++j) {
            for (int i = 0; i < Lattice::GRID; ++i) {
                out[Lattice::index(i, j)] = Vector2D{i == Lattice::GRID - 1 ? rect.x + rect.w : rect.x + i * XLEN, //
                                                     j == Lattice::GRID - 1 ? rect.y + rect.h : rect.y + j * YLEN};
            }
        }
    }

    // Conservative bounds of a deformed surface: a Bezier surface stays inside
    // the convex hull of its control net.
    inline CBox latticeBounds(const SLattice& points) {
        Vector2D min = points[0], max = points[0];

        for (const auto& p : points) {
            min.x = std::min(min.x, p.x);
            min.y = std::min(min.y, p.y);
            max.x = std::max(max.x, p.x);
            max.y = std::max(max.y, p.y);
        }

        return CBox{min.x, min.y, max.x - min.x, max.y - min.y};
    }

    // Shared between the manager (which owns the physics) and the transformer
    // (which lives inside CWindow::m_transformers and dies with the window).
    struct SMeshState {
        SLattice offsets{};

        // The lattice is defined over the window frame, not over the full
        // bounding box that gets rendered — shadows and borders should ride
        // along with the window rather than dilute the deformation. These two
        // place the frame inside the bounding box the renderer hands us.
        Vector2D frameOffsetInBox;
        Vector2D frameSize;

        int      tessellation = 20; // render mesh quads per axis

        bool     alive = true;
    };
}
