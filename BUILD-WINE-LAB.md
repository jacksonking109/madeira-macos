# Building the wine-lab port (x86_64 Wine 11.18 + DXMT v0.80, macOS)

PE side (needs llvm-mingw, DXMT's winemetal import lib, Wine's winecrt0/ntdll from the Wine build tree):

    M=~/wine-lab/toolchains/llvm-mingw-20260421-ucrt-macos-universal/bin; B=~/wine-lab/build-wine11
    $M/x86_64-w64-mingw32-clang -shared -O2 -Wall -Wno-unused-function -o out/d3d12.dll \
        src/pe/madeira_d3d12.c src/pe/madeira_ext.c src/pe/d3d12.def -Isrc/pe -Isrc \
        -I~/wine-lab/dxmt-src/src/winemetal -L~/wine-lab/dxmt-src/build64/src/winemetal -l:winemetal.dll.a \
        $B/libs/winecrt0/x86_64-windows/libwinecrt0.a $B/dlls/ntdll/x86_64-windows/libntdll.a -luuid -lole32
    $B/tools/winebuild/winebuild --builtin out/d3d12.dll

Unix side (DXIL via Apple's Metal Shader Converter; DXBC/airconv and AGS rewriting compiled out, they need LLVM):

    D=(-DMADEIRA_WINE_MACOS -DMADEIRA_NO_AIRCONV -DMADEIRA_NO_AGS)
    I=(-Isrc -Isrc/unix -I~/wine-lab/dxmt-src/src/winemetal -Ithird_party/metal-shader-converter/include)
    clang -arch x86_64 -O2 -fPIC -c src/unix/d3d12_unix.m -o out/d3d12_unix.o $D $I
    clang++ -arch x86_64 -O2 -fPIC -std=c++17 -x objective-c++ -c src/unix/madeira_ir_unix.mm -o out/madeira_ir_unix.o $D $I
    clang++ -arch x86_64 -dynamiclib -o out/d3d12.so out/d3d12_unix.o out/madeira_ir_unix.o \
        -Wl,-install_name,@rpath/d3d12.so -framework Foundation -framework Metal

libmetalirconverter.dylib (MSC 4.0 beta 2, x86_64 slice) goes next to d3d12.so, or set MADEIRA_MSC_DYLIB.

Install: d3d12.dll -> wine11/lib/wine/x86_64-windows/ (replaces Wine's vkd3d d3d12, which needs Vulkan this build
lacks; Wine picks builtins by export name, so a copy in system32 alone is not used), d3d12.so -> wine11/lib/wine/x86_64-unix/.

Config: madeira.cfg keys are read from the environment, e.g. fence-chain -> MADEIRA_FENCE_CHAIN.

Geometry/tessellation emulation (DXIL GS, DXIL HS+DS): d3d12.so builds the converter's object/mesh pipeline
(src/unix/d3d12_unix.m, new_geometry_emulation_pipeline_state). Host test, real converter VS/PS with stand-in
GS/HS/DS (no DXIL for those in this tree): see the header of tests/native/geom_emu_test.m. Known gap: DXIL
tessellation draws need 15360 bytes of object threadgroup memory, which madeira_d3d12.c asks for through
reserved words of the DrawMeshThreadgroups command that only Madeira's DXMT fork reads; stock DXMT v0.80 ignores them.
