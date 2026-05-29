#!/usr/bin/env bash
# Breezy — toolchain bootstrap + build (Linux / macOS).
#
# Installs the build dependencies if they are missing, then builds the compiler:
#   - gcc   (C compiler)
#   - make  (GNU Make)
#   - nasm  (assembler)
#
# Usage:  ./build.sh        (chmod +x build.sh first, or run: bash build.sh)
set -euo pipefail

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

# Run a command as root when needed (and possible).
SUDO() {
  if   [ "$(id -u)" -eq 0 ]; then "$@"
  elif have sudo;            then sudo "$@"
  else err "root needed to install packages, but 'sudo' was not found. Install manually: $*"; exit 1
  fi
}

install_linux() {
  if   have apt-get; then SUDO apt-get update && SUDO apt-get install -y build-essential nasm
  elif have dnf;     then SUDO dnf install -y gcc make nasm
  elif have yum;     then SUDO yum install -y gcc make nasm
  elif have pacman;  then SUDO pacman -Sy --needed --noconfirm base-devel nasm
  elif have zypper;  then SUDO zypper install -y gcc make nasm
  elif have apk;     then SUDO apk add build-base nasm
  else err "no supported package manager (apt/dnf/yum/pacman/zypper/apk). Install gcc, make, nasm manually."; exit 1
  fi
}

install_mac() {
  # gcc/clang + make come from the Xcode Command Line Tools.
  if ! xcode-select -p >/dev/null 2>&1; then
    say "Installing Xcode Command Line Tools (provides gcc/clang + make)…"
    xcode-select --install || true
    err "Re-run ./build.sh once the Command Line Tools install completes."
    exit 1
  fi
  if ! have brew; then
    say "Installing Homebrew (for nasm)…"
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    if   [ -x /opt/homebrew/bin/brew ]; then eval "$(/opt/homebrew/bin/brew shellenv)"
    elif [ -x /usr/local/bin/brew   ]; then eval "$(/usr/local/bin/brew shellenv)"
    fi
  fi
  brew install nasm
}

cd "$(dirname "$0")"
say "Breezy build (Unix)"

missing=()
have gcc  || missing+=(gcc)
have make || missing+=(make)
have nasm || missing+=(nasm)

if [ "${#missing[@]}" -ne 0 ]; then
  say "Missing tools: ${missing[*]} — installing…"
  case "$(uname -s)" in
    Linux*)  install_linux ;;
    Darwin*) install_mac ;;
    *) err "unsupported OS '$(uname -s)'. Install gcc, make, nasm manually."; exit 1 ;;
  esac
else
  say "Toolchain already present."
fi

# Verify everything is now available.
ok=1
for t in gcc make nasm; do
  if have "$t"; then printf '    %-5s %s\n' "$t" "$("$t" --version 2>/dev/null | head -n1)"
  else err "$t is still missing after install"; ok=0; fi
done
[ "$ok" -eq 1 ] || { err "toolchain incomplete — see messages above."; exit 1; }

# The compiler sources arrive with Part 01 of the plan.
if [ ! -f src/main.c ]; then
  say "Toolchain ready."
  say "Compiler sources are not present yet — implement Part 01"
  say "(docs/superpowers/plans/breezy-compiler/01-compiler-core.md), then run ./build.sh again."
  exit 0
fi

say "Building the Breezy compiler…"
make
say "Done — the 'breezy' compiler is built."
say "Note: Part 01 emits Win64 assembly; runnable native output on Linux/macOS needs Part 07 (Linux target)."
