#!/bin/bash
# Build omarchy-fx and wire it into the Omarchy Hyprland config.
set -euo pipefail

# Every tool below comes from the system directories, not from whatever PATH
# the caller had, and nothing inherited can redirect the loader or the shell.
export PATH=/usr/local/bin:/usr/bin:/bin
unset LD_PRELOAD LD_LIBRARY_PATH LD_AUDIT BASH_ENV ENV CDPATH GLOBIGNORE
for f in $(compgen -A function); do unset -f "$f"; done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/hyprland/plugins"
PLUGIN_SO="$PLUGIN_DIR/omarchy-fx.so"
HYPR_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
ENTRY="$HYPR_DIR/hyprland.lua"
SNIPPET='require("hypr.omarchy_fx")'

# The bar widget and settings panel, as an Omarchy shell plugin. The id has to
# match the "id" in manifest.json — that is what names the directory.
SHELL_ID="omarchy-fx"
SHELL_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/omarchy/plugins/$SHELL_ID"

# The widget was one-effect-per-widget before, and named for the only effect
# there was. Left in place it would sit in the bar next to its replacement,
# writing the same settings file from a stale panel.
OLD_SHELL_ID="omarchy-fx.wobbly"
OLD_SHELL_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/omarchy/plugins/$OLD_SHELL_ID"

# Rebuild-on-update, so a Hyprland upgrade cannot leave a stale .so behind.
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

# --plugin-only rebuilds and reinstalls just the .so. It is what the pacman hook
# runs after a Hyprland update, when the config and the bar widget are already
# in place and only the binary has gone stale.
PLUGIN_ONLY=0
WANT_HOOK=1
for arg in "$@"; do
  case "$arg" in
    --plugin-only) PLUGIN_ONLY=1 ;;
    --no-hook)     WANT_HOOK=0 ;;
    -h|--help)     echo "usage: ${0##*/} [--plugin-only] [--no-hook]"; exit 0 ;;
    *)             echo "error: unknown option '$arg'" >&2; exit 1 ;;
  esac
done

if ! pkg-config --exists hyprland; then
  echo "error: Hyprland development headers not found. Install them with:" >&2
  echo "       omarchy pkg add hyprland" >&2
  exit 1
fi

