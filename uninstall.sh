#!/usr/bin/env bash
# Remove omarchy-fx from the running compositor and from the Omarchy config.
set -euo pipefail

PLUGIN_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/hyprland/plugins"
HYPR_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
ENTRY="$HYPR_DIR/hyprland.lua"
SNIPPET='require("hypr.omarchy_fx")'

SHELL_ID="omarchy-fx.wobbly"
OMARCHY_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/omarchy"
SHELL_DIR="$OMARCHY_DIR/plugins/$SHELL_ID"

HOOK="/etc/pacman.d/hooks/95-omarchy-fx-rebuild.hook"
RUNNER="/usr/local/bin/omarchy-fx-rebuild"

hyprctl plugin unload "$PLUGIN_DIR/omarchy-fx.so" >/dev/null 2>&1 || true

# Disable before deleting, so the widget also leaves the bar layout in
# shell.json rather than lingering there as a dead id.
if [[ -d "$SHELL_DIR" ]] && command -v omarchy-shell >/dev/null 2>&1; then
  omarchy plugin disable "$SHELL_ID" >/dev/null 2>&1 || true
fi

rm -f "$PLUGIN_DIR/omarchy-fx.so" "$PLUGIN_DIR/omarchy-fx.so.stale" "$HYPR_DIR/omarchy_fx.lua"

# Left behind, the hook would rebuild a plugin nothing loads any more — and
# would fail the rebuild loudly on every Hyprland update once the checkout goes.
if [[ -e "$HOOK" || -e "$RUNNER" ]]; then
  echo ":: removing the pacman rebuild hook (needs sudo)"
  sudo rm -f "$HOOK" "$RUNNER" ||
    echo "warning: could not remove $HOOK / $RUNNER; delete them by hand." >&2
fi

# Settings written by the panel; ours alone, so it goes with the plugin.
rm -f "$OMARCHY_DIR/omarchy-fx.conf"

if [[ -d "$SHELL_DIR" ]]; then
  rm -rf "$SHELL_DIR"
  echo ":: removed the shell plugin from $SHELL_DIR"
  command -v omarchy-shell >/dev/null 2>&1 && omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
fi

if [[ -f "$ENTRY" ]] && grep -qF "$SNIPPET" "$ENTRY"; then
  cp "$ENTRY" "$ENTRY.bak.$(date +%s)"
  sed -i "/^-- omarchy-fx: wobbly windows$/d;\\|^$(printf '%s' "$SNIPPET" | sed 's/[][\\.*^$/]/\\&/g')\$|d" "$ENTRY"
  echo ":: removed '$SNIPPET' from $ENTRY (backup kept alongside it)"
fi

echo "Done."
