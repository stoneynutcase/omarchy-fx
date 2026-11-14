#pragma once

// One source tree, two Hyprland APIs. Omarchy's stable channel ships 0.56 and
// edge ships 0.57, and the plugin is built against whichever headers the
// hyprland package installed — so it has to compile against both.
//
// 0.57 moved CWindow into desktop/view/window/, replaced CWorkspace with the
// workspace/ hierarchy, put window state behind accessors, moved transformers
// into the window's effects controller with a new interface, and gave the
// logger a call-site argument. The switch is a header only 0.57 has.

#if __has_include(<hyprland/src/workspace/HLWorkspace.hpp>)
#define OMARCHY_FX_HL057 1
#endif

#include <hyprland/src/debug/log/Logger.hpp>

#ifdef OMARCHY_FX_HL057
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowEffectsController.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/render/transformer/TransformerList.hpp>
#else
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#endif

// 0.57's log() takes the caller's name before the format string, and its LOG
// macro supplies it. Calling log() the 0.56 way there binds the format string
// as the name and the first argument as the format.
#ifdef OMARCHY_FX_HL057
#define FX_LOG(level, fmt, ...) LOG(level, fmt __VA_OPT__(, ) __VA_ARGS__)
#else
#define FX_LOG(level, fmt, ...) Log::logger->log(level, fmt __VA_OPT__(, ) __VA_ARGS__)
#endif

namespace OmarchyFX::Compat {

    inline bool mapped(const PHLWINDOW& w) {
#ifdef OMARCHY_FX_HL057
        return w->mapped();
#else
        return w->m_isMapped;
#endif
    }

    inline bool animatingIn(const PHLWINDOW& w) {
#ifdef OMARCHY_FX_HL057
        return w->presentation().animatingIn();
#else
        return w->m_animatingIn;
#endif
    }

    inline bool floating(const PHLWINDOW& w) {
#ifdef OMARCHY_FX_HL057
        return w->isFloating();
#else
        return w->m_isFloating;
#endif
    }
}
