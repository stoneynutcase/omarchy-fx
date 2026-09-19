import QtQuick
import qs.Ui

// Bar entry point for omarchy-fx. The icon reflects whether the compositor
// plugin is loaded and whether any effect is on; everything else lives in
// Panel.qml, which is loaded once so the widget can read its state.
BarWidget {
  id: root
  moduleName: "omarchy-fx"

  function injectPanel() {
    var target = panelLoader.item
    if (!target) return
    if ("bar" in target) target.bar = root.bar
    if ("settings" in target) target.settings = root.settings
    if ("anchorItem" in target) target.anchorItem = button
    if ("hostWidget" in target) target.hostWidget = root
  }

  function togglePanel() {
    if (panelLoader.item && panelLoader.item.toggle) panelLoader.item.toggle()
  }

  function toggleEffect() {
    if (panelLoader.item && panelLoader.item.toggleEnabled) panelLoader.item.toggleEnabled()
  }

  // Shape contract for shell.summon/hide/toggle routing: the bar looks for
  // open/close/opened on the bar-widget root, not on the nested panel.
  readonly property bool opened: panelLoader.item ? panelLoader.item.opened === true : false

  function open() {
    if (panelLoader.item && panelLoader.item.open) panelLoader.item.open()
  }

  function close() {
    if (panelLoader.item && panelLoader.item.close) panelLoader.item.close()
  }

  readonly property bool popoutSwitchClosing: panelLoader.item ? panelLoader.item.popoutSwitchClosing === true : false

  function closeForPopoutSwitch() {
    if (panelLoader.item) panelLoader.item.closeForPopoutSwitch()
  }

  // The painted glyph is the honest extent for the bar's open-panel underline.
  readonly property real openPanelIndicatorWidth: button.labelWidth

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  onBarChanged: injectPanel()
  onSettingsChanged: injectPanel()

  Loader {
    id: panelLoader
    active: true
    source: Qt.resolvedUrl("Panel.qml")
    visible: false
    onLoaded: {
      root.injectPanel()
      Qt.callLater(root.injectPanel)
    }
  }

  WidgetButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    // nf-fa-magic
    text: ""
    // Dim when every effect is off, or when the compositor plugin isn't loaded
    // at all — in both cases nothing on screen will deform.
    dimmed: panelLoader.item ? !(panelLoader.item.pluginLoaded && panelLoader.item.enabled) : true
    tooltipText: panelLoader.item
      ? (panelLoader.item.pluginLoaded
          ? (panelLoader.item.enabled ? "Window effects: on" : "Window effects: off")
          : "omarchy-fx plugin not loaded")
      : "Window effects"

    onPressed: function(b) {
      if (b === Qt.MiddleButton) root.toggleEffect()
      else if (b !== Qt.RightButton) root.togglePanel()
    }
  }
}
