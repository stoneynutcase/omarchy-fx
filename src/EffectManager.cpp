#include "EffectManager.hpp"
#include "globals.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
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
#include <iterator>
#include <string_view>

using namespace OmarchyFX;

static constexpr const char* CFG_WOBBLY_ENABLED   = "plugin:omarchy-fx:wobbly_enabled";
static constexpr const char* CFG_WOBBLY_ON_MOVE   = "plugin:omarchy-fx:wobbly_on_move";
static constexpr const char* CFG_WOBBLY_ON_RESIZE = "plugin:omarchy-fx:wobbly_on_resize";
static constexpr const char* CFG_WOBBLINESS       = "plugin:omarchy-fx:wobbly_wobbliness";
static constexpr const char* CFG_WOBBLY_STIFFNESS = "plugin:omarchy-fx:wobbly_stiffness";
static constexpr const char* CFG_WOBBLY_DRAG      = "plugin:omarchy-fx:wobbly_drag";
static constexpr const char* CFG_WOBBLY_MOVE      = "plugin:omarchy-fx:wobbly_move_factor";
static constexpr const char* CFG_WOBBLY_TESS      = "plugin:omarchy-fx:wobbly_tessellation";

static constexpr const char* CFG_ELASTIC_ENABLED  = "plugin:omarchy-fx:elastic_enabled";
static constexpr const char* CFG_ELASTIC_TILED    = "plugin:omarchy-fx:elastic_on_tiled";
static constexpr const char* CFG_ELASTIC_FLOATING = "plugin:omarchy-fx:elastic_on_floating";
static constexpr const char* CFG_ELASTIC_WORKSPACE = "plugin:omarchy-fx:elastic_on_workspace";
static constexpr const char* CFG_STRETCHINESS     = "plugin:omarchy-fx:elastic_stretchiness";
static constexpr const char* CFG_ELASTIC_PERIOD   = "plugin:omarchy-fx:elastic_period";
static constexpr const char* CFG_ELASTIC_DAMPING  = "plugin:omarchy-fx:elastic_damping";
static constexpr const char* CFG_ELASTIC_TILT     = "plugin:omarchy-fx:elastic_tilt";
static constexpr const char* CFG_ELASTIC_FOLLOW   = "plugin:omarchy-fx:elastic_follow";
static constexpr const char* CFG_ELASTIC_MAX     = "plugin:omarchy-fx:elastic_max_stretch";
static constexpr const char* CFG_ELASTIC_TESS     = "plugin:omarchy-fx:elastic_tessellation";

static constexpr const char* CFG_PULSE_ENABLED   = "plugin:omarchy-fx:pulse_enabled";
static constexpr const char* CFG_PULSE_ON_SWITCH = "plugin:omarchy-fx:pulse_on_switch";
static constexpr const char* CFG_PULSE_ON_CLICK  = "plugin:omarchy-fx:pulse_on_click";
static constexpr const char* CFG_PULSE_ON_HOVER  = "plugin:omarchy-fx:pulse_on_hover";
static constexpr const char* CFG_PULSE_STRENGTH  = "plugin:omarchy-fx:pulse_strength";
static constexpr const char* CFG_PULSE_AMOUNT    = "plugin:omarchy-fx:pulse_amount";
static constexpr const char* CFG_PULSE_PERIOD    = "plugin:omarchy-fx:pulse_period";
static constexpr const char* CFG_PULSE_DAMPING   = "plugin:omarchy-fx:pulse_damping";
static constexpr const char* CFG_PULSE_TESS      = "plugin:omarchy-fx:pulse_tessellation";

// Both effects integrate in fixed substeps, so they look the same at 60 and
// 240 Hz. The wobble's is KWin's 10 ms, kept for fidelity to the port; the
// elastic spring is stiffer and wants a finer one. The frame cap keeps a stall
// from firing hundreds of steps at once.
static constexpr float WOBBLY_STEP_MS  = 10.F;
static constexpr float ELASTIC_STEP_MS = 5.F;
static constexpr float PULSE_STEP_MS   = 5.F;
static constexpr float MAX_FRAME_MS    = 100.F;

// How long a "this window was sent to another workspace" mark stays good. Long
// enough to cover the gap between the event and the workspace becoming
// visible, short enough that a silent move never surfaces later.
static constexpr auto PENDING_MOVE_TTL = std::chrono::milliseconds(600);

// How often the shell's bar layout is stat'ed for the widget. Cheap, but the
// check sits on the mouse-move path, so not every event.
static constexpr auto SHELL_CHECK_EVERY = std::chrono::milliseconds(500);

// The shell plugin id, which is also its directory under plugins/ and its
// "id" in the bar layout. Must match shell/manifest.json.
static constexpr std::string_view SHELL_WIDGET_ID = "omarchy-fx";

