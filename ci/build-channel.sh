#!/bin/bash
# Build omarchy-fx against the Hyprland an Omarchy channel ships today.
#
#   ci/build-channel.sh <stable|rc|edge>
#
# Runs as root inside a throwaway Arch container (archlinux:base-devel) with
# the checkout as the working directory; it rewrites the container's pacman
# setup, so never run it on a real machine.
#
# Omarchy channels are not "Arch, some days later": stable pins a snapshot
# mirror, and edge ships its own hyprland build from the Omarchy repository,
# ahead of Arch. So the channel's own pacman.conf and mirrorlist are taken from
# upstream Omarchy each run, and whatever hyprland that resolves to is what the
# plugin is built against — the same headers `install.sh` would find.
set -euo pipefail

CHANNEL="${1:?usage: $0 <stable|rc|edge>}"
case "$CHANNEL" in stable | rc | edge) ;; *) echo "unknown channel: $CHANNEL" >&2; exit 2 ;; esac

OMARCHY_RAW="https://raw.githubusercontent.com/omacom/omarchy/HEAD/default/pacman"

# The key Omarchy signs its repository with, pinned. Its key material comes
# from the omarchy-keyring package, but only this fingerprint is trusted: a
# keyring that carries a different key fails the run instead of being believed.
OMARCHY_KEY="40DFB630FF42BCFFB047046CF0134EE680CAC571"

echo "::group::pacman: Omarchy $CHANNEL channel"
curl -fsSL "$OMARCHY_RAW/pacman-$CHANNEL.conf" -o /etc/pacman.conf
curl -fsSL "$OMARCHY_RAW/mirrorlist-$CHANNEL" -o /etc/pacman.d/mirrorlist
# pacman 7's download sandbox needs kernel features container runtimes do not
# always grant; this container is disposable, so do without it.
sed -i '/^\[options\]/a DisableSandbox' /etc/pacman.conf
grep -E '^(Server|\[)' /etc/pacman.conf /etc/pacman.d/mirrorlist

pacman-key --init >/dev/null
pacman-key --populate archlinux >/dev/null

REPO="$(sed -n 's/^Server *= *//p' /etc/pacman.conf | sed 's/\$arch/x86_64/' | head -1)"
KEYRING_PKG="$(curl -fsSL "$REPO/omarchy.db" | bsdtar -xOf - '*/desc' | grep -A1 '^%FILENAME%$' | grep -o '^omarchy-keyring-.*\.pkg\.tar\.zst$' | head -1)"
[[ -n $KEYRING_PKG ]] || { echo "omarchy-keyring not found in $REPO" >&2; exit 1; }
KEYDIR="$(mktemp -d)"
curl -fsSL "$REPO/$KEYRING_PKG" | bsdtar -xf - -C "$KEYDIR" usr/share/pacman/keyrings/omarchy.gpg
pacman-key --add "$KEYDIR/usr/share/pacman/keyrings/omarchy.gpg" >/dev/null
pacman-key --lsign-key "$OMARCHY_KEY" >/dev/null
echo "::endgroup::"

echo "::group::pacman: sync and install hyprland"
# Edge tracks current Arch, so the container upgrades to it like any machine.
# Stable and rc serve a snapshot that is usually older than the container
# image, and downgrading the base system to it breaks on any package Arch has
# split or renamed since. The build only reads Hyprland's headers and those of
# its dependencies, none of which the image carries, so there it installs just
# hyprland and leaves the base alone.
if [[ $CHANNEL == edge ]]; then SYNC=-Syu; else SYNC=-Sy; fi
pacman "$SYNC" --noconfirm --needed hyprland pkgconf binutils
echo "::endgroup::"

VERSION="$(pacman -Q hyprland)"
echo "Omarchy $CHANNEL ships $VERSION ($(pacman -Si hyprland | sed -n 's/^Repository *: *//p' | head -1))"
[[ -n ${GITHUB_STEP_SUMMARY:-} ]] && echo "**$CHANNEL**: \`$VERSION\`" >>"$GITHUB_STEP_SUMMARY"

echo "::group::build"
make clean >/dev/null
make -j"$(nproc)"
echo "::endgroup::"

# Compiling proves the headers agree with the plugin; loading also needs every
# symbol the plugin calls to be exported by that Hyprland build or a library it
# links. A missing one here is a plugin that builds and then refuses to load.
echo "::group::symbols"
DEFINED="$(mktemp)"
{
  nm -D --defined-only /usr/bin/Hyprland
  ldd /usr/bin/Hyprland | awk '/=> \//{print $3}' | xargs -r nm -D --defined-only
} 2>/dev/null | awk '{print $NF}' | sed 's/@.*//' | sort -u >"$DEFINED"

MISSING="$(nm -D --undefined-only omarchy-fx.so | awk '$1 == "U" {print $2}' | sed 's/@.*//' | sort -u | comm -23 - "$DEFINED")"
echo "::endgroup::"

if [[ -n $MISSING ]]; then
  echo "::error::omarchy-fx.so needs symbols $VERSION does not export:"
  c++filt <<<"$MISSING"
  exit 1
fi

echo "ok: omarchy-fx builds and links against $VERSION"
