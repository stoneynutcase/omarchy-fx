#!/usr/bin/env bash
# Build omarchy-fx and wire it into the Omarchy Hyprland config.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/hyprland/plugins"
HYPR_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
ENTRY="$HYPR_DIR/hyprland.lua"
SNIPPET='require("hypr.omarchy_fx")'

# The bar widget and settings panel, as an Omarchy shell plugin. The id has to
# match the "id" in shell/manifest.json — that is what names the directory.
SHELL_ID="omarchy-fx.wobbly"
SHELL_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/omarchy/plugins/$SHELL_ID"

if ! pkg-config --exists hyprland; then
  echo "error: Hyprland development headers not found. Install them with:" >&2
  echo "       omarchy pkg add hyprland" >&2
  exit 1
fi

RUNNING="$(hyprctl version -j 2>/dev/null | sed -n 's/.*"commit": *"\([^"]*\)".*/\1/p' || true)"
BUILT="$(sed -n 's/.*GIT_COMMIT_HASH *"\([^"]*\)".*/\1/p' /usr/include/hyprland/src/version.h)"
if [[ -n "$RUNNING" && "$RUNNING" != "$BUILT" ]]; then
  echo "warning: headers ($BUILT) do not match the running Hyprland ($RUNNING)." >&2
  echo "         The plugin will refuse to load until both are on the same version." >&2
fi

echo ":: building"
make -C "$REPO" clean >/dev/null
make -C "$REPO"

echo ":: installing to $PLUGIN_DIR"
mkdir -p "$PLUGIN_DIR"
install -m 0755 "$REPO/omarchy-fx.so" "$PLUGIN_DIR/omarchy-fx.so"

echo ":: installing config to $HYPR_DIR/omarchy_fx.lua"
mkdir -p "$HYPR_DIR"
if [[ -e "$HYPR_DIR/omarchy_fx.lua" ]]; then
  cp "$HYPR_DIR/omarchy_fx.lua" "$HYPR_DIR/omarchy_fx.lua.bak.$(date +%s)"
fi
install -m 0644 "$REPO/hypr/omarchy_fx.lua" "$HYPR_DIR/omarchy_fx.lua"

if [[ -f "$ENTRY" ]] && ! grep -qF "$SNIPPET" "$ENTRY"; then
  cp "$ENTRY" "$ENTRY.bak.$(date +%s)"
  printf '\n-- omarchy-fx: wobbly windows\n%s\n' "$SNIPPET" >>"$ENTRY"
  echo ":: added '$SNIPPET' to $ENTRY (backup kept alongside it)"
fi

# The bar widget is optional: the compositor plugin works on its own, and this
# half only makes sense on an Omarchy shell.
if command -v omarchy-shell >/dev/null 2>&1; then
  echo ":: installing shell plugin to $SHELL_DIR"
  mkdir -p "$SHELL_DIR"
  install -m 0644 "$REPO"/shell/manifest.json "$REPO"/shell/*.qml "$SHELL_DIR/"

  omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true

  # Enable on first install only. Re-running must not drag the widget back to
  # the right section after the user has moved it with `omarchy bar move`.
  if omarchy plugin list --json 2>/dev/null |
    jq -e --arg id "$SHELL_ID" 'any(.[]; .id == $id and .enabled)' >/dev/null 2>&1; then
    echo ":: shell plugin already enabled, leaving its bar placement alone"
  elif omarchy plugin enable "$SHELL_ID" --section right >/dev/null 2>&1; then
    echo ":: added the widget to the right of the bar (omarchy bar move $SHELL_ID to relocate)"
  else
    echo ":: could not enable the widget automatically. Enable it with:" >&2
    echo "       omarchy plugin enable $SHELL_ID --section right" >&2
  fi
else
  echo ":: omarchy-shell not found, skipping the bar widget"
fi

echo
echo "Done. Load it now without restarting Hyprland:"
echo "    hyprctl plugin load $PLUGIN_DIR/omarchy-fx.so"
echo "Or just reload the config — it is picked up on the next Hyprland start."
