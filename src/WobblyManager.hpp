#pragma once

#include "WobblyTransformer.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>

#include <chrono>
#include <vector>

namespace OmarchyFX {

    // Watches Hyprland's drag controller and keeps a wobble simulation running
    // for whichever window is being dragged, plus any window still settling.
    class CWobblyManager {
      public:
        void registerConfig();
        void init();
        void shutdown();

      private:
        struct SEntry {
            PHLWINDOWREF                          window;
            SP<SWobblyState>                      state;
            Render::IWindowTransformer*           transformer = nullptr;
            CBox                                  lastBounds;
            std::chrono::steady_clock::time_point lastTick;
        };

        void          syncDrag();
        void          tick(const PHLMONITOR& monitor);
        SEntry*       find(const PHLWINDOW& window);
        void          begin(const PHLWINDOW& window, const Vector2D& cursor, bool resizing);
        void          detach(SEntry& entry);
        SWobblyParams readParams() const;

        bool                             m_configOk = false;
        std::vector<SEntry>              m_entries;
        std::vector<CHyprSignalListener> m_listeners;
    };

    inline UP<CWobblyManager> g_wobbly;
}
