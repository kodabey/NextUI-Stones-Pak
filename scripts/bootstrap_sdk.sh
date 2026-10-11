#!/usr/bin/env bash
# Downloads TrimUI's official tg5040 cross toolchain (Linaro GCC 7.5) and SDK userland
# (SDL2 headers/libs) into ./sdk. About 400 MB download, ~1.5 GB unpacked.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="$ROOT/sdk"
BASE="https://github.com/trimui/toolchain_sdk_smartpro/releases/download/20231018"
mkdir -p "$SDK/downloads" "$SDK/usr" "$SDK/tc"
for f in SDK_usr_tg5040_a133p.tgz aarch64-linux-gnu-7.5.0-linaro.tgz; do
  [ -f "$SDK/downloads/$f" ] || curl -fL "$BASE/$f" -o "$SDK/downloads/$f"
done
tar -xzf "$SDK/downloads/SDK_usr_tg5040_a133p.tgz" -C "$SDK/usr"
tar -xzf "$SDK/downloads/aarch64-linux-gnu-7.5.0-linaro.tgz" -C "$SDK/tc"
test -x "$SDK/tc/aarch64-linux-gnu-7.5.0-linaro/bin/aarch64-linux-gnu-gcc"
test -f "$SDK/usr/usr/include/SDL2/SDL.h"
echo "SDK ready in $SDK"
