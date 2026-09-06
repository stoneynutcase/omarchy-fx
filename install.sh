#!/usr/bin/env bash
# Build omarchy-fx and wire it into the Omarchy Hyprland config.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/hyprland/plugins"
HYPR_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
ENTRY="$HYPR_DIR/hyprland.lua"
SNIPPET='require("hypr.omarchy_fx")'

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

echo
echo "Done. Load it now without restarting Hyprland:"
echo "    hyprctl plugin load $PLUGIN_DIR/omarchy-fx.so"
echo "Or just reload the config — it is picked up on the next Hyprland start."
