#!/bin/sh
# Dragon install script.
#
#   curl -fsSL https://dragonlang.org/install.sh | sh
#
# Downloads the latest release build for your OS and CPU from GitHub and
# installs the toolchain (bin, lib, share, include) under a prefix. Fetching
# with curl is required as I am not going to fork out $99 for apple to allow
# dmg work via browser download with this method (similar to how rust does it)
# the binary just runs: no apple ceremenoy required.
#
# Environment overrides:
#   DRAGON_PREFIX=/opt/dragon    where to install (default: /usr/local)
#   DRAGON_VERSION=v0.0.1        pin a version   (default: latest release)
set -eu

REPO="rayattack/dragon"
PREFIX="${DRAGON_PREFIX:-/usr/local}"

say()  { printf 'dragon: %s\n' "$*"; }
err()  { printf 'dragon: error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

have curl || err "curl is required but was not found"
have tar  || err "tar is required but was not found"

os="$(uname -s)"
cpu="$(uname -m)"
case "$os" in
    Linux)  plat="linux" ;;
    Darwin) plat="macos" ;;
    *) err "unsupported OS '$os' - Windows users, see https://dragonlang.org/download" ;;
esac
case "$cpu" in
    x86_64|amd64)  arch="x86_64" ;;
    arm64|aarch64) arch="arm64"  ;;
    *) err "unsupported CPU '$cpu'" ;;
esac
if [ "$plat" = "linux" ] && [ "$arch" != "x86_64" ]; then
    err "Linux $arch is not built yet (x86_64 only) - build from source: https://dragonlang.org/docs/0003-installation"
fi

ver="${DRAGON_VERSION:-}"
if [ -z "$ver" ]; then
    say "finding the latest release"
    ver="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
        | grep '"tag_name"' | head -n1 \
        | sed 's/.*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/')"
    [ -n "$ver" ] || err "could not resolve the latest version (set DRAGON_VERSION to install a specific one)"
fi
num="${ver#v}"
asset="dragon-${num}-${plat}-${arch}.tar.gz"
url="https://github.com/$REPO/releases/download/${ver}/${asset}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

say "downloading ${asset} (${ver})"
curl -fSL --proto '=https' --tlsv1.2 -o "$tmp/pkg.tar.gz" "$url" \
    || err "download failed: $url"

# Verify the checksum when the sidecar is published alongside the asset.
if curl -fsSL -o "$tmp/pkg.sha256" "${url}.sha256" 2>/dev/null; then
    want="$(awk '{print $1}' "$tmp/pkg.sha256")"
    if   have sha256sum; then got="$(sha256sum "$tmp/pkg.tar.gz" | awk '{print $1}')"
    elif have shasum;    then got="$(shasum -a 256 "$tmp/pkg.tar.gz" | awk '{print $1}')"
    else got=""; fi
    if [ -n "$got" ]; then
        [ "$want" = "$got" ] || err "checksum mismatch (expected $want, got $got)"
        say "checksum verified"
    fi
fi

mkdir -p "$tmp/unpack"
tar xzf "$tmp/pkg.tar.gz" -C "$tmp/unpack"

# The archive's internal layout can differ (macOS: <root>/bin, older Linux
# builds: <root>/usr/bin), so find the directory that actually holds bin/dragon
# and treat its parent as the toolchain root. This keeps the installer correct
# across release versions regardless of packaging tweaks.
found="$(find "$tmp/unpack" -type f -name dragon -path '*/bin/dragon' | head -n1)"
[ -n "$found" ] || err "archive did not contain bin/dragon"
root="$(dirname "$(dirname "$found")")"

# TO sudo or not to sudo - if we can create and write the prefix, let's not
# test -w on a path that may not exist (a prefix whose parent is
# writable needs no sudo; /usr/local oft doth needeth it).
sudo=""
if mkdir -p "$PREFIX" 2>/dev/null && [ -w "$PREFIX" ]; then
    say "installing to $PREFIX"
elif have sudo; then
    sudo="sudo"
    say "installing to $PREFIX (needs sudo)"
    $sudo mkdir -p "$PREFIX"
else
    err "cannot write to $PREFIX and sudo is unavailable - set DRAGON_PREFIX to a writable directory"
fi

# Copy bin/lib/share/include as siblings under the prefix. -R preserves the
# `dr` symlink; the toolchain finds its stdlib relative to the binary, so all
# four directories must stay together.
copy="cp -R"
[ "$plat" = "macos" ] && copy="cp -RX"
$sudo $copy "$root/." "$PREFIX/"

# macOS: we need to clear any quarantine flag so the (ad-hoc signed) binary runs silently.
if [ "$plat" = "macos" ] && have xattr; then
    $sudo xattr -dr com.apple.quarantine "$PREFIX/bin/dragon" "$PREFIX/bin/dragon-egg" 2>/dev/null || true
fi

version_line="$("$PREFIX/bin/dragon" --version 2>/dev/null || echo "dragon ${num}")"
say "installed ${version_line}"
case ":$PATH:" in
    *":$PREFIX/bin:"*) say "run 'dragon --version' to get started" ;;
    *) say "add it to your PATH:  export PATH=\"$PREFIX/bin:\$PATH\"" ;;
esac
