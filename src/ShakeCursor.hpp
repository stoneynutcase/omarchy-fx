#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/managers/XCursorManager.hpp>

#include <aquamarine/buffer/Buffer.hpp>
#include <hyprcursor/hyprcursor.hpp>

#include <chrono>
#include <deque>
#include <string>
#include <vector>

namespace OmarchyFX {

    // KDE Plasma's "Shake Cursor": shake the pointer and it grows until you
    // stop, so you can find it on a big screen. Not a window effect at all,
    // hence its own module. It does not touch the mesh.
    //
    // How: Hyprland's pointer manager shows whatever cursor image it is handed
    // last, sized as `buffer size / scale`. While a shake is on, this hands it
    // a bigger rendition of the current shape from the cursor theme with a
    // scale that lands on the wanted logical size, and hands the original back
    // when the shake is over. Software cursors are locked meanwhile, because
    // a hardware cursor plane has a size cap the magnified image would exceed.
    class CShakeCursor {
      public:
        CShakeCursor();
        ~CShakeCursor(); // out of line: the worker is an incomplete type here

        void init();
        void shutdown();

        // Master switch. Turning it off mid-shake restores the cursor.
        void setEnabled(bool enabled);

        // Feed: every pointer motion, and once per frame.
        void onMove();
        void tick();

      private:
        struct SSample {
            std::chrono::steady_clock::time_point at;
            Vector2D                              pos;
        };

        // KWin's ShakeDetector, ported: over the last second of motion, the
        // trail's length divided by the diagonal of its bounding box. A sweep
        // scores about 1, a shake scores high. True once per shake.
        bool   detect(std::chrono::steady_clock::time_point now, const Vector2D& pos);

        // KWin's inflate / deflate: tween the magnification to a new target.
        void   animateTo(float target, std::chrono::steady_clock::time_point now);

        void   apply(float zoom);
        void   captureOriginal();
        void   restoreOriginal();

        // The magnified rendition for this zoom, into m_bigBuffer. From the
        // hyprcursor theme at the exact pixel size when there is one (SVG, so
        // sharp at any size), else the largest raster the Xcursor theme ships.
        bool   ensureBigImage(float zoom);
        bool   renderVector(int px);
        bool   renderRaster();
        double outputScale() const;
        void   probeHyprcursor();

        // How far the cursor may grow: unbounded with a vector theme, and on
        // a raster theme a little past its largest image, so it never balloons
        // into a blur.
        float  maxZoom();

        // Vector renders happen on a worker thread with its own hyprcursor
        // instance: loading a style renders the whole theme, dozens of SVGs,
        // and at 240 px that is over a hundred milliseconds — far too long to
        // stall the compositor for. Results come back through drainRenders().
        struct SWorker;
        void   ensureWorker();
        void   requestRender(const std::string& shape, int px);
        void   drainRenders();

        std::deque<SSample>                   m_history;
        std::chrono::steady_clock::time_point m_lastShake{};

        bool                                  m_enabled = true;
        bool                                  m_active  = false; // our image is the one showing
        bool                                  m_locked  = false; // software cursors locked by us

        // The tween: from m_animFrom at m_animStart to m_target.
        float                                 m_zoom      = 1.F;
        float                                 m_target    = 1.F;
        float                                 m_animFrom  = 1.F;
        std::chrono::steady_clock::time_point m_animStart{};
        float                                 m_applied   = 1.F;

        // What Hyprland was showing before we stepped in, to hand back.
        SP<Aquamarine::IBuffer>               m_origBuffer;
        WP<Desktop::View::CWLSurface>         m_origSurface;
        Vector2D                              m_origHotspot;
        Vector2D                              m_origSize;
        float                                 m_origScale = 1.F;

        // The magnified rendition. Vector when the theme allows, raster else.
        UP<Hyprcursor::CHyprcursorManager>    m_hyprcursor;
        bool                                  m_hyprcursorTried = false;
        UP<CXCursorManager>                   m_xcursor;
        std::string                           m_shape = "default"; // last shape a client asked for
        std::string                           m_bigShape;
        int                                   m_bigPx = 0; // 0: raster, else the vector render size
        SP<Aquamarine::IBuffer>               m_bigBuffer;
        Vector2D                              m_bigSize;
        Vector2D                              m_bigHotspot;

        // Finished renders, kept, so a size is rendered once per shape and the
        // next shake at it is free.
        struct SRender {
            std::string             shape;
            int                     px = 0;
            SP<Aquamarine::IBuffer> buffer;
            Vector2D                size;
            Vector2D                hotspot;
        };
        std::vector<SRender>                  m_renders;
        std::vector<std::pair<std::string, int>> m_pending; // asked of the worker, not back yet
        SP<Aquamarine::IBuffer>               m_appliedBuffer;
        UP<SWorker>                           m_worker;

        std::vector<CHyprSignalListener>      m_listeners;
    };
}
