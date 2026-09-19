pragma ComponentBehavior: Bound
import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Ui

// Settings panel for the omarchy-fx Hyprland plugin — one section per effect.
//
// Both halves agree on one small key=value file. This panel owns it: it reads
// the file directly and writes it back, then issues a stock `hyprctl reload`,
// which the plugin listens for to re-read the file. Keys absent from the file
// fall back to the Hyprland config, so a value set in Lua still shows here.
//
// Deliberately no plugin-specific hyprctl command: registering one crashed
// Hyprland inside pluginInit, and a compositor plugin that dies there takes the
// whole session down. Everything below uses stock hyprctl only.
Panel {
  id: root
  moduleName: "omarchy-fx"
  ipcTarget: "omarchy-fx"
  manageIpc: false

  property var anchorItem: null

  // The bar tracks the widget mounted in its slot — BarWidget.qml — not this
  // nested panel, so popout coordination has to identify as that widget.
  property var hostWidget: null
  readonly property var barIdentity: hostWidget || root

  readonly property color fg: Color.popups.text
  readonly property string fontFam: bar ? bar.fontFamily : Style.font.family

  // ---- effect state, seeded from the compositor plugin --------------------
  property bool pluginLoaded: false

  // Derived rather than assigned: pluginLoaded and the option query are filled
  // in by two processes racing each other, and whichever lands second used to
  // leave this stale.
  property bool hyprOptionsMissing: false
  readonly property bool configError: root.pluginLoaded && root.hyprOptionsMissing

  property bool wobblyEnabled: true
  property bool onMove: true
  property bool onResize: true
  property int  wobbliness: 1

  property bool elasticEnabled: true
  property bool onTiled: true
  property bool onFloating: false
  property bool onWorkspace: true
  property int  stretchiness: 2

  property bool pulseEnabled: true
  property bool onSwitch: true
  property bool onClick: true
  property bool onHover: false
  property int  pulseStrength: 2

  // One mesh resolution for both effects. The plugin keeps a key per effect;
  // this writes them together, because "how finely is the window tessellated"
  // is a rendering cost, not a per-effect taste.
  property int  tessellation: 20

  // Anything on at all? That is what the bar icon and the middle-click toggle
  // act on.
  readonly property bool enabled: root.wobblyEnabled || root.elasticEnabled || root.pulseEnabled

  // Read-only echo of what the physics is actually running with, so a value
  // set in Lua is visible here rather than silently contradicting the sliders.
  property real stiffness: 0.10
  property real drag: 0.85
  property real moveFactor: 0.10

  property real period: 180
  property real damping: 0.55
  property real maxStretch: 65

  property real pulseAmount: 9
  property real pulsePeriod: 260
  property real pulseDamping: 0.55

  readonly property var wobblinessNames:  ["Rigid", "Subtle", "Springy", "Loose", "Jelly"]
  readonly property var stretchinessNames: ["Taut", "Springy", "Elastic", "Rubber", "Taffy"]
  readonly property var pulseNames:        ["Subtle", "Soft", "Firm", "Lively", "Bouncy"]

  // KWin's pset[0..4]. Mirrors wobblyPreset() in src/WobblyModel.cpp.
  readonly property var presetStiffness: [0.15, 0.10, 0.06, 0.03, 0.01]
  readonly property var presetDrag:      [0.80, 0.85, 0.90, 0.92, 0.97]
  readonly property var presetMove:      [0.10, 0.10, 0.10, 0.20, 0.25]

  // Mirrors elasticPreset() in src/ElasticModel.cpp. Both tables stay the
  // source of truth — keep these in step with them.
  readonly property var presetPeriod:     [110, 145, 180, 235, 310]
  readonly property var presetDamping:    [0.80, 0.65, 0.55, 0.45, 0.35]
  readonly property var presetMaxStretch: [30, 45, 65, 90, 130]

  // Mirrors pulsePreset() in src/PulseModel.cpp.
  readonly property var presetPulseAmount:  [4, 6, 9, 13, 18]
  readonly property var presetPulsePeriod:  [180, 220, 260, 300, 340]
  readonly property var presetPulseDamping: [0.90, 0.70, 0.55, 0.42, 0.32]

  readonly property string settingsPath: Quickshell.env("HOME") + "/.config/omarchy/omarchy-fx.conf"

  // Every option registerConfig() registers, under plugin:omarchy-fx:. The
  // query below and the did-it-register check both read off this one list.
  readonly property var hyprKeys: [
    "wobbly_enabled", "wobbly_on_move", "wobbly_on_resize", "wobbly_wobbliness",
    "wobbly_tessellation", "wobbly_stiffness", "wobbly_drag", "wobbly_move_factor",
    "elastic_enabled", "elastic_on_tiled", "elastic_on_floating", "elastic_on_workspace", "elastic_stretchiness",
    "elastic_tessellation", "elastic_period", "elastic_damping", "elastic_tilt",
    "elastic_follow", "elastic_max_stretch",
    "pulse_enabled", "pulse_on_switch", "pulse_on_click", "pulse_on_hover", "pulse_strength",
    "pulse_tessellation", "pulse_amount", "pulse_period", "pulse_damping"
  ]

  // Settings-file keys as the panel wrote them before there was more than one
  // effect. Read for seeding, never written back.
  readonly property var legacyKeys: ({
    "enabled": "wobbly_enabled",
    "on_move": "wobbly_on_move",
    "on_resize": "wobbly_on_resize",
    "tessellation": "wobbly_tessellation"
  })

  // ---- reading state -------------------------------------------------------

  // Overrides parsed out of the settings file, and the Hyprland config values
  // they fall back to. Both are plain key -> value maps.
  property var fileVals: ({})
  property var hyprVals: ({})

  function refresh() {
    loadedProc.running = false
    loadedProc.running = true
    settingsFile.reload()
    hyprProc.running = false
    hyprProc.running = true
  }

  // Effective value for one setting: settings file wins, then the Hyprland
  // config, then the plugin's own registered default.
  function effective(fileKey, hyprKey, fallback) {
    if (root.fileVals[fileKey] !== undefined) return root.fileVals[fileKey]
    if (root.hyprVals[hyprKey] !== undefined) return root.hyprVals[hyprKey]
    return fallback
  }

  function clamp5(v) { return Math.max(0, Math.min(4, Number(v))) }

  function apply() {
    root.wobblyEnabled = effective("wobbly_enabled",   "wobbly_enabled",   1) != 0
    root.onMove        = effective("wobbly_on_move",   "wobbly_on_move",   1) != 0
    root.onResize      = effective("wobbly_on_resize", "wobbly_on_resize", 1) != 0
    root.wobbliness    = root.clamp5(effective("wobbliness", "wobbly_wobbliness", 1))

    root.elasticEnabled = effective("elastic_enabled",     "elastic_enabled",     1) != 0
    root.onTiled        = effective("elastic_on_tiled",    "elastic_on_tiled",    1) != 0
    root.onFloating     = effective("elastic_on_floating", "elastic_on_floating", 0) != 0
    root.onWorkspace    = effective("elastic_on_workspace", "elastic_on_workspace", 1) != 0
    root.stretchiness   = root.clamp5(effective("stretchiness", "elastic_stretchiness", 2))

    root.tessellation = Number(effective("wobbly_tessellation", "wobbly_tessellation", 20))

    // The physics the plugin will actually run with: the preset for the current
    // slider position, with any non-negative per-parameter override on top.
    var st = Number(effective("stiffness",   "wobbly_stiffness",   -1))
    var dr = Number(effective("drag",        "wobbly_drag",        -1))
    var mv = Number(effective("move_factor", "wobbly_move_factor", -1))

    root.stiffness  = st >= 0 ? st : root.presetStiffness[root.wobbliness]
    root.drag       = dr >= 0 ? dr : root.presetDrag[root.wobbliness]
    root.moveFactor = mv >= 0 ? mv : root.presetMove[root.wobbliness]

    var pe = Number(effective("elastic_period",      "elastic_period",      -1))
    var da = Number(effective("elastic_damping",     "elastic_damping",     -1))
    var ms = Number(effective("elastic_max_stretch", "elastic_max_stretch", -1))

    root.period     = pe >= 0 ? pe : root.presetPeriod[root.stretchiness]
    root.damping    = da >= 0 ? da : root.presetDamping[root.stretchiness]
    root.maxStretch = ms >= 0 ? ms : root.presetMaxStretch[root.stretchiness]

    root.pulseEnabled  = effective("pulse_enabled",   "pulse_enabled",   1) != 0
    root.onSwitch      = effective("pulse_on_switch", "pulse_on_switch", 1) != 0
    root.onClick       = effective("pulse_on_click",  "pulse_on_click",  1) != 0
    root.onHover       = effective("pulse_on_hover",  "pulse_on_hover",  0) != 0
    root.pulseStrength = root.clamp5(effective("pulse_strength", "pulse_strength", 2))

    var pa = Number(effective("pulse_amount",  "pulse_amount",  -1))
    var pp = Number(effective("pulse_period",  "pulse_period",  -1))
    var pd = Number(effective("pulse_damping", "pulse_damping", -1))

    root.pulseAmount  = pa >= 0 ? pa : root.presetPulseAmount[root.pulseStrength]
    root.pulsePeriod  = pp >= 0 ? pp : root.presetPulsePeriod[root.pulseStrength]
    root.pulseDamping = pd >= 0 ? pd : root.presetPulseDamping[root.pulseStrength]
  }

  // key=value, '#' starts a comment. Mirrors CEffectManager::loadSettings().
  function parseSettings(text) {
    var vals = {}
    var lines = String(text).split("\n")
    for (var i = 0; i < lines.length; i++) {
      var line = lines[i]
      var hash = line.indexOf("#")
      if (hash !== -1) line = line.substring(0, hash)

      var eq = line.indexOf("=")
      if (eq === -1) continue

      var key = line.substring(0, eq).trim()
      var raw = line.substring(eq + 1).trim()
      if (key === "" || raw === "") continue

      if (raw === "true" || raw === "false" || raw === "yes" || raw === "no")
        vals[key] = (raw === "true" || raw === "yes") ? 1 : 0
      else if (!isNaN(Number(raw)))
        vals[key] = Number(raw)
    }

    // Fold in what an older panel wrote, without letting it win over a key
    // written in the current spelling.
    for (var legacy in root.legacyKeys) {
      var target = root.legacyKeys[legacy]
      if (vals[target] === undefined && vals[legacy] !== undefined)
        vals[target] = vals[legacy]
    }

    root.fileVals = vals
    root.apply()
  }

  // `hyprctl -j getoption` reports the value under whichever field matches the
  // option's type. Its `set` field says whether the config assigned the option,
  // NOT whether the option exists — an unregistered one answers with the bare
  // text "no such option", which never gets past the JSON check in hyprProc. So
  // a `set:false` option still carries the plugin's registered default, and
  // that default is exactly what the effect is running with: read it, don't
  // discard it.
  function optValue(o) {
    if (!o) return undefined
    if (typeof o.float === "number") return o.float
    if (typeof o.int === "number") return o.int
    if (typeof o.bool === "boolean") return o.bool ? 1 : 0
    return undefined
  }

  function consumeHyprConfig(text) {
    try {
      var data = JSON.parse(text)
      var vals = {}
      var missing = false
      // Walk the keys we expect, not the keys that came back: an option the
      // plugin failed to register is absent from the reply entirely, so
      // iterating the reply could never notice it was gone.
      for (var i = 0; i < root.hyprKeys.length; i++) {
        var key = root.hyprKeys[i]
        var v = optValue(data[key])
        if (v === undefined) missing = true
        else vals[key] = v
      }
      root.hyprVals = vals
      // registerConfig() registers all of them or none.
      root.hyprOptionsMissing = missing
    } catch (e) {
      root.hyprVals = ({})
      root.hyprOptionsMissing = false
    }
    root.apply()
  }

  // ---- writing state -------------------------------------------------------

  // Writes the settings file and asks the plugin to pick it up. Only ever
  // called from a user interaction — seeding must not write back.
  function persist() {
    var lines = [
      "# omarchy-fx settings — written by the Omarchy shell plugin.",
      "# Values not listed here fall back to the Hyprland config.",
      "wobbly_enabled=" + (root.wobblyEnabled ? "true" : "false"),
      "wobbly_on_move=" + (root.onMove ? "true" : "false"),
      "wobbly_on_resize=" + (root.onResize ? "true" : "false"),
      "wobbliness=" + root.wobbliness,
      "elastic_enabled=" + (root.elasticEnabled ? "true" : "false"),
      "elastic_on_tiled=" + (root.onTiled ? "true" : "false"),
      "elastic_on_floating=" + (root.onFloating ? "true" : "false"),
      "elastic_on_workspace=" + (root.onWorkspace ? "true" : "false"),
      "stretchiness=" + root.stretchiness,
      "pulse_enabled=" + (root.pulseEnabled ? "true" : "false"),
      "pulse_on_switch=" + (root.onSwitch ? "true" : "false"),
      "pulse_on_click=" + (root.onClick ? "true" : "false"),
      "pulse_on_hover=" + (root.onHover ? "true" : "false"),
      "pulse_strength=" + root.pulseStrength,
      "wobbly_tessellation=" + root.tessellation,
      "elastic_tessellation=" + root.tessellation,
      "pulse_tessellation=" + root.tessellation,
      ""
    ]
    settingsFile.setText(lines.join("\n"))

    reloadProc.running = false
    reloadProc.running = true
  }

  // Slider drags fire continuously; coalesce them into one write.
  function persistSoon() { writeDebounce.restart() }

  // Middle-click on the bar widget: one switch for the lot.
  function toggleEnabled() {
    var next = !root.enabled
    root.wobblyEnabled = next
    root.elasticEnabled = next
    root.pulseEnabled = next
    persist()
  }

  Component.onCompleted: refresh()
  onOpenedChanged: if (opened) refresh()

  Timer {
    id: writeDebounce
    interval: 180
    onTriggered: root.persist()
  }

  // Keeps the bar icon honest when the plugin is loaded or unloaded behind our
  // back. One hyprctl call while closed, so a slow beat is plenty.
  Timer {
    interval: root.opened ? 2000 : 15000
    running: true
    repeat: true
    onTriggered: root.opened ? root.refresh() : loadedProc.restart()
  }

  // Is the compositor plugin loaded? Stock `hyprctl plugin list`; matched as
  // text so it works whether or not this Hyprland gives JSON for it.
  Process {
    id: loadedProc
    command: ["hyprctl", "plugin", "list"]
    function restart() { running = false; running = true }
    onRunningChanged: if (!running && !loadedCollector.sawOutput) root.pluginLoaded = false
    stdout: StdioCollector {
      id: loadedCollector
      property bool sawOutput: false
      waitForEnd: true
      onStreamFinished: {
        sawOutput = text.length > 0
        root.pluginLoaded = text.indexOf("omarchy-fx") !== -1
      }
    }
  }

  // The Hyprland-config side of the fallback chain, as one JSON object.
  Process {
    id: hyprProc
    command: ["sh", "-c",
      'sep=""; printf "{"; ' +
      'for k in ' + root.hyprKeys.join(" ") + '; do ' +
      '  v=$(hyprctl -j getoption "plugin:omarchy-fx:$k" 2>/dev/null); ' +
      '  case "$v" in "{"*) printf "%s\\"%s\\":%s" "$sep" "$k" "$v"; sep=",";; esac; ' +
      'done; printf "}"']
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.consumeHyprConfig(text)
    }
  }

  Process {
    id: reloadProc
    // Stock reload; the plugin re-reads the settings file on config.reloaded.
    command: ["hyprctl", "reload"]
    onExited: root.refresh()
  }

  FileView {
    id: settingsFile
    path: root.settingsPath
    atomicWrites: true
    printErrors: false
    onLoaded: root.parseSettings(text())
    // No file yet just means nothing is overridden.
    onLoadFailed: root.parseSettings("")
  }

  // ---- UI ------------------------------------------------------------------

  component EffectSlider: Item {
    id: sliderBlock
    property var    hostBar: null
    property string caption: ""
    property string valueText: ""
    property bool   live: false
    property int    minimum: 0
    property int    maximum: 4
    property int    step: 1
    property int    ticks: 5
    property int    value: 0
    signal picked(int v)
    signal committed(int v)

    width: parent ? parent.width : 0
    height: captionRow.implicitHeight + slider.implicitHeight + Style.space(4)
    opacity: sliderBlock.live ? 1 : 0.5

    Row {
      id: captionRow
      width: parent.width

      Text {
        width: parent.width - valueLabel.implicitWidth
        text: sliderBlock.caption
        textFormat: Text.PlainText
        color: Qt.darker(Color.popups.text, 1.4)
        font.family: sliderBlock.hostBar ? sliderBlock.hostBar.fontFamily : Style.font.family
        font.pixelSize: Style.font.caption
      }

      Text {
        id: valueLabel
        text: sliderBlock.valueText
        textFormat: Text.PlainText
        color: Color.popups.text
        font.family: sliderBlock.hostBar ? sliderBlock.hostBar.fontFamily : Style.font.family
        font.pixelSize: Style.font.caption
        font.bold: true
      }
    }

    PanelSlider {
      id: slider
      anchors.top: captionRow.bottom
      anchors.topMargin: Style.space(4)
      width: parent.width
      bar: sliderBlock.hostBar
      enabled: sliderBlock.live
      minimum: sliderBlock.minimum
      maximum: sliderBlock.maximum
      step: sliderBlock.step
      integer: true
      tickCount: sliderBlock.ticks
      value: sliderBlock.value
      onMoved: function(v) { sliderBlock.picked(Math.round(v)) }
      onReleased: function(v) { sliderBlock.committed(Math.round(v)) }
    }
  }

  // A trigger chip: one of several independent switches shown as a row of
  // bordered buttons, lit when on. Multi-select, so not a ButtonGroup.
  component OptionChip: Button {
    id: chip
    property bool live: true
    property bool on: false
    bordered: true
    selected: chip.on
    enabled: chip.live
    opacity: chip.live ? 1 : 0.5
    foreground: root.fg
    background: Color.popups.background
    fontFamily: root.fontFam
    fontSize: Style.font.bodySmall
  }

  // Muted one-liner under a control.
  component Hint: Text {
    width: parent ? parent.width : 0
    wrapMode: Text.WordWrap
    textFormat: Text.PlainText
    color: Qt.darker(root.fg, 1.5)
    font.family: root.fontFam
    font.pixelSize: Style.font.caption
  }

  // Which tab is showing. Not persisted: the panel opens on the wobble, the
  // effect people come for, and the other two are a click away.
  property string tab: "wobbly"
  // Glyphs are Nerd Font: nf-md-waves, nf-fa-arrows_h, nf-fa-dot_circle_o, nf-md-grid.
  readonly property var tabs: [
    { value: "wobbly",  label: "Wobbly",  icon: "󰞍" },
    { value: "elastic", label: "Elastic", icon: "" },
    { value: "pulse",   label: "Pulse",   icon: "" },
    { value: "mesh",    label: "Mesh",    icon: "󰋁" }
  ]

  KeyboardPanel {
    id: panel
    anchorItem: root.anchorItem
    owner: root.barIdentity
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(380))
    contentHeight: panel.fittedContentHeight(contentColumn.implicitHeight)

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      Column {
        id: contentColumn
        width: parent.width
        spacing: Style.space(10)

        Text {
          width: parent.width
          text: "Window effects"
          textFormat: Text.PlainText
          color: root.fg
          font.family: root.fontFam
          font.pixelSize: Style.font.heading
          font.bold: true
        }

        Text {
          width: parent.width
          visible: !root.pluginLoaded || root.configError
          wrapMode: Text.WordWrap
          textFormat: Text.PlainText
          color: root.bar ? root.bar.urgent : Color.urgent
          font.family: root.fontFam
          font.pixelSize: Style.font.bodySmall
          text: root.configError
            ? "The plugin loaded but could not register its options."
            : "The omarchy-fx Hyprland plugin is not loaded, so nothing will deform. Load it with:\nhyprctl plugin load ~/.local/share/hyprland/plugins/omarchy-fx.so"
        }

        // ---- tabs -----------------------------------------------------------

        ButtonGroup {
          options: root.tabs
          value: root.tab
          focusable: false
          foreground: root.fg
          background: Color.popups.background
          fontFamily: root.fontFam
          fontSize: Style.font.bodySmall
          onChanged: function(v) { root.tab = v }
        }

        PanelSeparator { foreground: root.fg }

        // ---- wobble ---------------------------------------------------------

        Column {
          width: parent.width
          spacing: Style.space(10)
          visible: root.tab === "wobbly"

          Toggle {
            width: parent.width
            enabled: root.pluginLoaded
            opacity: root.pluginLoaded ? 1 : 0.5
            label: "Wobbly windows"
            description: "Wobble a window while the pointer drags it"
            checked: root.wobblyEnabled
            foreground: root.fg
            fontFamily: root.fontFam
            onClicked: {
              root.wobblyEnabled = !root.wobblyEnabled
              root.persist()
            }
          }

          PanelSectionHeader {
            width: parent.width
            text: "Wobble on"
            foreground: root.fg
            fontFamily: root.fontFam
          }

          Flow {
            width: parent.width
            spacing: Style.spacing.md

            OptionChip {
              text: "Move"
              tooltipText: "SUPER + left mouse drag"
              live: root.pluginLoaded && root.wobblyEnabled
              on: root.onMove
              onClicked: { root.onMove = !root.onMove; root.persist() }
            }

            OptionChip {
              text: "Resize"
              tooltipText: "SUPER + right mouse drag"
              live: root.pluginLoaded && root.wobblyEnabled
              on: root.onResize
              onClicked: { root.onResize = !root.onResize; root.persist() }
            }
          }

          EffectSlider {
            hostBar: root.bar
            caption: "Wobbliness"
            valueText: root.wobblinessNames[root.clamp5(root.wobbliness)]
            live: root.pluginLoaded && root.wobblyEnabled
            value: root.wobbliness
            onPicked: function(v) { root.wobbliness = v; root.persistSoon() }
            onCommitted: function(v) { root.wobbliness = v; root.persist() }
          }

          Hint {
            visible: root.pluginLoaded && !root.configError
            text: "stiffness " + root.stiffness.toFixed(2)
              + " · drag " + root.drag.toFixed(2)
              + " · move factor " + root.moveFactor.toFixed(2)
          }
        }

        // ---- elasticity -----------------------------------------------------

        Column {
          width: parent.width
          spacing: Style.space(10)
          visible: root.tab === "elastic"

          Toggle {
            width: parent.width
            enabled: root.pluginLoaded
            opacity: root.pluginLoaded ? 1 : 0.5
            label: "Elastic moves"
            description: "Stretch a window when the layout moves it"
            checked: root.elasticEnabled
            foreground: root.fg
            fontFamily: root.fontFam
            onClicked: {
              root.elasticEnabled = !root.elasticEnabled
              root.persist()
            }
          }

          PanelSectionHeader {
            width: parent.width
            text: "Stretch on"
            foreground: root.fg
            fontFamily: root.fontFam
          }

          Flow {
            width: parent.width
            spacing: Style.spacing.md

            OptionChip {
              text: "Tiled"
              tooltipText: "Swapping, and any reflow of the layout"
              live: root.pluginLoaded && root.elasticEnabled
              on: root.onTiled
              onClicked: { root.onTiled = !root.onTiled; root.persist() }
            }

            OptionChip {
              text: "Floating"
              tooltipText: "Animated moves and resizes of floating windows"
              live: root.pluginLoaded && root.elasticEnabled
              on: root.onFloating
              onClicked: { root.onFloating = !root.onFloating; root.persist() }
            }

            OptionChip {
              text: "Workspace"
              tooltipText: "Carrying a window to another workspace"
              live: root.pluginLoaded && root.elasticEnabled
              on: root.onWorkspace
              onClicked: { root.onWorkspace = !root.onWorkspace; root.persist() }
            }
          }

          Hint {
            visible: root.onWorkspace && root.elasticEnabled
            text: "Workspace needs Hyprland's workspace animation, which Omarchy ships off. Turn it on in ~/.config/hypr/looknfeel.lua."
          }

          EffectSlider {
            hostBar: root.bar
            caption: "Stretchiness"
            valueText: root.stretchinessNames[root.clamp5(root.stretchiness)]
            live: root.pluginLoaded && root.elasticEnabled
            value: root.stretchiness
            onPicked: function(v) { root.stretchiness = v; root.persistSoon() }
            onCommitted: function(v) { root.stretchiness = v; root.persist() }
          }

          Hint {
            visible: root.pluginLoaded && !root.configError
            text: "period " + Math.round(root.period) + " ms"
              + " · damping " + root.damping.toFixed(2)
              + " · max stretch " + Math.round(root.maxStretch) + " px"
          }
        }

        // ---- pulse ----------------------------------------------------------

        Column {
          width: parent.width
          spacing: Style.space(10)
          visible: root.tab === "pulse"

          Toggle {
            width: parent.width
            enabled: root.pluginLoaded
            opacity: root.pluginLoaded ? 1 : 0.5
            label: "Focus pulse"
            description: "Swell the window that just became active"
            checked: root.pulseEnabled
            foreground: root.fg
            fontFamily: root.fontFam
            onClicked: {
              root.pulseEnabled = !root.pulseEnabled
              root.persist()
            }
          }

          PanelSectionHeader {
            width: parent.width
            text: "Pulse on"
            foreground: root.fg
            fontFamily: root.fontFam
          }

          Flow {
            width: parent.width
            spacing: Style.spacing.md

            OptionChip {
              text: "Switch"
              tooltipText: "Keyboard, a dispatcher, or focus moving on its own"
              live: root.pluginLoaded && root.pulseEnabled
              on: root.onSwitch
              onClicked: { root.onSwitch = !root.onSwitch; root.persist() }
            }

            OptionChip {
              text: "Click"
              tooltipText: "A click that gives a window focus"
              live: root.pluginLoaded && root.pulseEnabled
              on: root.onClick
              onClicked: { root.onClick = !root.onClick; root.persist() }
            }

            OptionChip {
              text: "Hover"
              tooltipText: "Focus following the mouse. Every tile you cross will swell"
              live: root.pluginLoaded && root.pulseEnabled
              on: root.onHover
              onClicked: { root.onHover = !root.onHover; root.persist() }
            }
          }

          EffectSlider {
            hostBar: root.bar
            caption: "Strength"
            valueText: root.pulseNames[root.clamp5(root.pulseStrength)]
            live: root.pluginLoaded && root.pulseEnabled
            value: root.pulseStrength
            onPicked: function(v) { root.pulseStrength = v; root.persistSoon() }
            onCommitted: function(v) { root.pulseStrength = v; root.persist() }
          }

          Hint {
            visible: root.pluginLoaded && !root.configError
            text: "swell " + Math.round(root.pulseAmount) + " px"
              + " · period " + Math.round(root.pulsePeriod) + " ms"
              + " · damping " + root.pulseDamping.toFixed(2)
          }
        }

        // ---- shared rendering -----------------------------------------------

        Column {
          width: parent.width
          spacing: Style.space(10)
          visible: root.tab === "mesh"

          EffectSlider {
            hostBar: root.bar
            caption: "Quads per axis"
            valueText: String(root.tessellation)
            live: root.pluginLoaded && root.enabled
            minimum: 4
            maximum: 40
            step: 2
            ticks: 0
            value: root.tessellation
            onPicked: function(v) { root.tessellation = v; root.persistSoon() }
            onCommitted: function(v) { root.tessellation = v; root.persist() }
          }

          Hint {
            text: "One mesh resolution for every effect. Higher is smoother and costs more per frame; 20 is what KWin uses."
          }
        }
      }
    }
  }
}
