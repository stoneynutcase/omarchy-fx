#!/usr/bin/env bash
# Remove omarchy-fx from the running compositor and from the Omarchy config.
set -euo pipefail

PLUGIN_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/hyprland/plugins"
HYPR_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
ENTRY="$HYPR_DIR/hyprland.lua"
SNIPPET='require("hypr.omarchy_fx")'

hyprctl plugin unload "$PLUGIN_DIR/omarchy-fx.so" >/dev/null 2>&1 || true

rm -f "$PLUGIN_DIR/omarchy-fx.so" "$HYPR_DIR/omarchy_fx.lua"

if [[ -f "$ENTRY" ]] && grep -qF "$SNIPPET" "$ENTRY"; then
  cp "$ENTRY" "$ENTRY.bak.$(date +%s)"
  sed -i "/^-- omarchy-fx: wobbly windows$/d;\\|^$(printf '%s' "$SNIPPET" | sed 's/[][\\.*^$/]/\\&/g')\$|d" "$ENTRY"
  echo ":: removed '$SNIPPET' from $ENTRY (backup kept alongside it)"
fi

echo "Done."
