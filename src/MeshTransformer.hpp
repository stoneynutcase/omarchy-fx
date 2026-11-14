#pragma once

#include "Mesh.hpp"
#include "compat.hpp"

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

#ifdef OMARCHY_FX_HL057
        // 0.57 hands us a framebuffer cut to the window, with its place on the
        // monitor, and only keeps what lands inside transformedExtents(). The
        // deformed mesh is drawn by Hyprland's own textured-mesh renderer.
        virtual Render::SWindowTransformBuffer transform(Render::CRenderContext& ctx, const Render::SWindowTransformBuffer& in,
                                                         const Render::SWindowTransformContext& context);
        virtual bool                           active() const;
        virtual CBox                           transformedExtents(const CBox& currentBox) const;
#else
        virtual SP<Render::IFramebuffer> transform(SP<Render::IFramebuffer> in);
        virtual void                     preWindowRender(CSurfacePassElement::SRenderData* pRenderData);
        virtual void                     amendTransformedRenderData(const CBox& currentBox, SMotionBlurData* pMotionBlurData);
#endif

        static void dropProgram();

      private:
        // Evaluates the Bezier displacement at every render-mesh vertex laid
        // over box (the window's full bounding box) into m_grid, and returns
        // the largest component. Shared by both render paths.
        double                        evaluateGrid(const CBox& box, int tess) const;

        SP<SMeshState>                m_state;
        mutable std::vector<Vector2D> m_grid;

#ifndef OMARCHY_FX_HL057
        bool ensureProgram();

        // Window bounding box for this frame, monitor-local and unscaled, as
        // handed to us right before the transformed pass is submitted.
        CBox               m_currentBox;
        bool               m_boxValid = false;

        std::vector<float> m_verts; // x, y, u, v per vertex
#endif
    };
}
