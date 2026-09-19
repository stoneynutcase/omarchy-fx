#pragma once

#include "ElasticModel.hpp"
#include "MeshTransformer.hpp"
#include "PulseModel.hpp"
#include "ShakeCursor.hpp"
#include "WobblyModel.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>

#include <chrono>
#include <filesystem>
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

        std::optional<bool>  elasticEnabled, elasticOnTiled, elasticOnFloating, elasticOnWorkspace;
        std::optional<int>   stretchiness, elasticTessellation;
        std::optional<float> elasticPeriod, elasticDamping, elasticTilt, elasticFollow, elasticMaxStretch;

        std::optional<bool>  pulseEnabled, pulseOnSwitch, pulseOnClick, pulseOnHover;
        std::optional<int>   pulseStrength, pulseTessellation;
        std::optional<float> pulseAmount, pulsePeriod, pulseDamping;

        std::optional<bool>  shakeEnabled;
    };

    // Drives every effect in the plugin. A window gets at most one entry, and an
    // entry carries one mesh, one transformer and whichever simulations are
    // running on it.
    //
    // The effects are mutually exclusive on a given window, and the wobble
    // wins: while you are dragging something, that drag is the only thing that
    // should be deforming it. The other two let whichever is running finish.
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

            CPulseModel                           pulse;
            SPulseParams                          pulseParams;
            bool                                  pulseOn = false;
        };

        // Wobble: whichever window the pointer is dragging.
        void           syncDrag();
        void           beginWobble(const PHLWINDOW& window, const Vector2D& cursor, bool resizing);

        // Elasticity: whichever windows Hyprland is animating for us.
        void           scanAnimations(const PHLMONITOR& monitor);

        // ...plus the one case there is nothing to watch for. A window carried
        // to another workspace is slid there by the workspace's render offset,
        // so its own geometry never moves and the event is the only signal.
        void           noteWorkspaceMove(const PHLWINDOW& window);
        bool           pendingWorkspaceMove(const PHLWINDOW& window);
        void           clearWorkspaceMove(const PHLWINDOW& window);

        // Pulse: whichever window just became the active one.
        void           onFocus(const PHLWINDOW& window, Desktop::eFocusReason reason);

        void           tick(const PHLMONITOR& monitor);

        SEntry*        find(const PHLWINDOW& window);
        SEntry*        ensureEntry(const PHLWINDOW& window);
        void           detach(SEntry& entry);

        SWobblyParams  wobblyParams() const;
        SElasticParams elasticParams() const;
        SPulseParams   pulseParams() const;

        // Settings file, shared with the shell plugin.
        static std::string configHome();
        static std::string settingsPath();
        void               loadSettings();

        // On an Omarchy shell the bar widget is the switch: `omarchy plugin
        // disable` or `remove` takes it off the bar, and that alone must stop
        // the effects — nothing in the shell can reach the compositor to say so.
        // So the compositor watches the shell's bar layout instead. Without an
        // Omarchy shell this is always true and the config stays in charge.
        bool                                  shellAllows();
        std::chrono::steady_clock::time_point m_shellCheckedAt{};
        std::filesystem::file_time_type       m_shellLayoutStamp{};
        bool                                  m_shellWidgetInstalled = false;
        bool                                  m_shellEvaluated       = false;
        bool                                  m_shellAllows          = true;

        struct SPendingMove {
            PHLWINDOWREF                          window;
            std::chrono::steady_clock::time_point at;
        };

        bool                             m_configOk = false;
        std::vector<SPendingMove>        m_pendingMoves;
        SOverrides                       m_overrides;

        // Hyprland reports the active window on every focus pass, including
        // ones that land on the window that already had it.
        PHLWINDOWREF                     m_lastActive;

        // The one effect here that is not a window effect. Fed from the same
        // listeners; owns nothing of the mesh.
        CShakeCursor                     m_shake;
        void                             syncShake();

        // A list, not a vector: find() and ensureEntry() hand out pointers into
        // it and entries are added while others are being walked.
        std::list<SEntry>                m_entries;
        std::vector<CHyprSignalListener> m_listeners;
    };

    inline UP<CEffectManager> g_fx;
}