void CEffectManager::registerConfig() {
    using namespace Config::Values;

    // Reading a value that failed to register aborts Hyprland, so every later
    // config read is gated on all of these having landed.
    m_configOk = true;

    const auto BOOL = [this](const char* name, const char* desc, bool def) {
        m_configOk &= HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CBoolValue>(name, desc, def));
    };
    const auto INT = [this](const char* name, const char* desc, int def, int min, int max) {
        m_configOk &= HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>(name, desc, def, SIntValueOptions{.min = min, .max = max}));
    };
    const auto FLOAT = [this](const char* name, const char* desc, float def, float min, float max) {
        m_configOk &= HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CFloatValue>(name, desc, def, SFloatValueOptions{.min = min, .max = max}));
    };

    BOOL(CFG_WOBBLY_ENABLED, "enable the wobbly windows effect", true);
    BOOL(CFG_WOBBLY_ON_MOVE, "wobble while dragging a window around", true);
    BOOL(CFG_WOBBLY_ON_RESIZE, "wobble while resize-dragging a window", true);
    INT(CFG_WOBBLINESS, "wobbliness preset, 0 (least) to 4 (most)", 1, 0, 4);
    FLOAT(CFG_WOBBLY_STIFFNESS, "override the preset's spring stiffness, -1 to keep it", -1.F, -1.F, 1.F);
    FLOAT(CFG_WOBBLY_DRAG, "override the preset's drag, -1 to keep it", -1.F, -1.F, 1.F);
    FLOAT(CFG_WOBBLY_MOVE, "override the preset's move factor, -1 to keep it", -1.F, -1.F, 1.F);
    INT(CFG_WOBBLY_TESS, "wobble render mesh quads per axis", 20, 2, 64);

    BOOL(CFG_ELASTIC_ENABLED, "enable the elastic effect on animated window moves", true);
    BOOL(CFG_ELASTIC_TILED, "stretch tiled windows when the layout moves them", true);
    BOOL(CFG_ELASTIC_FLOATING, "stretch floating windows when something animates them", false);
    BOOL(CFG_ELASTIC_WORKSPACE, "stretch a window you move to another workspace and follow", true);
    INT(CFG_STRETCHINESS, "stretchiness preset, 0 (least) to 4 (most)", 2, 0, 4);
    FLOAT(CFG_ELASTIC_PERIOD, "override the preset's spring period in ms, -1 to keep it", -1.F, -1.F, 2000.F);
    FLOAT(CFG_ELASTIC_DAMPING, "override the preset's damping ratio, -1 to keep it", -1.F, -1.F, 4.F);
    FLOAT(CFG_ELASTIC_TILT, "override the preset's leading/trailing stiffness tilt, -1 to keep it", -1.F, -1.F, 0.95F);
    FLOAT(CFG_ELASTIC_FOLLOW, "override how much bulk lag is drawn, -1 to keep it", -1.F, -1.F, 1.F);
    FLOAT(CFG_ELASTIC_MAX, "override the stretch the spring stiffens past, in px, -1 to keep it", -1.F, -1.F, 1000.F);
    INT(CFG_ELASTIC_TESS, "elastic render mesh quads per axis", 20, 2, 64);

    BOOL(CFG_PULSE_ENABLED, "swell a window once when it becomes the active one", true);
    BOOL(CFG_PULSE_ON_SWITCH, "pulse when focus moves by keyboard, by a dispatcher, or on its own", true);
    BOOL(CFG_PULSE_ON_CLICK, "pulse when a click gives a window focus", true);
    BOOL(CFG_PULSE_ON_HOVER, "pulse when focus follows the mouse", false);
    INT(CFG_PULSE_STRENGTH, "pulse preset, 0 (least) to 4 (most)", 2, 0, 4);
    FLOAT(CFG_PULSE_AMOUNT, "override how far the edges swell, in px, -1 to keep the preset's", -1.F, -1.F, 200.F);
    FLOAT(CFG_PULSE_PERIOD, "override the swell period in ms, -1 to keep it", -1.F, -1.F, 2000.F);
    FLOAT(CFG_PULSE_DAMPING, "override the damping ratio, -1 to keep it", -1.F, -1.F, 4.F);
    INT(CFG_PULSE_TESS, "pulse render mesh quads per axis", 20, 2, 64);

    // A notification, not a log line: Log::logger is an inline variable, so a
    // plugin links its own uninitialised copy of it and anything logged through
    // it goes nowhere. This is the one failure the user has to be told about —
    // it leaves every effect inert.
    if (!m_configOk)
        HyprlandAPI::addNotification(PHANDLE, "[omarchy-fx] Could not register its config options; effects are inert.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 6000);
}

