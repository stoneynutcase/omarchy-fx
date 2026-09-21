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
# agent (which Omarchy always runs) is what can still ask. Decided once, here:
# the hook step feeds root's stdin from a pipe, and by then fd 0 no longer says
# whether there is a terminal.
if [[ -t 0 ]] || ! command -v pkexec >/dev/null 2>&1; then
  ROOT_VIA=sudo
else
  ROOT_VIA=pkexec
fi
as_root() { "$ROOT_VIA" "$@"; }

# What root runs, verbatim, to install the hook. Root never opens a path this
# user can write to: the two files arrive on its stdin, framed as one line
# "<runner bytes> <hook bytes>" followed by both back to back, and the SHA-256
# of each is fixed in root's arguments at the moment of the prompt. Root reads
# the stream into a directory only it can see, checks both hashes against the
# arguments, and installs nothing unless both match. So content swapped on
# disk after the prompt was authorized never reaches the root-owned paths:
# the stream was produced from memory before the prompt, and a stream that
# somehow differs from what was hashed is refused.
read -r -d '' ROOT_INSTALL <<'EOF' || true
set -euo pipefail
export PATH=/usr/local/bin:/usr/bin:/bin
umask 022
runner_sha=$1 hook_sha=$2 runner_dst=$3 hook_dst=$4
fail() { echo "omarchy-fx: $*" >&2; exit 1; }
stage="$(mktemp -d)"
trap 'rm -rf -- "$stage"' EXIT
IFS=' ' read -r runner_len hook_len || fail "no content on stdin"
[[ "$runner_len" =~ ^[0-9]+$ && "$hook_len" =~ ^[0-9]+$ ]] || fail "malformed stream header"
head -c "$runner_len" >"$stage/runner"
head -c "$hook_len" >"$stage/hook"
[[ "$(head -c 1 | wc -c)" == 0 ]] || fail "trailing data on stdin"
[[ "$(sha256sum -- "$stage/runner" | cut -d' ' -f1)" == "$runner_sha" ]] ||
  fail "runner content does not match the hash it was authorized with; nothing installed"
[[ "$(sha256sum -- "$stage/hook" | cut -d' ' -f1)" == "$hook_sha" ]] ||
  fail "hook content does not match the hash it was authorized with; nothing installed"
install -Dm 0755 -- "$stage/runner" "$runner_dst"
install -Dm 0644 -- "$stage/hook" "$hook_dst"
EOF

# The user side of the same step. The runner text is built in memory from the
# template, the hook read in likewise, both hashed, and then streamed to root
# from those variables — nothing root receives is re-read from disk after the
# prompt. Returns non-zero if root refused or was not granted.
install_hook() {
  echo ":: installing the rebuild hook (needs root)"

  # The runner is executed by root. The four values baked into it are
  # shell-quoted with printf %q, so a path with a quote, a dollar or a space
  # in it is a string in the script rather than code — and a newline in one
  # is refused outright, since %q would keep it but the file would not read
  # as intended.
  local v
  for v in "$REPO" "$HOME" "$PLUGIN_SO"; do
    if [[ "$v" == *$'\n'* ]]; then
      echo "error: a path with a newline in it cannot go in the rebuild hook." >&2
      return 1
    fi
  done

  # $(...) drops trailing newlines; the sentinel keeps them.
  local runner hook line
  runner="$(
    while IFS= read -r line; do
      if [[ "$line" == "# @CONFIG@" ]]; then
        printf 'REPO=%q\nRUN_AS=%q\nRUN_HOME=%q\nPLUGIN_SO=%q\n' "$REPO" "$(id -un)" "$HOME" "$PLUGIN_SO"
      else
        printf '%s\n' "$line"
      fi
    done <"$REPO/pacman/omarchy-fx-rebuild.in"
    printf x
  )"
  runner="${runner%x}"
  hook="$(cat -- "$REPO/pacman/95-omarchy-fx-rebuild.hook"; printf x)"
  hook="${hook%x}"

  local runner_sha hook_sha runner_len hook_len
  runner_sha="$(printf '%s' "$runner" | sha256sum | cut -d' ' -f1)"
  hook_sha="$(printf '%s' "$hook" | sha256sum | cut -d' ' -f1)"
  runner_len="$(printf '%s' "$runner" | wc -c)"
  hook_len="$(printf '%s' "$hook" | wc -c)"

  # One root call for both files: pkexec asks for the password on every
  # invocation, so two calls would mean two dialogs. `install` unlinks the
  # destination before creating it, so it never writes through a link.
  if { printf '%s %s\n' "$runner_len" "$hook_len"; printf '%s%s' "$runner" "$hook"; } |
       as_root /bin/bash -c "$ROOT_INSTALL" omarchy-fx-hook "$runner_sha" "$hook_sha" "$RUNNER" "$HOOK"; then
    echo "   $HOOK"
    echo "   $RUNNER"
  else
    return 1
  fi
}

# --plugin-only rebuilds and reinstalls just the .so. It is what the pacman hook
# runs after a Hyprland update, when the config and the bar widget are already
# in place and only the binary has gone stale. --hook-only installs just the
# hook, for when root was not available the first time.
PLUGIN_ONLY=0
HOOK_ONLY=0
WANT_HOOK=1
for arg in "$@"; do
  case "$arg" in
    --plugin-only) PLUGIN_ONLY=1 ;;
    --hook-only)   HOOK_ONLY=1 ;;
    --no-hook)     WANT_HOOK=0 ;;
    -h|--help)     echo "usage: ${0##*/} [--plugin-only | --hook-only] [--no-hook]"; exit 0 ;;
    *)             echo "error: unknown option '$arg'" >&2; exit 1 ;;
  esac
done
if (( HOOK_ONLY )) && (( PLUGIN_ONLY || ! WANT_HOOK )); then
  echo "error: --hook-only cannot be combined with --plugin-only or --no-hook" >&2
  exit 1
fi

if (( HOOK_ONLY )); then
  if ! command -v pacman >/dev/null 2>&1; then
    echo "error: not a pacman system, there is no rebuild hook to install." >&2
    exit 1
  fi
  install_hook || { echo "error: could not install the rebuild hook." >&2; exit 1; }
  exit 0
fi

# Nothing is installed from here: the check only names what is missing. On
# Omarchy the headers come with the hyprland package itself.
if ! pkg-config --exists hyprland; then
  echo "error: Hyprland development headers not found (pkg-config cannot see hyprland)." >&2
  echo "       On Omarchy they are part of the hyprland package; install that and re-run." >&2
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
  elif ! install_hook; then
    echo "warning: could not install the rebuild hook." >&2
    echo "         Run ./install.sh --hook-only when root is available, or pass" >&2
    echo "         --no-hook to stop being asked. Without it, remember to" >&2
    echo "         re-run ./install.sh after every Hyprland update." >&2
  fi
fi

echo
echo "Done. Load it now without restarting Hyprland:"
echo "    hyprctl plugin load $PLUGIN_SO"
echo "Or just reload the config — it is picked up on the next Hyprland start."
