#!/usr/bin/env bash
# Builds the BBSC mod for Linux: libbbsc.so, which a host (bbport) loads and starts with bbsc_attach
# (bbsc_host.h). The same sources as build-mod.cmd, with bbsc_platform_posix.cpp for the Windows
# side. Output: <repo>/build/mod/ ; log: <repo>/build/mod-build.log (ends with exit=N).
# Needs a C++23 compiler (CXX, default c++) and the SDL3 headers (externals/sdl3, else the
# system's).
set -uo pipefail
src=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
out=$src/../../build/mod
log=$src/../../build/mod-build.log
ext=$src/../externals
mkdir -p "$out"
sdl=()
if [[ -d $ext/sdl3/include/SDL3 ]]; then
    sdl=(-I"$ext/sdl3/include")
elif pkg-config --exists sdl3 2>/dev/null; then
    read -ra sdl <<<"$(pkg-config --cflags sdl3)"
fi
echo building >"$log"
if "${CXX:-c++}" -std=c++23 -O2 -fPIC -shared -fvisibility=hidden -pthread -Wall -Wextra \
    -DBBSC_MOD -DFMT_HEADER_ONLY -I"$src/shim" -I"$src/../src" -I"$ext/fmt/include" \
    -I"$ext/xbyak" "${sdl[@]}" \
    "$src/bbsc_mod.cpp" "$src/bbsc_hooks.cpp" "$src/bbsc_relay.cpp" "$src/bbsc_channel.cpp" \
    "$src/bbsc_http.cpp" "$src/bbsc_discord.cpp" "$src/bbsc_platform_posix.cpp" \
    "$src/../src/core/bbsc_memscan.cpp" "$src/../src/core/bloodborne_re.cpp" \
    -Wl,--no-undefined -o "$out/libbbsc.so" >>"$log" 2>&1; then
    echo exit=0 >>"$log"
else
    echo exit=1 >>"$log"
    exit 1
fi