std::string CEffectManager::configHome() {
    const auto XDG = getenv("XDG_CONFIG_HOME");
    if (XDG && *XDG)
        return XDG;

    const auto HOME = getenv("HOME");
    return std::string{HOME ? HOME : ""} + "/.config";
}

std::string CEffectManager::settingsPath() {
    return configHome() + "/omarchy/omarchy-fx.conf";
}

// Is this widget placed on the bar in the shell's layout file? A substring
// match on `"id": "<id>"`, not a JSON parse: the file is small, the shape is
// Omarchy's own, and pulling in a JSON library for one key is not worth it.
static bool barHasWidget(const std::string& layoutPath, std::string_view id) {
    std::ifstream file(layoutPath);
    if (!file.good())
        return false;

    const std::string TEXT((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::string NEEDLE = "\"" + std::string{id} + "\"";

    for (auto pos = TEXT.find(NEEDLE); pos != std::string::npos; pos = TEXT.find(NEEDLE, pos + NEEDLE.size())) {
        // Walk back over the separator and expect the "id" key in front of it.
        auto i = pos;
        while (i > 0 && (TEXT[i - 1] == ':' || TEXT[i - 1] == ' ' || TEXT[i - 1] == '\t' || TEXT[i - 1] == '\n' || TEXT[i - 1] == '\r'))
            --i;
        if (i >= 4 && TEXT.compare(i - 4, 4, "\"id\"") == 0)
            return true;
    }

    return false;
}

bool CEffectManager::shellAllows() {
    const auto NOW = std::chrono::steady_clock::now();
    if (m_shellEvaluated && NOW - m_shellCheckedAt < SHELL_CHECK_EVERY)
        return m_shellAllows;
    m_shellCheckedAt = NOW;

    // No Omarchy shell, no widget to be the switch: the config decides.
    const auto OMARCHY = getenv("OMARCHY_PATH");
    if (!std::filesystem::is_directory(std::string{OMARCHY && *OMARCHY ? OMARCHY : "/usr/share/omarchy"} + "/shell")) {
        m_shellEvaluated = true;
        m_shellAllows    = true;
        return true;
    }

    const auto      LAYOUT = configHome() + "/omarchy/shell.json";
    const auto      WIDGET = configHome() + "/omarchy/plugins/" + std::string{SHELL_WIDGET_ID};

    std::error_code ec;
    const auto      STAMP     = std::filesystem::last_write_time(LAYOUT, ec); // min() when the file is missing
    const bool      INSTALLED = std::filesystem::is_directory(WIDGET);

    if (m_shellEvaluated && STAMP == m_shellLayoutStamp && INSTALLED == m_shellWidgetInstalled)
        return m_shellAllows;

    m_shellEvaluated       = true;
    m_shellLayoutStamp     = STAMP;
    m_shellWidgetInstalled = INSTALLED;

    // Disabling a shell plugin drops its entry from the bar layout, and
    // removing it also takes the directory. Either one means "off".
    m_shellAllows = INSTALLED && barHasWidget(LAYOUT, SHELL_WIDGET_ID);
    return m_shellAllows;
}

void CEffectManager::loadSettings() {
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
            // The unprefixed spellings are what the panel wrote before there was
            // more than one effect; they still mean the wobble.
            if (key == "wobbly_enabled" || key == "enabled")
                m_overrides.wobblyEnabled = asBool(value);
            else if (key == "wobbly_on_move" || key == "on_move")
                m_overrides.wobblyOnMove = asBool(value);
            else if (key == "wobbly_on_resize" || key == "on_resize")
                m_overrides.wobblyOnResize = asBool(value);
            else if (key == "wobbliness")
                m_overrides.wobbliness = std::stoi(value);
            else if (key == "wobbly_tessellation" || key == "tessellation")
                m_overrides.wobblyTessellation = std::stoi(value);
            else if (key == "wobbly_stiffness" || key == "stiffness")
                m_overrides.wobblyStiffness = std::stof(value);
            else if (key == "wobbly_drag" || key == "drag")
                m_overrides.wobblyDrag = std::stof(value);
            else if (key == "wobbly_move_factor" || key == "move_factor")
                m_overrides.wobblyMoveFactor = std::stof(value);
            else if (key == "elastic_enabled")
                m_overrides.elasticEnabled = asBool(value);
            else if (key == "elastic_on_tiled")
                m_overrides.elasticOnTiled = asBool(value);
            else if (key == "elastic_on_floating")
                m_overrides.elasticOnFloating = asBool(value);
            else if (key == "elastic_on_workspace")
                m_overrides.elasticOnWorkspace = asBool(value);
            else if (key == "stretchiness")
                m_overrides.stretchiness = std::stoi(value);
            else if (key == "elastic_tessellation")
                m_overrides.elasticTessellation = std::stoi(value);
            else if (key == "elastic_period")
                m_overrides.elasticPeriod = std::stof(value);
            else if (key == "elastic_damping")
                m_overrides.elasticDamping = std::stof(value);
            else if (key == "elastic_tilt")
                m_overrides.elasticTilt = std::stof(value);
            else if (key == "elastic_follow")
                m_overrides.elasticFollow = std::stof(value);
            else if (key == "elastic_max_stretch")
                m_overrides.elasticMaxStretch = std::stof(value);
            else if (key == "pulse_enabled")
                m_overrides.pulseEnabled = asBool(value);
            else if (key == "pulse_on_switch")
                m_overrides.pulseOnSwitch = asBool(value);
            else if (key == "pulse_on_click")
                m_overrides.pulseOnClick = asBool(value);
            else if (key == "pulse_on_hover")
                m_overrides.pulseOnHover = asBool(value);
            else if (key == "pulse_strength")
                m_overrides.pulseStrength = std::stoi(value);
            else if (key == "pulse_tessellation")
                m_overrides.pulseTessellation = std::stoi(value);
            else if (key == "pulse_amount")
                m_overrides.pulseAmount = std::stof(value);
            else if (key == "pulse_period")
                m_overrides.pulsePeriod = std::stof(value);
            else if (key == "pulse_damping")
                m_overrides.pulseDamping = std::stof(value);
        } catch (const std::exception& e) { Log::logger->log(Log::WARN, "[omarchy-fx] bad value for '{}' in settings: {}", key, value); }
    }
}