# Skipped under --plugin-only: the hook runs mid-upgrade, where the headers are
# already the new ones and the running compositor is still the old one, so a
# mismatch there is expected rather than a problem.
if (( ! PLUGIN_ONLY )); then
  RUNNING="$(hyprctl version -j 2>/dev/null | sed -n 's/.*"commit": *"\([^"]*\)".*/\1/p' || true)"
  BUILT="$(sed -n 's/.*GIT_COMMIT_HASH *"\([^"]*\)".*/\1/p' /usr/include/hyprland/src/version.h)"
  if [[ -n "$RUNNING" && "$RUNNING" != "$BUILT" ]]; then
    echo "warning: headers ($BUILT) do not match the running Hyprland ($RUNNING)." >&2
    echo "         The plugin will refuse to load until both are on the same version." >&2
  fi
fi

echo ":: building"
make -C "$REPO" clean >/dev/null
make -C "$REPO"

# Install by rename, never in place. A running Hyprland has the old .so mmap'd,
# and truncating that file underneath it leaves the mapped pages with no file
# behind them — the next call into plugin code then takes SIGBUS and the
# compositor goes down. rename(2) keeps the old inode alive for whoever still
# has it open, and only new loads see the new binary.
echo ":: installing to $PLUGIN_DIR"
mkdir -p "$PLUGIN_DIR"
# The staging name is random, so nothing planted at a guessable path can be
# written through, and rename(2) replaces whatever is at the final name —
# a symlink included — rather than following it.
STAGED="$(mktemp "$PLUGIN_DIR/.omarchy-fx.XXXXXXXX")"
install -m 0755 "$REPO/omarchy-fx.so" "$STAGED"
mv -f "$STAGED" "$PLUGIN_SO"

if (( PLUGIN_ONLY )); then
  echo ":: rebuilt $PLUGIN_SO"
  exit 0
fi

# A backup is a fresh random-named file next to the original: never a
# guessable name something else could have put a symlink at.
backup() {
  local bak
  bak="$(mktemp "$1.bak.XXXXXXXX")"
  cat -- "$1" >"$bak"
  echo "$bak"
}

echo ":: installing config to $HYPR_DIR/omarchy_fx.lua"
mkdir -p "$HYPR_DIR"
if [[ -e "$HYPR_DIR/omarchy_fx.lua" ]]; then
  backup "$HYPR_DIR/omarchy_fx.lua" >/dev/null
fi
install -m 0644 "$REPO/hypr/omarchy_fx.lua" "$HYPR_DIR/omarchy_fx.lua"

# hyprland.lua is the user's own file and is appended to in place, which
# follows a symlink on purpose: dotfile setups keep it as one. What it is not
# allowed to be is someone else's file.
if [[ -f "$ENTRY" ]] && ! grep -qF "$SNIPPET" "$ENTRY"; then
  if [[ ! -O "$ENTRY" ]]; then
    echo "error: $ENTRY is not owned by you; not touching it." >&2
    exit 1
  fi
  backup "$ENTRY" >/dev/null
  printf '\n-- omarchy-fx: window effects\n%s\n' "$SNIPPET" >>"$ENTRY"
  echo ":: added '$SNIPPET' to $ENTRY (backup kept alongside it)"
fi

# The bar widget is optional: the compositor plugin works on its own, and this
# half only makes sense on an Omarchy shell.
if command -v omarchy-shell >/dev/null 2>&1; then
  # Disable before deleting, so the old id also leaves the bar layout in
  # shell.json rather than lingering there as a dead entry.
  if [[ -d "$OLD_SHELL_DIR" ]]; then
    echo ":: replacing the old '$OLD_SHELL_ID' widget"
    omarchy plugin disable "$OLD_SHELL_ID" >/dev/null 2>&1 || true
    rm -rf "$OLD_SHELL_DIR"
  fi

  # Added with `omarchy plugin add`, this checkout *is* the shell plugin
  # directory, and copying it onto itself is pointless. Otherwise the widget
  # files at the repo root go in.
  if [[ "$(realpath -m "$REPO")" == "$(realpath -m "$SHELL_DIR")" ]]; then
    echo ":: shell plugin is this checkout, nothing to copy"
  else
    echo ":: installing shell plugin to $SHELL_DIR"
    mkdir -p "$SHELL_DIR"
    install -m 0644 "$REPO"/manifest.json "$REPO"/BarWidget.qml "$REPO"/Panel.qml "$SHELL_DIR/"
  fi

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

# A stale plugin is not a cosmetic problem: the ABI check faults inside
# pluginInit, and the Hyprland config loads plugins during compositor startup,
# so an un-rebuilt .so can cost you the next login. The hook removes the need to
# remember. Root-owned paths, hence as_root — pass --no-hook to skip it.
if (( WANT_HOOK )); then
  if ! command -v pacman >/dev/null 2>&1; then
    echo ":: not a pacman system, skipping the rebuild hook"
  else
    echo ":: installing the rebuild hook (needs root)"

    # The runner is executed by root. The four values baked into it are
    # shell-quoted with printf %q, so a path with a quote, a dollar or a
    # space in it is a string in the script rather than code — and a newline
    # in one is refused outright, since %q would keep it but the file would
    # not read as intended.
    for v in "$REPO" "$HOME" "$PLUGIN_SO"; do
      if [[ "$v" == *$'\n'* ]]; then
        echo "error: a path with a newline in it cannot go in the rebuild hook." >&2
        exit 1
      fi
    done
    TMP_RUNNER="$(mktemp)"
    while IFS= read -r line; do
      if [[ "$line" == "# @CONFIG@" ]]; then
        printf 'REPO=%q\nRUN_AS=%q\nRUN_HOME=%q\nPLUGIN_SO=%q\n' "$REPO" "$(id -un)" "$HOME" "$PLUGIN_SO"
      else
        printf '%s\n' "$line"
      fi
    done <"$REPO/pacman/omarchy-fx-rebuild.in" >"$TMP_RUNNER"

    # One root call for both files: pkexec asks for the password on every
    # invocation, so two calls would mean two dialogs. `install` unlinks the
    # destination before creating it, so it never writes through a link.
    if as_root /bin/sh -c 'install -Dm 0755 "$1" "$2" && install -Dm 0644 "$3" "$4"' _ \
         "$TMP_RUNNER" "$RUNNER" "$REPO/pacman/95-omarchy-fx-rebuild.hook" "$HOOK"; then
      echo "   $HOOK"
      echo "   $RUNNER"
    else
      echo "warning: could not install the rebuild hook." >&2
      echo "         Re-run ./install.sh when root is available, or pass" >&2
      echo "         --no-hook to stop being asked. Without it, remember to" >&2
      echo "         re-run ./install.sh after every Hyprland update." >&2
    fi
    rm -f "$TMP_RUNNER"
  fi
fi

echo
echo "Done. Load it now without restarting Hyprland:"
echo "    hyprctl plugin load $PLUGIN_SO"
echo "Or just reload the config — it is picked up on the next Hyprland start."
