#include "WobblyManager.hpp"
#include "globals.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>

using namespace OmarchyFX;

static constexpr const char* CFG_ENABLED     = "plugin:omarchy-fx:wobbly_enabled";
static constexpr const char* CFG_ON_MOVE     = "plugin:omarchy-fx:wobbly_on_move";
static constexpr const char* CFG_ON_RESIZE   = "plugin:omarchy-fx:wobbly_on_resize";
static constexpr const char* CFG_WOBBLINESS  = "plugin:omarchy-fx:wobbly_wobbliness";
static constexpr const char* CFG_STIFFNESS   = "plugin:omarchy-fx:wobbly_stiffness";
static constexpr const char* CFG_DRAG        = "plugin:omarchy-fx:wobbly_drag";
static constexpr const char* CFG_MOVE_FACTOR = "plugin:omarchy-fx:wobbly_move_factor";
static constexpr const char* CFG_TESSELATION = "plugin:omarchy-fx:wobbly_tessellation";

// KWin integrates in fixed 10 ms steps and so do we, so the wobble looks the
// same at 60 and 240 Hz. The cap keeps a stall from firing hundreds of steps.
static constexpr float INTEGRATION_STEP_MS = 10.F;
static constexpr float MAX_FRAME_MS        = 100.F;

void CWobblyManager::registerConfig() {
    // Reading a value that failed to register aborts Hyprland, so every later
    // config read is gated on all of these having landed.
    m_configOk = true;
    m_configOk &= HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<Config::Values::CBoolValue>(CFG_ENABLED, "enable the wobbly windows effect", true));
    m_configOk &= HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<Config::Values::CBoolValue>(CFG_ON_MOVE, "wobble while dragging a window around", true));
    m_configOk &= HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<Config::Values::CBoolValue>(CFG_ON_RESIZE, "wobble while resize-dragging a window", true));
    m_configOk &= HyprlandAPI::addConfigValueV2(
        PHANDLE, makeShared<Config::Values::CIntValue>(CFG_WOBBLINESS, "wobbliness preset, 0 (least) to 4 (most)", 1, Config::Values::SIntValueOptions{.min = 0, .max = 4}));
    m_configOk &= HyprlandAPI::addConfigValueV2(PHANDLE,
                                                makeShared<Config::Values::CFloatValue>(CFG_STIFFNESS, "override the preset's spring stiffness, -1 to keep it", -1.F,
                                                                                        Config::Values::SFloatValueOptions{.min = -1.F, .max = 1.F}));
    m_configOk &= HyprlandAPI::addConfigValueV2(PHANDLE,
                                                makeShared<Config::Values::CFloatValue>(CFG_DRAG, "override the preset's drag, -1 to keep it", -1.F,
                                                                                        Config::Values::SFloatValueOptions{.min = -1.F, .max = 1.F}));
    m_configOk &= HyprlandAPI::addConfigValueV2(PHANDLE,
                                                makeShared<Config::Values::CFloatValue>(CFG_MOVE_FACTOR, "override the preset's move factor, -1 to keep it", -1.F,
                                                                                        Config::Values::SFloatValueOptions{.min = -1.F, .max = 1.F}));
    m_configOk &= HyprlandAPI::addConfigValueV2(
        PHANDLE, makeShared<Config::Values::CIntValue>(CFG_TESSELATION, "render mesh quads per axis", 20, Config::Values::SIntValueOptions{.min = 2, .max = 64}));

    if (!m_configOk)
        Log::logger->log(Log::ERR, "[omarchy-fx] failed to register config values, staying inert");
}

std::string CWobblyManager::settingsPath() {
    const auto XDG = getenv("XDG_CONFIG_HOME");
    if (XDG && *XDG)
        return std::string{XDG} + "/omarchy/omarchy-fx.conf";

    const auto HOME = getenv("HOME");
    return std::string{HOME ? HOME : ""} + "/.config/omarchy/omarchy-fx.conf";
}