void CEffectManager::init() {
    loadSettings();

    // The settings file is written by the shell plugin's panel, which follows
    // the write with `hyprctl reload`. Picking the file back up here is what
    // makes the panel's changes take effect.
    m_listeners.emplace_back(Event::bus()->m_events.config.reloaded.listen([this]() {
        loadSettings();
        m_shellEvaluated = false; // re-stat the bar layout on the next trigger
    }));

    m_listeners.emplace_back(Event::bus()->m_events.render.preChecks.listen([this](const PHLMONITOR& monitor) {
        syncDrag();
        scanAnimations(monitor);
        tick(monitor);
    }));

    // React to the button press/release that starts and ends a drag without
    // waiting for the next frame.
    // The only trigger that cannot be seen by watching geometry: a carried
    // window's own rect does not move, the workspace under it does.
    m_listeners.emplace_back(Event::bus()->m_events.window.moveToWorkspace.listen([this](const PHLWINDOW& window, const PHLWORKSPACE& workspace) { noteWorkspaceMove(window); }));

    m_listeners.emplace_back(Event::bus()->m_events.window.active.listen([this](const PHLWINDOW& window, Desktop::eFocusReason reason) { onFocus(window, reason); }));

    m_listeners.emplace_back(Event::bus()->m_events.input.mouse.button.listen([this]() { syncDrag(); }));
    m_listeners.emplace_back(Event::bus()->m_events.input.mouse.move.listen([this]() { syncDrag(); }));
}

void CEffectManager::shutdown() {
    for (auto& entry : m_entries) {
        detach(entry);
    }

    m_entries.clear();
    m_listeners.clear();

    if (Render::GL::g_pHyprOpenGL) {
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        CMeshTransformer::dropProgram();
    }
}

SWobblyParams CEffectManager::wobblyParams() const {
    static auto PWOBBLINESS = CConfigValue<Config::INTEGER>(CFG_WOBBLINESS);
    static auto PSTIFFNESS  = CConfigValue<Config::FLOAT>(CFG_WOBBLY_STIFFNESS);
    static auto PDRAG       = CConfigValue<Config::FLOAT>(CFG_WOBBLY_DRAG);
    static auto PMOVEFACTOR = CConfigValue<Config::FLOAT>(CFG_WOBBLY_MOVE);
    static auto PTESS       = CConfigValue<Config::INTEGER>(CFG_WOBBLY_TESS);

    auto        params = wobblyPreset(m_overrides.wobbliness.value_or(sc<int>(*PWOBBLINESS)));

    const float STIFFNESS = m_overrides.wobblyStiffness.value_or(*PSTIFFNESS);
    const float DRAG      = m_overrides.wobblyDrag.value_or(*PDRAG);
    const float MOVE      = m_overrides.wobblyMoveFactor.value_or(*PMOVEFACTOR);

    if (STIFFNESS >= 0.F)
        params.stiffness = STIFFNESS;
    if (DRAG >= 0.F)
        params.drag = DRAG;
    if (MOVE >= 0.F)
        params.moveFactor = MOVE;

    params.tessellation = std::clamp(m_overrides.wobblyTessellation.value_or(sc<int>(*PTESS)), 2, 64);

    return params;
}

