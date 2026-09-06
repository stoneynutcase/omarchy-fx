#pragma once

#include "WobblyTransformer.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace OmarchyFX {

    // Settings written by the Omarchy shell plugin's panel. Anything left unset
    // falls through to the Hyprland config value, so configuring in Lua and
    // configuring from the bar both keep working. The panel reads this same file
    // back for its own state; the plugin re-reads it on every config reload.
    struct SOverrides {
        std::optional<bool>  enabled, onMove, onResize;
        std::optional<int>   wobbliness, tessellation;
        std::optional<float> stiffness, drag, moveFactor;
    };

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

        // Settings file, shared with the shell plugin.
        static std::string settingsPath();
        void               loadSettings();

        bool                             m_configOk = false;
        SOverrides                       m_overrides;
        std::vector<SEntry>              m_entries;
        std::vector<CHyprSignalListener> m_listeners;
    };

    inline UP<CWobblyManager> g_wobbly;
}
