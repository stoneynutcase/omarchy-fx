#pragma once

#include "ElasticModel.hpp"
#include "MeshTransformer.hpp"
#include "WobblyModel.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>

#include <chrono>
#include <list>
#include <optional>
#include <string>
#include <vector>

namespace OmarchyFX {

    // Settings written by the Omarchy shell plugin's panel. Anything left unset
    // falls through to the Hyprland config value, so configuring in Lua and
    // configuring from the bar both keep working. The panel reads this same file
    // back for its own state; the plugin re-reads it on every config reload.
    struct SOverrides {
        std::optional<bool>  wobblyEnabled, wobblyOnMove, wobblyOnResize;
        std::optional<int>   wobbliness, wobblyTessellation;
        std::optional<float> wobblyStiffness, wobblyDrag, wobblyMoveFactor;

        std::optional<bool>  elasticEnabled, elasticOnTiled, elasticOnFloating;
        std::optional<int>   stretchiness, elasticTessellation;
        std::optional<float> elasticPeriod, elasticDamping, elasticTilt, elasticFollow, elasticMaxStretch;
    };

    // Drives every effect in the plugin. A window gets at most one entry, and an
    // entry carries one mesh, one transformer and whichever simulations are
    // running on it.
    //
    // The two effects are mutually exclusive on a given window, and the wobble
    // wins: while you are dragging something, that drag is the only thing that
    // should be deforming it.
    class CEffectManager {
      public:
        void registerConfig();
        void init();
        void shutdown();

      private:
        struct SEntry {
            PHLWINDOWREF                          window;
            SP<SMeshState>                        state;
            Render::IWindowTransformer*           transformer = nullptr;
            CBox                                  lastBounds;
            std::chrono::steady_clock::time_point lastTick;

            CWobblyModel                          wobbly;
            SWobblyParams                         wobblyParams;
            bool                                  wobblyOn = false;

            CElasticModel                         elastic;
            SElasticParams                        elasticParams;
            bool                                  elasticOn = false;
        };

        // Wobble: whichever window the pointer is dragging.
        void           syncDrag();
        void           beginWobble(const PHLWINDOW& window, const Vector2D& cursor, bool resizing);

        // Elasticity: whichever windows Hyprland is animating for us.
        void           scanAnimations(const PHLMONITOR& monitor);

        void           tick(const PHLMONITOR& monitor);

        SEntry*        find(const PHLWINDOW& window);
        SEntry*        ensureEntry(const PHLWINDOW& window);
        void           detach(SEntry& entry);

        SWobblyParams  wobblyParams() const;
        SElasticParams elasticParams() const;

        // Settings file, shared with the shell plugin.
        static std::string settingsPath();
        void               loadSettings();

        bool                             m_configOk = false;
        SOverrides                       m_overrides;

        // A list, not a vector: find() and ensureEntry() hand out pointers into
        // it and entries are added while others are being walked.
        std::list<SEntry>                m_entries;
        std::vector<CHyprSignalListener> m_listeners;
    };

    inline UP<CEffectManager> g_fx;
}