SElasticParams CEffectManager::elasticParams() const {
    static auto PSTRETCH = CConfigValue<Config::INTEGER>(CFG_STRETCHINESS);
    static auto PPERIOD  = CConfigValue<Config::FLOAT>(CFG_ELASTIC_PERIOD);
    static auto PDAMPING = CConfigValue<Config::FLOAT>(CFG_ELASTIC_DAMPING);
    static auto PTILT    = CConfigValue<Config::FLOAT>(CFG_ELASTIC_TILT);
    static auto PFOLLOW  = CConfigValue<Config::FLOAT>(CFG_ELASTIC_FOLLOW);
    static auto PSTRETCHPX = CConfigValue<Config::FLOAT>(CFG_ELASTIC_MAX);
    static auto PTESS    = CConfigValue<Config::INTEGER>(CFG_ELASTIC_TESS);

    auto        params = elasticPreset(m_overrides.stretchiness.value_or(sc<int>(*PSTRETCH)));

    const float PERIOD  = m_overrides.elasticPeriod.value_or(*PPERIOD);
    const float DAMPING = m_overrides.elasticDamping.value_or(*PDAMPING);
    const float TILT    = m_overrides.elasticTilt.value_or(*PTILT);
    const float FOLLOW  = m_overrides.elasticFollow.value_or(*PFOLLOW);
    const float STRETCH = m_overrides.elasticMaxStretch.value_or(*PSTRETCHPX);

    if (PERIOD >= 0.F)
        params.period = std::max(PERIOD, 1.F);
    if (DAMPING >= 0.F)
        params.damping = DAMPING;
    if (TILT >= 0.F)
        params.tilt = TILT;
    if (FOLLOW >= 0.F)
        params.follow = FOLLOW;
    if (STRETCH >= 0.F)
        params.maxStretch = std::max(STRETCH, 1.F);

    params.tessellation = std::clamp(m_overrides.elasticTessellation.value_or(sc<int>(*PTESS)), 2, 64);

    return params;
}

SPulseParams CEffectManager::pulseParams() const {
    static auto PSTRENGTH = CConfigValue<Config::INTEGER>(CFG_PULSE_STRENGTH);
    static auto PAMOUNT   = CConfigValue<Config::FLOAT>(CFG_PULSE_AMOUNT);
    static auto PPERIOD   = CConfigValue<Config::FLOAT>(CFG_PULSE_PERIOD);
    static auto PDAMPING  = CConfigValue<Config::FLOAT>(CFG_PULSE_DAMPING);
    static auto PTESS     = CConfigValue<Config::INTEGER>(CFG_PULSE_TESS);

    auto        params = pulsePreset(m_overrides.pulseStrength.value_or(sc<int>(*PSTRENGTH)));

    const float AMOUNT  = m_overrides.pulseAmount.value_or(*PAMOUNT);
    const float PERIOD  = m_overrides.pulsePeriod.value_or(*PPERIOD);
    const float DAMPING = m_overrides.pulseDamping.value_or(*PDAMPING);

    if (AMOUNT >= 0.F)
        params.amount = AMOUNT;
    if (PERIOD >= 0.F)
        params.period = std::max(PERIOD, 1.F);
    if (DAMPING >= 0.F)
        params.damping = DAMPING;

    params.tessellation = std::clamp(m_overrides.pulseTessellation.value_or(sc<int>(*PTESS)), 2, 64);

    return params;
}

CEffectManager::SEntry* CEffectManager::find(const PHLWINDOW& window) {
    const auto IT = std::ranges::find_if(m_entries, [&window](const auto& e) { return e.window.lock() == window; });
    return IT == m_entries.end() ? nullptr : &*IT;
}

// The lattice lives on the window frame; the renderer hands us the full
// bounding box. This records where one sits inside the other.
static void syncFrameGeometry(const PHLWINDOW& window, SMeshState& state) {
    const auto BOX   = window->getFullWindowBoundingBox();
    const auto FRAME = window->getWindowMainSurfaceBox();

    state.frameOffsetInBox = Vector2D{FRAME.x - BOX.x, FRAME.y - BOX.y};
    state.frameSize        = Vector2D{std::max(FRAME.w, 1.0), std::max(FRAME.h, 1.0)};
}

CEffectManager::SEntry* CEffectManager::ensureEntry(const PHLWINDOW& window) {
    if (auto* existing = find(window))
        return existing;

    auto state = makeShared<SMeshState>();
    syncFrameGeometry(window, *state);

    auto transformer = makeUnique<CMeshTransformer>(state);
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

    return &m_entries.back();
}

