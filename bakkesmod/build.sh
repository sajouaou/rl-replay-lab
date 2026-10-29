#!/usr/bin/env bash
# Compile ReplayLab.dll (BakkesMod plugin) on Linux with clang-cl, using the Microsoft
# C++ runtime and Windows SDK headers fetched by xwin into toolchain/msvc.
# BakkesMod plugins must use the MSVC ABI and the dynamic runtime (/MD), like BakkesMod itself.
set -euo pipefail
cd "$(dirname "$0")"
MSVC=toolchain/msvc
SDK=sdk
[ -d imgui_src/imgui ] || { mkdir -p imgui_src && unzip -q -o "$SDK/imgui_bm.zip" -d imgui_src; }   # ImGui as used by BakkesMod
[ -d "$MSVC/crt" ] || { echo "toolchain/msvc manquant : lance d'abord l'etape xwin (voir README)"; exit 1; }
mkdir -p build
# replay reader (Rust, subtr-actor) as a Windows DLL; cargo only rebuilds what changed
# (run from the crate folder so cargo picks up rlab_replay/.cargo/config.toml: lld-link + xwin libraries)
LIBS="$PWD/$MSVC"
export CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS="-C target-feature=+crt-static -Lnative=$LIBS/crt/lib/x86_64 -Lnative=$LIBS/sdk/lib/um/x86_64 -Lnative=$LIBS/sdk/lib/ucrt/x86_64"
(cd rlab_replay && systemd-run --user --scope --quiet --collect -p MemoryMax=1500M -p MemorySwapMax=0 \
    "$HOME/.cargo/bin/cargo" build --release -j1 --lib --target x86_64-pc-windows-msvc)
cp rlab_replay/target/x86_64-pc-windows-msvc/release/rlab_replay.dll build/
# Nexto's network: fast float math and AVX2 (0.3 ms per decision instead of 2.8; same actions as the original model)
clang-cl --target=x86_64-pc-windows-msvc /nologo /std:c++17 /EHsc /MD /O2 /fp:fast /arch:AVX2 /utf-8 /c \
    /imsvc "$MSVC/crt/include" /imsvc "$MSVC/sdk/include/ucrt" /imsvc "$MSVC/sdk/include/um" /imsvc "$MSVC/sdk/include/shared" \
    nexto/nexto.cpp /Fobuild/nexto.obj
clang-cl --target=x86_64-pc-windows-msvc /nologo /std:c++17 /EHsc /MD /O2 /utf-8 /LD \
    -Wno-unused-command-line-argument -Wno-ignored-pragmas -Wno-microsoft-template \
    /imsvc "$MSVC/crt/include" /imsvc "$MSVC/sdk/include/ucrt" /imsvc "$MSVC/sdk/include/um" /imsvc "$MSVC/sdk/include/shared" \
    /I "$SDK/include" /I imgui_src /I nexto \
    ReplayLab/*.cpp build/nexto.obj imgui_src/imgui/imgui.cpp imgui_src/imgui/imgui_draw.cpp \
    imgui_src/imgui/imgui_widgets.cpp imgui_src/imgui/imgui_demo.cpp /Fobuild/ /Febuild/ReplayLab.dll \
    -fuse-ld=lld-link /link /LIBPATH:"$MSVC/crt/lib/x86_64" /LIBPATH:"$MSVC/sdk/lib/um/x86_64" \
    /LIBPATH:"$MSVC/sdk/lib/ucrt/x86_64" "$SDK/lib/pluginsdk.lib" /MAP:build/ReplayLab.map
ls -la build/ReplayLab.dll build/rlab_replay.dll