void CWobblyManager::loadSettings() {
    m_overrides = {};

    std::ifstream file(settingsPath());
    if (!file.good())
        return;

    const auto asBool = [](const std::string& v) { return v == "true" || v == "1" || v == "yes"; };

    std::string line;
    while (std::getline(file, line)) {
        const auto HASH = line.find('#');
        if (HASH != std::string::npos)
            line = line.substr(0, HASH);

        const auto EQ = line.find('=');
        if (EQ == std::string::npos)
            continue;

        auto key   = line.substr(0, EQ);
        auto value = line.substr(EQ + 1);

        const auto TRIM = [](std::string& s) {
            const auto FIRST = s.find_first_not_of(" \t\r\n");
            const auto LAST  = s.find_last_not_of(" \t\r\n");
            s                = FIRST == std::string::npos ? "" : s.substr(FIRST, LAST - FIRST + 1);
        };
        TRIM(key);
        TRIM(value);

        if (key.empty() || value.empty())
            continue;

        try {
            if (key == "enabled")
                m_overrides.enabled = asBool(value);
            else if (key == "on_move")
                m_overrides.onMove = asBool(value);
            else if (key == "on_resize")
                m_overrides.onResize = asBool(value);
            else if (key == "wobbliness")
                m_overrides.wobbliness = std::stoi(value);
            else if (key == "tessellation")
                m_overrides.tessellation = std::stoi(value);
            else if (key == "stiffness")
                m_overrides.stiffness = std::stof(value);
            else if (key == "drag")
                m_overrides.drag = std::stof(value);
            else if (key == "move_factor")
                m_overrides.moveFactor = std::stof(value);
        } catch (const std::exception& e) {
            Log::logger->log(Log::WARN, "[omarchy-fx] bad value for '{}' in settings: {}", key, value);
        }
    }
}

void CWobblyManager::init() {
    loadSettings();

    // The settings file is written by the shell plugin's panel, which follows
    // the write with `hyprctl reload`. Picking the file back up here is what
    // makes the panel's changes take effect.
    m_listeners.emplace_back(Event::bus()->m_events.config.reloaded.listen([this]() { loadSettings(); }));

    m_listeners.emplace_back(Event::bus()->m_events.render.preChecks.listen([this](const PHLMONITOR& monitor) {
        syncDrag();
        tick(monitor);
    }));

    // React to the button press/release that starts and ends a drag without
    // waiting for the next frame.
    m_listeners.emplace_back(Event::bus()->m_events.input.mouse.button.listen([this]() { syncDrag(); }));
    m_listeners.emplace_back(Event::bus()->m_events.input.mouse.move.listen([this]() { syncDrag(); }));
}

void CWobblyManager::shutdown() {
    for (auto& entry : m_entries) {
        detach(entry);
    }

    m_entries.clear();
    m_listeners.clear();

    if (Render::GL::g_pHyprOpenGL) {
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        CWobblyTransformer::dropProgram();
    }
}

SWobblyParams CWobblyManager::readParams() const {
    static auto PWOBBLINESS = CConfigValue<Config::INTEGER>(CFG_WOBBLINESS);
    static auto PSTIFFNESS  = CConfigValue<Config::FLOAT>(CFG_STIFFNESS);
    static auto PDRAG       = CConfigValue<Config::FLOAT>(CFG_DRAG);
    static auto PMOVEFACTOR = CConfigValue<Config::FLOAT>(CFG_MOVE_FACTOR);
    static auto PTESS       = CConfigValue<Config::INTEGER>(CFG_TESSELATION);

    auto params = wobblyPreset(m_overrides.wobbliness.value_or(sc<int>(*PWOBBLINESS)));

    const float STIFFNESS = m_overrides.stiffness.value_or(*PSTIFFNESS);
    const float DRAG      = m_overrides.drag.value_or(*PDRAG);
    const float MOVE      = m_overrides.moveFactor.value_or(*PMOVEFACTOR);

    if (STIFFNESS >= 0.F)
        params.stiffness = STIFFNESS;
    if (DRAG >= 0.F)
        params.drag = DRAG;
    if (MOVE >= 0.F)
        params.moveFactor = MOVE;

    params.tessellation = std::clamp(m_overrides.tessellation.value_or(sc<int>(*PTESS)), 2, 64);

    return params;
}

CWobblyManager::SEntry* CWobblyManager::find(const PHLWINDOW& window) {
    const auto IT = std::ranges::find_if(m_entries, [&window](const auto& e) { return e.window.lock() == window; });
    return IT == m_entries.end() ? nullptr : &*IT;
}

// The lattice lives on the window frame; the renderer hands us the full
// bounding box. This records where one sits inside the other.
static void syncFrameGeometry(const PHLWINDOW& window, SWobblyState& state) {
    const auto BOX   = window->getFullWindowBoundingBox();
    const auto FRAME = window->getWindowMainSurfaceBox();

    state.frameOffsetInBox = Vector2D{FRAME.x - BOX.x, FRAME.y - BOX.y};
    state.frameSize        = Vector2D{std::max(FRAME.w, 1.0), std::max(FRAME.h, 1.0)};
}

void CWobblyManager::begin(const PHLWINDOW& window, const Vector2D& cursor, bool resizing) {
    const auto FRAME = window->getWindowMainSurfaceBox();
    if (FRAME.w <= 1 || FRAME.h <= 1)
        return;

    auto state    = makeShared<SWobblyState>();
    state->params = readParams();
    state->model.reset(FRAME);
    state->model.grab(FRAME, cursor, resizing);
    syncFrameGeometry(window, *state);

    auto transformer = makeUnique<CWobblyTransformer>(state);
    auto raw         = transformer.get();
    window->m_transformers.emplace_back(std::move(transformer));

    m_entries.emplace_back(SEntry{
        .window      = window,
        .state       = state,
        .transformer = raw,
        .lastBounds  = window->getFullWindowBoundingBox(),
        .lastTick    = std::chrono::steady_clock::now(),
    });

    CBox damage = window->getFullWindowBoundingBox();
    damage.expand(8);
    g_pHyprRenderer->damageBox(damage);
}