void CEffectManager::detach(SEntry& entry) {
    const auto WINDOW = entry.window.lock();
    if (WINDOW && entry.transformer) {
        auto damage = entry.lastBounds;
        damage.expand(8);
        g_pHyprRenderer->damageBox(damage);

        std::erase_if(WINDOW->m_transformers, [&entry](const auto& t) { return t.get() == entry.transformer; });
    }

    entry.transformer = nullptr;
    entry.wobblyOn    = false;
    entry.elasticOn   = false;
    entry.pulseOn     = false;
    if (entry.state)
        entry.state->alive = false;
}

static CBox unionOf(const CBox& a, const CBox& b) {
    const double X1 = std::min(a.x, b.x), Y1 = std::min(a.y, b.y);
    const double X2 = std::max(a.x + a.w, b.x + b.w), Y2 = std::max(a.y + a.h, b.y + b.h);
    return CBox{X1, Y1, X2 - X1, Y2 - Y1};
}

void CEffectManager::beginWobble(const PHLWINDOW& window, const Vector2D& cursor, bool resizing) {
    const auto FRAME = window->getWindowMainSurfaceBox();
    if (FRAME.w <= 1 || FRAME.h <= 1)
        return;

    auto* entry = ensureEntry(window);
    if (!entry)
        return;

    entry->wobblyParams = wobblyParams();
    entry->wobbly.reset(FRAME);
    entry->wobbly.grab(FRAME, cursor, resizing);
    entry->wobblyOn = true;

    // The drag owns the window now.
    entry->elasticOn = false;
    entry->pulseOn   = false;
}

void CEffectManager::syncDrag() {
    if (!m_configOk)
        return;

    static auto PENABLED  = CConfigValue<Config::BOOL>(CFG_WOBBLY_ENABLED);
    static auto PONMOVE   = CConfigValue<Config::BOOL>(CFG_WOBBLY_ON_MOVE);
    static auto PONRESIZE = CConfigValue<Config::BOOL>(CFG_WOBBLY_ON_RESIZE);

    const bool  ENABLED   = shellAllows() && m_overrides.wobblyEnabled.value_or(*PENABLED);
    const bool  ON_MOVE   = m_overrides.wobblyOnMove.value_or(*PONMOVE);
    const bool  ON_RESIZE = m_overrides.wobblyOnResize.value_or(*PONRESIZE);

    PHLWINDOW   dragged;
    bool        resizing = false;

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
        if (entry.wobblyOn && entry.wobbly.grabbed() && entry.window.lock() != dragged)
            entry.wobbly.release();
    }

    if (!dragged || dragged->isHidden())
        return;

    const auto CURSOR = g_pInputManager->getMouseCoordsInternal();

    if (auto* existing = find(dragged); existing && existing->wobblyOn) {
        // Grabbed again while it was still settling from the last drag.
        if (!existing->wobbly.grabbed())
            existing->wobbly.grab(dragged->getWindowMainSurfaceBox(), CURSOR, resizing);
        return;
    }

    beginWobble(dragged, CURSOR, resizing);
}

// Whether Hyprland is animating the window's own geometry. This is the trigger
// for a swap or a reflow: we do not care which dispatcher moved the window,
// only that its rect is on its way somewhere.
static bool geometryAnimating(const PHLWINDOW& window) {
    return window->positionAnimation()->isBeingAnimated() || window->sizeAnimation()->isBeingAnimated();
}

// A workspace does not move its windows to slide — it animates one render
// offset that every window on it is drawn through. So a window carried to
// another workspace travels the width of the screen while its own geometry
// says it never moved.
static bool workspaceSliding(const PHLWINDOW& window) {
    const auto WS = window->m_workspace;
    return WS && WS->m_renderOffset->isBeingAnimated();
}

// Everything that puts the window somewhere other than where its rect says.
static Vector2D renderOffsetOf(const PHLWINDOW& window) {
    const auto WS = window->m_workspace;
    return WS ? WS->m_renderOffset->value() : Vector2D{};
}

static bool travelling(const PHLWINDOW& window) {
    return geometryAnimating(window) || workspaceSliding(window);
}

void CEffectManager::noteWorkspaceMove(const PHLWINDOW& window) {
    const auto NOW = std::chrono::steady_clock::now();

    // The window is not on screen yet when the event lands — the workspace it
    // was sent to still has to become the visible one. So this only marks it,
    // and the next render pass that finds it visible and sliding arms it.
    std::erase_if(m_pendingMoves, [&NOW, &window](const auto& p) { //
        return p.window.expired() || p.window.lock() == window || NOW - p.at > PENDING_MOVE_TTL;
    });

    m_pendingMoves.emplace_back(SPendingMove{.window = window, .at = NOW});
}

