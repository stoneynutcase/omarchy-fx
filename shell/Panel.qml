pragma ComponentBehavior: Bound
import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Ui

// Settings panel for the omarchy-fx Hyprland plugin.
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
  moduleName: "omarchy-fx.wobbly"
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

  property bool enabled: true
  property bool onMove: true
  property bool onResize: true
  property int  wobbliness: 1
  property int  tessellation: 20

  // Read-only echo of what the physics is actually running with, so a value
  // set in Lua is visible here rather than silently contradicting the sliders.
  property real stiffness: 0.10
  property real drag: 0.85
  property real moveFactor: 0.10

  readonly property var wobblinessNames: ["Rigid", "Subtle", "Springy", "Loose", "Jelly"]

  // KWin's pset[0..4]. Mirrors wobblyPreset() in src/WobblyModel.cpp, which
  // stays the source of truth — keep the two tables in step.
  readonly property var presetStiffness: [0.15, 0.10, 0.06, 0.03, 0.01]
  readonly property var presetDrag:      [0.80, 0.85, 0.90, 0.92, 0.97]
  readonly property var presetMove:      [0.10, 0.10, 0.10, 0.20, 0.25]

  readonly property string settingsPath: Quickshell.env("HOME") + "/.config/omarchy/omarchy-fx.conf"

  // Every option registerConfig() registers, under plugin:omarchy-fx:. The
  // query below and the did-it-register check both read off this one list.
  readonly property var hyprKeys: [
    "wobbly_enabled", "wobbly_on_move", "wobbly_on_resize", "wobbly_wobbliness",
    "wobbly_tessellation", "wobbly_stiffness", "wobbly_drag", "wobbly_move_factor"
  ]

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

  function apply() {
    root.enabled      = effective("enabled",      "wobbly_enabled",      1) != 0
    root.onMove       = effective("on_move",      "wobbly_on_move",      1) != 0
    root.onResize     = effective("on_resize",    "wobbly_on_resize",    1) != 0
    root.wobbliness   = Math.max(0, Math.min(4, Number(effective("wobbliness",   "wobbly_wobbliness",   1))))
    root.tessellation = Number(effective("tessellation", "wobbly_tessellation", 20))

    // The physics the plugin will actually run with: the preset for the current
    // wobbliness, with any non-negative per-parameter override applied on top.
    var st = Number(effective("stiffness",   "wobbly_stiffness",   -1))
    var dr = Number(effective("drag",        "wobbly_drag",        -1))
    var mv = Number(effective("move_factor", "wobbly_move_factor", -1))

    root.stiffness  = st >= 0 ? st : root.presetStiffness[root.wobbliness]
    root.drag       = dr >= 0 ? dr : root.presetDrag[root.wobbliness]
    root.moveFactor = mv >= 0 ? mv : root.presetMove[root.wobbliness]
  }

  // key=value, '#' starts a comment. Mirrors CWobblyManager::loadSettings().
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

      if (key === "enabled" || key === "on_move" || key === "on_resize")
        vals[key] = (raw === "true" || raw === "1" || raw === "yes") ? 1 : 0
      else if (!isNaN(Number(raw)))
        vals[key] = Number(raw)
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
      // registerConfig() registers all eight or none.
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
      "enabled=" + (root.enabled ? "true" : "false"),
      "on_move=" + (root.onMove ? "true" : "false"),
      "on_resize=" + (root.onResize ? "true" : "false"),
      "wobbliness=" + root.wobbliness,
      "tessellation=" + root.tessellation,
      ""
    ]
    settingsFile.setText(lines.join("\n"))

    reloadProc.running = false
    reloadProc.running = true
  }

  // Slider drags fire continuously; coalesce them into one write.
  function persistSoon() { writeDebounce.restart() }

  function toggleEnabled() {
    root.enabled = !root.enabled
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

  KeyboardPanel {
    id: panel
    anchorItem: root.anchorItem
    owner: root.barIdentity
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(340))
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
          text: "Wobbly windows"
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
            : "The omarchy-fx Hyprland plugin is not loaded, so nothing will wobble. Load it with:\nhyprctl plugin load ~/.local/share/hyprland/plugins/omarchy-fx.so"
        }

        PanelSeparator { foreground: root.fg }

        Toggle {
          width: parent.width
          enabled: root.pluginLoaded
          opacity: root.pluginLoaded ? 1 : 0.5
          label: "Enabled"
          description: "Wobble windows while they are dragged"
          checked: root.enabled
          foreground: root.fg
          fontFamily: root.fontFam
          onClicked: {
            root.enabled = !root.enabled
            root.persist()
          }
        }

        Toggle {
          width: parent.width
          enabled: root.pluginLoaded && root.enabled
          opacity: (root.pluginLoaded && root.enabled) ? 1 : 0.5
          label: "On move"
          description: "SUPER + left mouse drag"
          checked: root.onMove
          foreground: root.fg
          fontFamily: root.fontFam
          onClicked: {
            root.onMove = !root.onMove
            root.persist()
          }
        }

        Toggle {
          width: parent.width
          enabled: root.pluginLoaded && root.enabled
          opacity: (root.pluginLoaded && root.enabled) ? 1 : 0.5
          label: "On resize"
          description: "SUPER + right mouse drag"
          checked: root.onResize
          foreground: root.fg
          fontFamily: root.fontFam
          onClicked: {
            root.onResize = !root.onResize
            root.persist()
          }
        }

        PanelSeparator { foreground: root.fg }

        PanelSectionHeader {
          width: parent.width
          text: "Wobbliness"
          foreground: root.fg
          fontFamily: root.fontFam
        }

        Item {
          width: parent.width
          height: wobblinessRow.implicitHeight + wobblinessSlider.implicitHeight + Style.space(4)
          opacity: (root.pluginLoaded && root.enabled) ? 1 : 0.5

          Row {
            id: wobblinessRow
            width: parent.width

            Text {
              width: parent.width - wobblinessValue.implicitWidth
              text: "Less wobble to more"
              textFormat: Text.PlainText
              color: Qt.darker(root.fg, 1.4)
              font.family: root.fontFam
              font.pixelSize: Style.font.caption
            }

            Text {
              id: wobblinessValue
              text: root.wobblinessNames[Math.max(0, Math.min(4, root.wobbliness))]
              textFormat: Text.PlainText
              color: root.fg
              font.family: root.fontFam
              font.pixelSize: Style.font.caption
              font.bold: true
            }
          }

          PanelSlider {
            id: wobblinessSlider
            anchors.top: wobblinessRow.bottom
            anchors.topMargin: Style.space(4)
            width: parent.width
            bar: root.bar
            enabled: root.pluginLoaded && root.enabled
            minimum: 0
            maximum: 4
            step: 1
            integer: true
            tickCount: 5
            value: root.wobbliness
            onMoved: function(v) {
              root.wobbliness = Math.round(v)
              root.persistSoon()
            }
            onReleased: function(v) {
              root.wobbliness = Math.round(v)
              root.persist()
            }
          }
        }

        PanelSectionHeader {
          width: parent.width
          text: "Mesh resolution"
          foreground: root.fg
          fontFamily: root.fontFam
        }

        Item {
          width: parent.width
          height: tessRow.implicitHeight + tessSlider.implicitHeight + Style.space(4)
          opacity: (root.pluginLoaded && root.enabled) ? 1 : 0.5

          Row {
            id: tessRow
            width: parent.width

            Text {
              width: parent.width - tessValue.implicitWidth
              text: "Quads per axis"
              textFormat: Text.PlainText
              color: Qt.darker(root.fg, 1.4)
              font.family: root.fontFam
              font.pixelSize: Style.font.caption
            }

            Text {
              id: tessValue
              text: String(root.tessellation)
              textFormat: Text.PlainText
              color: root.fg
              font.family: root.fontFam
              font.pixelSize: Style.font.caption
              font.bold: true
            }
          }

          PanelSlider {
            id: tessSlider
            anchors.top: tessRow.bottom
            anchors.topMargin: Style.space(4)
            width: parent.width
            bar: root.bar
            enabled: root.pluginLoaded && root.enabled
            minimum: 4
            maximum: 40
            step: 2
            integer: true
            value: root.tessellation
            onMoved: function(v) {
              root.tessellation = Math.round(v)
              root.persistSoon()
            }
            onReleased: function(v) {
              root.tessellation = Math.round(v)
              root.persist()
            }
          }
        }

        PanelSeparator { foreground: root.fg }

        Text {
          width: parent.width
          visible: root.pluginLoaded && !root.configError
          wrapMode: Text.WordWrap
          textFormat: Text.PlainText
          color: Qt.darker(root.fg, 1.5)
          font.family: root.fontFam
          font.pixelSize: Style.font.caption
          text: "In effect: stiffness " + root.stiffness.toFixed(2)
            + " · drag " + root.drag.toFixed(2)
            + " · move factor " + root.moveFactor.toFixed(2)
        }
      }
    }
  }
}
