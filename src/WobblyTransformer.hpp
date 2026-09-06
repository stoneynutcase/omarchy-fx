#pragma once

#include "WobblyModel.hpp"

#include <hyprland/src/render/transformer/Transformer.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <vector>

namespace OmarchyFX {

    // Shared between the manager (which owns the simulation) and the transformer
    // (which lives inside CWindow::m_transformers and dies with the window).
    struct SWobblyState {
        CWobblyModel  model;
        SWobblyParams params;

        // The lattice is defined over the window frame, not over the full
        // bounding box that gets rendered — shadows and borders should ride
        // along with the window rather than dilute the deformation. These two
        // place the frame inside the bounding box the renderer hands us.
        Vector2D      frameOffsetInBox;
        Vector2D      frameSize;

        bool          alive = true;
    };

    // Renders the window's offscreen framebuffer through the deformed lattice.
    class CWobblyTransformer : public Render::IWindowTransformer {
      public:
        CWobblyTransformer(SP<SWobblyState> state);
        virtual ~CWobblyTransformer();

        virtual SP<Render::IFramebuffer> transform(SP<Render::IFramebuffer> in);
        virtual void                     preWindowRender(CSurfacePassElement::SRenderData* pRenderData);
        virtual void                     amendTransformedRenderData(const CBox& currentBox, SMotionBlurData* pMotionBlurData);

        static void                      dropProgram();

      private:
        bool               ensureProgram();

        SP<SWobblyState>   m_state;

        // Window bounding box for this frame, monitor-local and unscaled, as
        // handed to us right before the transformed pass is submitted.
        CBox               m_currentBox;
        bool               m_boxValid = false;

        std::vector<float> m_verts; // x, y, u, v per vertex
        std::vector<Vector2D> m_grid;
    };
}