bool CEffectManager::pendingWorkspaceMove(const PHLWINDOW& window) {
    const auto NOW = std::chrono::steady_clock::now();
    const auto IT  = std::ranges::find_if(m_pendingMoves, [&window](const auto& p) { return p.window.lock() == window; });

    if (IT == m_pendingMoves.end())
        return false;

    // A silent move never brings the window on screen, so its mark is never
    // taken; it has to age out instead.
    if (NOW - IT->at > PENDING_MOVE_TTL) {
        m_pendingMoves.erase(IT);
        return false;
    }

    return true;
}

void CEffectManager::clearWorkspaceMove(const PHLWINDOW& window) {
    std::erase_if(m_pendingMoves, [&window](const auto& p) { return p.window.expired() || p.window.lock() == window; });
}

void CEffectManager::scanAnimations(const PHLMONITOR& monitor) {
    if (!m_configOk || !monitor)
        return;

    static auto PENABLED   = CConfigValue<Config::BOOL>(CFG_ELASTIC_ENABLED);
    static auto PTILED     = CConfigValue<Config::BOOL>(CFG_ELASTIC_TILED);
    static auto PFLOATING  = CConfigValue<Config::BOOL>(CFG_ELASTIC_FLOATING);
    static auto PWORKSPACE = CConfigValue<Config::BOOL>(CFG_ELASTIC_WORKSPACE);

    const bool  ENABLED   = shellAllows() && m_overrides.elasticEnabled.value_or(*PENABLED);
    const bool  TILED     = m_overrides.elasticOnTiled.value_or(*PTILED);
    const bool  FLOATING  = m_overrides.elasticOnFloating.value_or(*PFLOATING);
    const bool  WORKSPACE = m_overrides.elasticOnWorkspace.value_or(*PWORKSPACE);

    if (!ENABLED || (!TILED && !FLOATING && !WORKSPACE))
        return;

    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (!WINDOW->m_isMapped || WINDOW->isHidden() || WINDOW->m_monitor.lock() != monitor)
            continue;

        // The open animation is Hyprland's own, and it animates the geometry
        // from nothing — stretching that would fight it rather than dress it.
        if (WINDOW->m_animatingIn)
            continue;

        // The three triggers are independent: a window you throw at another
        // workspace stretches on the way even if the swap trigger is off.
        const bool CARRIED = WORKSPACE && pendingWorkspaceMove(WINDOW);

        if (!CARRIED && (WINDOW->m_isFloating ? !FLOATING : !TILED))
            continue;

        if (CARRIED ? !travelling(WINDOW) : !geometryAnimating(WINDOW))
            continue;

        const auto FRAME = WINDOW->getWindowMainSurfaceBox().copy().translate(renderOffsetOf(WINDOW));
        if (FRAME.w <= 1 || FRAME.h <= 1)
            continue;

        // Something is already deforming this window; let it finish.
        if (auto* existing = find(WINDOW); existing && (existing->wobblyOn || existing->elasticOn || existing->pulseOn))
            continue;

        auto* entry = ensureEntry(WINDOW);
        if (!entry)
            continue;

        entry->elasticParams = elasticParams();
        entry->elastic.arm(FRAME);
        entry->elasticOn = true;

        if (CARRIED)
            clearWorkspaceMove(WINDOW);
    }
}

void CEffectManager::onFocus(const PHLWINDOW& window, Desktop::eFocusReason reason) {
    if (!m_configOk || !window)
        return;

    // Track this before any of the gates below, so that turning the effect on
    // later does not pulse the window that has had focus all along.
    const bool SAME = m_lastActive.lock() == window;
    m_lastActive    = window;
    if (SAME)
        return;

    static auto PENABLED = CConfigValue<Config::BOOL>(CFG_PULSE_ENABLED);
    static auto PSWITCH  = CConfigValue<Config::BOOL>(CFG_PULSE_ON_SWITCH);
    static auto PCLICK   = CConfigValue<Config::BOOL>(CFG_PULSE_ON_CLICK);
    static auto PHOVER   = CConfigValue<Config::BOOL>(CFG_PULSE_ON_HOVER);

    if (!shellAllows() || !m_overrides.pulseEnabled.value_or(*PENABLED))
        return;

    // Three doors, by what moved the focus. Hover is off by default because
    // with focus following the mouse every tile you cross would swell, and a
    // cue for "where did focus go" is pointless when the pointer is on it.
    bool wanted = false;
    switch (reason) {
        case Desktop::FOCUS_REASON_FFM: wanted = m_overrides.pulseOnHover.value_or(*PHOVER); break;
        case Desktop::FOCUS_REASON_CLICK: wanted = m_overrides.pulseOnClick.value_or(*PCLICK); break;
        default: wanted = m_overrides.pulseOnSwitch.value_or(*PSWITCH); break;
    }
    if (!wanted)
        return;

    // A window that is still opening has Hyprland's own animation on it.
    if (!window->m_isMapped || window->isHidden() || window->m_animatingIn)
        return;

    // One that fills its monitor has nowhere to swell to. Judged by size rather
    // than by fullscreen state, which also catches a maximised window with the
    // gaps turned off.
    if (const auto MON = window->m_monitor.lock()) {
        const auto FRAME = window->getWindowMainSurfaceBox();
        if (FRAME.w >= MON->m_size.x - 1 && FRAME.h >= MON->m_size.y - 1)
            return;
    }

    // Let a drag or a stretch already on this window finish; a pulse still
    // ringing is simply restarted.
    if (auto* existing = find(window); existing && (existing->wobblyOn || existing->elasticOn))
        return;

    const auto FRAME = window->getWindowMainSurfaceBox().copy().translate(renderOffsetOf(window));
    if (FRAME.w <= 1 || FRAME.h <= 1)
        return;

    auto* entry = ensureEntry(window);
    if (!entry)
        return;

    entry->pulseParams = pulseParams();
    entry->pulse.arm(FRAME, entry->pulseParams);
    entry->pulseOn = true;
}

