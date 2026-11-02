#!/bin/zsh
# Build the unix side, out/d3d12.so (see BUILD-WINE-LAB.md). Run from the repository root.
#
# MADEIRA_IR_CONVERTER_ID is a hash of everything that shapes a shader conversion. It keys the
# persistent shader caches (DXIL .mdxc, DXBC .mdsc), so a rebuild that changes none of these inputs
# keeps every cached conversion; without it the key fell back to the build's date and time, and
# every rebuild converted all of a game's shaders again (~23,000 for The Last of Us Part I).
set -e
DX=${DX:-$HOME/wine-lab/dxmt-src}; LL=${LL:-$HOME/wine-lab/deps/llvm15-x86_64}
CONV_INPUTS=(
    src/madeira_ir_abi.h
    src/unix/madeira_ir_unix.mm src/unix/madeira_dxil_cache.h src/unix/madeira_cfg.h
    src/unix/madeira_dxil_rewrite.cpp src/unix/madeira_dxil_rewrite.h
    src/unix/madeira_air_rewrite.cpp src/unix/madeira_air_rewrite.h
    src/unix/madeira_sm5_ia.cpp
    third_party/metal-shader-converter/include/*/*.h
    $DX/build-full/src/airconv/darwin/libairconv.a $DX/build-full/libs/DXBCParser/libDXBCParserNative.a
    $LL/include/llvm/Config/llvm-config.h
    build-so.sh
)
CID=$(cat $CONV_INPUTS | shasum -a 256 | cut -c1-16)
echo "converter id $CID"
D=(-DMADEIRA_WINE_MACOS -DMADEIRA_NO_AGS "-DMADEIRA_IR_CONVERTER_ID=\"$CID\"")
I=(-Isrc -Isrc/unix -I$DX/src/winemetal -I$DX/src/airconv -Ithird_party/metal-shader-converter/include)
IA=(-Isrc -I$DX/src/airconv -I$DX/libs -I$DX/include/native/windows -I$DX/include/native/directx)
mkdir -p out
clang -arch x86_64 -O2 -fPIC -c src/unix/d3d12_unix.m -o out/d3d12_unix.o $D $I
clang++ -arch x86_64 -O2 -fPIC -std=c++17 -x objective-c++ -c src/unix/madeira_ir_unix.mm -o out/madeira_ir_unix.o $D $I
clang++ -arch x86_64 -O2 -fPIC -std=c++20 -Wno-extern-c-compat -Wno-unknown-pragmas \
    -c src/unix/madeira_sm5_ia.cpp -o out/madeira_sm5_ia.o $D $IA
clang++ -arch x86_64 -O2 -fPIC -std=c++17 -fno-rtti -c src/unix/madeira_dxil_rewrite.cpp -o out/madeira_dxil_rewrite.o -I$LL/include
clang++ -arch x86_64 -O2 -fPIC -std=c++17 -fno-rtti -c src/unix/madeira_air_rewrite.cpp -o out/madeira_air_rewrite.o -I$LL/include
# LLVM components: llvm_deps in dxmt-src/src/airconv/meson.build
LLVM_LIBS=(Passes Target ObjCARCOpts Coroutines ipo Instrumentation Vectorize Linker IRReader AsmParser
    FrontendOpenMP ScalarOpts InstCombine AggressiveInstCombine TransformUtils BitWriter Analysis ProfileData
    Symbolize DebugInfoPDB DebugInfoMSF DebugInfoDWARF Object TextAPI MCParser MC DebugInfoCodeView BitReader
    Core Remarks BitstreamReader BinaryFormat Support Demangle)
AIRCONV=($DX/build-full/src/airconv/darwin/libairconv.a $DX/build-full/libs/DXBCParser/libDXBCParserNative.a
    $LL/lib/libLLVM${^LLVM_LIBS}.a -lz -lcurses)
# export only what Wine and the tests look up, so the LLVM copy here never coalesces with winemetal.so's
EXP=(-Wl,-exported_symbol,___wine_unix_call_funcs -Wl,-exported_symbol,_madeira_ir_convert
    -Wl,-exported_symbol,_madeira_ir_convert_impl -Wl,-exported_symbol,_madeira_ags_rewrite)
clang++ -arch x86_64 -dynamiclib -o out/d3d12.so out/d3d12_unix.o out/madeira_ir_unix.o out/madeira_sm5_ia.o \
    out/madeira_dxil_rewrite.o out/madeira_air_rewrite.o $AIRCONV $EXP -Wl,-install_name,@rpath/d3d12.so \
    -framework Foundation -framework Metal
echo "built out/d3d12.so"