void CWobblyManager::detach(SEntry& entry) {
    const auto WINDOW = entry.window.lock();
    if (WINDOW && entry.transformer) {
        auto damage = entry.lastBounds;
        damage.expand(8);
        g_pHyprRenderer->damageBox(damage);

        std::erase_if(WINDOW->m_transformers, [&entry](const auto& t) { return t.get() == entry.transformer; });
    }

    entry.transformer = nullptr;
    if (entry.state)
        entry.state->alive = false;
}

static CBox unionOf(const CBox& a, const CBox& b) {
    const double X1 = std::min(a.x, b.x), Y1 = std::min(a.y, b.y);
    const double X2 = std::max(a.x + a.w, b.x + b.w), Y2 = std::max(a.y + a.h, b.y + b.h);
    return CBox{X1, Y1, X2 - X1, Y2 - Y1};
}

void CWobblyManager::syncDrag() {
    if (!m_configOk)
        return;

    static auto PENABLED  = CConfigValue<Config::BOOL>(CFG_ENABLED);
    static auto PONMOVE   = CConfigValue<Config::BOOL>(CFG_ON_MOVE);
    static auto PONRESIZE = CConfigValue<Config::BOOL>(CFG_ON_RESIZE);

    const bool ENABLED   = m_overrides.enabled.value_or(*PENABLED);
    const bool ON_MOVE   = m_overrides.onMove.value_or(*PONMOVE);
    const bool ON_RESIZE = m_overrides.onResize.value_or(*PONRESIZE);

    PHLWINDOW  dragged;
    bool       resizing = false;

    if (ENABLED && g_layoutManager) {
        const auto& CONTROLLER = g_layoutManager->dragController();
        if (const auto TARGET = CONTROLLER->target()) {
            const auto MODE   = CONTROLLER->mode();
            const bool RESIZE = MODE != MBIND_MOVE && MODE != MBIND_INVALID;
            if ((MODE == MBIND_MOVE && ON_MOVE) || (RESIZE && ON_RESIZE)) {
                dragged  = TARGET->window();
                resizing = RESIZE;
            }
        }
    }

    // Whatever is no longer under the cursor lets go and springs back.
    for (auto& entry : m_entries) {
        if (entry.state->model.grabbed() && entry.window.lock() != dragged)
            entry.state->model.release();
    }

    if (!dragged || dragged->isHidden())
        return;

    const auto CURSOR = g_pInputManager->getMouseCoordsInternal();

    if (auto* existing = find(dragged)) {
        // Grabbed again while it was still settling from the last drag.
        if (!existing->state->model.grabbed())
            existing->state->model.grab(dragged->getWindowMainSurfaceBox(), CURSOR, resizing);
        return;
    }

    begin(dragged, CURSOR, resizing);
}

void CWobblyManager::tick(const PHLMONITOR& monitor) {
    if (m_entries.empty() || !monitor)
        return;

    const auto NOW = std::chrono::steady_clock::now();

    for (auto& entry : m_entries) {
        const auto WINDOW = entry.window.lock();
        if (!WINDOW || !entry.state->alive || !entry.transformer) {
            entry.transformer = nullptr;
            continue;
        }

        if (WINDOW->m_monitor.lock() != monitor)
            continue;

        float elapsed  = std::chrono::duration<float, std::milli>(NOW - entry.lastTick).count();
        entry.lastTick = NOW;
        elapsed        = std::clamp(elapsed, 0.F, MAX_FRAME_MS);

        auto&      state = *entry.state;
        const auto FRAME = WINDOW->getWindowMainSurfaceBox();

        if (state.model.grabbed()) {
            state.params = readParams();
            state.model.noteGeometry(FRAME);
        }

        syncFrameGeometry(WINDOW, state);

        while (elapsed > 0.F) {
            const float DT = std::min(elapsed, INTEGRATION_STEP_MS);
            elapsed -= DT;
            state.model.step(DT, state.params, FRAME);
        }

        const auto BOUNDS = unionOf(state.model.controlBounds(), WINDOW->getFullWindowBoundingBox());

        CBox       damage = unionOf(BOUNDS, entry.lastBounds);
        damage.expand(8);
        g_pHyprRenderer->damageBox(damage);

        entry.lastBounds = BOUNDS;

        if (state.model.finished())
            detach(entry);
    }

    std::erase_if(m_entries, [](const auto& e) { return !e.transformer || e.window.expired(); });
}