void CEffectManager::tick(const PHLMONITOR& monitor) {
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

        auto& state = *entry.state;

        // Everything below works in the space the window is actually drawn in,
        // which during a workspace slide is not where its own rect says it is.
        // The offset is zero in every other case, so this costs the wobble
        // nothing and gets a carried window right.
        const auto OFFSET = renderOffsetOf(WINDOW);
        const auto FRAME  = WINDOW->getWindowMainSurfaceBox().copy().translate(OFFSET);

        syncFrameGeometry(WINDOW, state);

        CBox deformed = FRAME;

        if (entry.wobblyOn) {
            if (entry.wobbly.grabbed()) {
                entry.wobblyParams = wobblyParams();
                entry.wobbly.noteGeometry(FRAME);
            }

            for (float left = elapsed; left > 0.F;) {
                const float DT = std::min(left, WOBBLY_STEP_MS);
                left -= DT;
                entry.wobbly.step(DT, entry.wobblyParams, FRAME);
            }

            if (entry.wobbly.finished())
                entry.wobblyOn = false;
            else {
                state.offsets      = entry.wobbly.offsets();
                state.tessellation = entry.wobblyParams.tessellation;
                deformed           = entry.wobbly.controlBounds();
            }
        } else if (entry.elasticOn) {
            const bool ANIMATING = travelling(WINDOW);

            entry.elastic.advance(FRAME, elapsed, ANIMATING);

            for (float left = elapsed; left > 0.F;) {
                const float DT = std::min(left, ELASTIC_STEP_MS);
                left -= DT;
                entry.elastic.step(DT, entry.elasticParams, FRAME);
            }

            if (!ANIMATING && entry.elastic.settled())
                entry.elasticOn = false;
            else {
                state.offsets      = entry.elastic.offsets(entry.elasticParams);
                state.tessellation = entry.elasticParams.tessellation;
                deformed           = entry.elastic.controlBounds(entry.elasticParams);
            }
        } else if (entry.pulseOn) {
            for (float left = elapsed; left > 0.F;) {
                const float DT = std::min(left, PULSE_STEP_MS);
                left -= DT;
                entry.pulse.step(DT, entry.pulseParams, FRAME);
            }

            if (entry.pulse.settled())
                entry.pulseOn = false;
            else {
                state.offsets      = entry.pulse.offsets(entry.pulseParams);
                state.tessellation = entry.pulseParams.tessellation;
                deformed           = entry.pulse.controlBounds(entry.pulseParams);
            }
        }

        const bool DONE = !entry.wobblyOn && !entry.elasticOn && !entry.pulseOn;
        if (DONE)
            state.offsets = {};

        const auto BOUNDS = unionOf(deformed, WINDOW->getFullWindowBoundingBox().copy().translate(OFFSET));

        // The control net bounds the frame, but the mesh runs over the whole
        // bounding box and the Bezier *extrapolates* past the net for the
        // shadow and borders. So the margin has to scale with how far the
        // lattice has been pushed, or a heavy deformation leaves trails.
        double maxOffset = 0;
        for (const auto& o : state.offsets) {
            maxOffset = std::max(maxOffset, o.size());
        }

        CBox damage = unionOf(BOUNDS, entry.lastBounds);
        damage.expand(8 + maxOffset);
        g_pHyprRenderer->damageBox(damage);

        entry.lastBounds = BOUNDS;

        if (DONE)
            detach(entry);
    }

    std::erase_if(m_entries, [](const auto& e) { return !e.transformer || e.window.expired(); });
}
