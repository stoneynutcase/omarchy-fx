#pragma once

#include "Mesh.hpp"

#include <hyprland/src/render/transformer/Transformer.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <vector>

namespace OmarchyFX {

    // Renders the window's offscreen framebuffer through the deformed lattice.
    // It knows nothing about which effect deformed it — only about SMeshState.
    class CMeshTransformer : public Render::IWindowTransformer {
      public:
        CMeshTransformer(SP<SMeshState> state);
        virtual ~CMeshTransformer();

        virtual SP<Render::IFramebuffer> transform(SP<Render::IFramebuffer> in);
        virtual void                     preWindowRender(CSurfacePassElement::SRenderData* pRenderData);
        virtual void                     amendTransformedRenderData(const CBox& currentBox, SMotionBlurData* pMotionBlurData);

        static void                      dropProgram();

      private:
        bool                  ensureProgram();

        SP<SMeshState>        m_state;

        // Window bounding box for this frame, monitor-local and unscaled, as
        // handed to us right before the transformed pass is submitted.
        CBox                  m_currentBox;
        bool                  m_boxValid = false;

        std::vector<float>    m_verts; // x, y, u, v per vertex
        std::vector<Vector2D> m_grid;
    };
}
