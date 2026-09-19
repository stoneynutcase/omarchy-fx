#!/usr/bin/env bash
# Remove omarchy-fx from the running compositor and from the Omarchy config.
set -euo pipefail

PLUGIN_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/hyprland/plugins"
HYPR_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
ENTRY="$HYPR_DIR/hyprland.lua"
SNIPPET='require("hypr.omarchy_fx")'

# Both the current widget id and the one it replaced, so an uninstall after an
# upgrade does not leave the old one behind.
SHELL_IDS=("omarchy-fx" "omarchy-fx.wobbly")
OMARCHY_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/omarchy"

HOOK="/etc/pacman.d/hooks/95-omarchy-fx-rebuild.hook"
RUNNER="/usr/local/bin/omarchy-fx-rebuild"

# Root, for the two hook paths. sudo can only ask for a password on a terminal;
# run from a launcher, a GUI or an agent there is none, and polkit's graphical
# agent (which Omarchy always runs) is what can still ask.
as_root() {
  if [[ -t 0 ]] || ! command -v pkexec >/dev/null 2>&1; then
    sudo "$@"
  else
    pkexec "$@"
  fi
}

hyprctl plugin unload "$PLUGIN_DIR/omarchy-fx.so" >/dev/null 2>&1 || true

# Disable before deleting, so the widget also leaves the bar layout in
# shell.json rather than lingering there as a dead id.
for id in "${SHELL_IDS[@]}"; do
  if [[ -d "$OMARCHY_DIR/plugins/$id" ]] && command -v omarchy-shell >/dev/null 2>&1; then
    omarchy plugin disable "$id" >/dev/null 2>&1 || true
  fi
done

rm -f "$PLUGIN_DIR/omarchy-fx.so" "$PLUGIN_DIR/omarchy-fx.so.stale" "$HYPR_DIR/omarchy_fx.lua"

# Left behind, the hook would rebuild a plugin nothing loads any more — and
# would fail the rebuild loudly on every Hyprland update once the checkout goes.
if [[ -e "$HOOK" || -e "$RUNNER" ]]; then
  echo ":: removing the pacman rebuild hook (needs root)"
  as_root rm -f "$HOOK" "$RUNNER" ||
    echo "warning: could not remove $HOOK / $RUNNER; delete them by hand." >&2
fi

# Settings written by the panel; ours alone, so it goes with the plugin.
rm -f "$OMARCHY_DIR/omarchy-fx.conf"

for id in "${SHELL_IDS[@]}"; do
  if [[ -d "$OMARCHY_DIR/plugins/$id" ]]; then
    rm -rf "${OMARCHY_DIR:?}/plugins/$id"
    echo ":: removed the shell plugin from $OMARCHY_DIR/plugins/$id"
  fi
done
command -v omarchy-shell >/dev/null 2>&1 && omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true

if [[ -f "$ENTRY" ]] && grep -qF "$SNIPPET" "$ENTRY"; then
  cp "$ENTRY" "$ENTRY.bak.$(date +%s)"
  sed -i "/^-- omarchy-fx: \(wobbly windows\|window effects\)$/d;\\|^$(printf '%s' "$SNIPPET" | sed 's/[][\\.*^$/]/\\&/g')\$|d" "$ENTRY"
  echo ":: removed '$SNIPPET' from $ENTRY (backup kept alongside it)"
fi

echo "Done."
