#!/bin/bash
# Exercises the root step of install.sh — the one that writes the pacman hook
# and its runner — against the two things a same-user process could try
# between the password prompt and root acting on it:
#
#   swap    both source files in the checkout are overwritten while the prompt
#           is open. Expected: the install completes, and what lands under
#           /usr/local/bin and /etc/pacman.d/hooks is byte for byte what was
#           prepared before the prompt; the swapped content is nowhere.
#   tamper  the stream root reads is replaced with different content of the
#           same shape while the prompt is open. Expected: root refuses, and
#           nothing at all is written to either root-owned path.
#
# No root is needed and nothing on the machine is touched: the test re-runs
# itself in a user and mount namespace, where it is uid 0, mounts an empty
# tmpfs over /usr/local/bin and /etc/pacman.d, and binds a stand-in over
# /usr/bin/sudo and /usr/bin/pkexec. The stand-in is the "authorization
# window": it announces that it is open, waits for the test to finish
# meddling, and then runs the command exactly as sudo would.
#
# Needs: unshare (util-linux), user namespaces enabled, and a pacman system
# (install.sh refuses the hook step elsewhere).
set -euo pipefail
export PATH=/usr/local/bin:/usr/bin:/bin

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -z "${FX_HOOK_TEST_INSIDE:-}" ]]; then
  for need in unshare pacman; do
    if ! command -v "$need" >/dev/null 2>&1; then
      echo "skip: $need not found" >&2
      exit 77
    fi
  done
  if ! unshare -Urm --propagation private /bin/true 2>/dev/null; then
    echo "skip: cannot create a user+mount namespace here" >&2
    exit 77
  fi
  exec unshare -Urm --propagation private /usr/bin/env FX_HOOK_TEST_INSIDE=1 "$ROOT/tests/hook-install.sh"
fi

# ---- inside the namespace: uid 0 in name only --------------------------------

T="$(mktemp -d)"
trap 'rm -rf -- "$T"' EXIT
mkdir -p "$T/repo/pacman" "$T/tmp"
cp -- "$ROOT/install.sh" "$T/repo/"
cp -- "$ROOT/pacman/omarchy-fx-rebuild.in" "$ROOT/pacman/95-omarchy-fx-rebuild.hook" "$T/repo/pacman/"

RUNNER=/usr/local/bin/omarchy-fx-rebuild
HOOK=/etc/pacman.d/hooks/95-omarchy-fx-rebuild.hook
MARK='# TAMPERED-BY-THE-TEST: this line must never be installed'

mount -t tmpfs tmpfs /usr/local/bin
mount -t tmpfs tmpfs /etc/pacman.d

# The stand-in for sudo/pkexec. $T is baked in; everything else is passed on.
cat >"$T/fake-root" <<EOF
#!/bin/bash
touch "$T/window-open"
until [[ -e "$T/window-closed" ]]; do sleep 0.02; done
if [[ -e "$T/stream" ]]; then exec "\$@" <"$T/stream"; fi
exec "\$@"
EOF
chmod 0755 "$T/fake-root"
for tool in /usr/bin/sudo /usr/bin/pkexec; do
  [[ -e "$tool" ]] && mount --bind "$T/fake-root" "$tool"
done

FAILED=0
check() {
  if "$@"; then
    echo "  ok    $*"
  else
    echo "  FAIL  $*"
    FAILED=1
  fi
}
not_exists() { [[ ! -e "$1" ]]; }
no_mark()    { ! grep -qF -- "$MARK" "$1"; }
mode_is()    { [[ "$(stat -c %a -- "$1")" == "$2" ]]; }
empty_dir()  { [[ -z "$(ls -A -- "$1")" ]]; }
has_line()   { grep -qF -- "$2" "$1"; }
runner_matches_template() {
  # Everything except the four generated lines must equal the template minus
  # its marker line.
  diff -q <(grep -v '^\(REPO\|RUN_AS\|RUN_HOME\|PLUGIN_SO\)=' "$1") \
          <(grep -v '^# @CONFIG@$' "$ROOT/pacman/omarchy-fx-rebuild.in") >/dev/null
}

# Runs install.sh --hook-only in the background, waits for the prompt window to
# open, runs $1 inside it, closes the window, and leaves the exit status in RC
# and the output in $T/out.
run_hook_install() {
  local meddle="$1"
  rm -f "$T/window-open" "$T/window-closed"
  set +e
  TMPDIR="$T/tmp" "$T/repo/install.sh" --hook-only </dev/null >"$T/out" 2>&1 &
  local pid=$!
  set -e
  local i
  for ((i = 0; i < 500; i++)); do
    [[ -e "$T/window-open" ]] && break
    sleep 0.02
  done
  if [[ ! -e "$T/window-open" ]]; then
    echo "  FAIL  the privilege prompt never opened; output:"
    sed 's/^/        /' "$T/out"
    kill "$pid" 2>/dev/null || true
    FAILED=1
    return 1
  fi
  "$meddle"
  touch "$T/window-closed"
  set +e
  wait "$pid"
  RC=$?
  set -e
}

# ---- swap ---------------------------------------------------------------------
echo "swap: both sources overwritten while the prompt is open"
swap_sources() {
  printf '#!/bin/bash\n%s\nexit 42\n' "$MARK" >"$T/repo/pacman/omarchy-fx-rebuild.in"
  printf '%s\n[Trigger]\n' "$MARK" >"$T/repo/pacman/95-omarchy-fx-rebuild.hook"
}
run_hook_install swap_sources
check test "$RC" -eq 0
check test -f "$RUNNER"
check test -f "$HOOK"
check mode_is "$RUNNER" 755
check mode_is "$HOOK" 644
check no_mark "$RUNNER"
check no_mark "$HOOK"
check runner_matches_template "$RUNNER"
check has_line "$RUNNER" "REPO=$T/repo"
check cmp -s "$HOOK" "$ROOT/pacman/95-omarchy-fx-rebuild.hook"
check bash -n "$RUNNER"
check empty_dir "$T/tmp"

# ---- tamper -------------------------------------------------------------------
echo "tamper: the stream root reads is replaced while the prompt is open"
rm -f "$RUNNER" "$HOOK"
cp -- "$ROOT/pacman/omarchy-fx-rebuild.in" "$ROOT/pacman/95-omarchy-fx-rebuild.hook" "$T/repo/pacman/"
feed_tampered_stream() {
  local runner hook
  runner="$(printf '#!/bin/bash\n%s\nexit 42\n' "$MARK")"$'\n'
  hook="$(cat -- "$ROOT/pacman/95-omarchy-fx-rebuild.hook")"$'\n'
  {
    printf '%s %s\n' "$(printf '%s' "$runner" | wc -c)" "$(printf '%s' "$hook" | wc -c)"
    printf '%s%s' "$runner" "$hook"
  } >"$T/stream"
}
run_hook_install feed_tampered_stream
rm -f "$T/stream"
check test "$RC" -ne 0
check not_exists "$RUNNER"
check not_exists "$HOOK"
check has_line "$T/out" "does not match the hash it was authorized with"
check empty_dir "$T/tmp"

if (( FAILED )); then
  echo "FAILED; last install.sh output:"
  sed 's/^/  /' "$T/out"
  exit 1
fi
echo "all passed"
