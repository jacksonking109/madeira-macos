# Building the wine-lab port (x86_64 Wine 11.18 + DXMT v0.80, macOS)

PE side (needs llvm-mingw, DXMT's winemetal import lib, Wine's winecrt0/ntdll from the Wine build tree):

    M=~/wine-lab/toolchains/llvm-mingw-20260421-ucrt-macos-universal/bin; B=~/wine-lab/build-wine11
    $M/x86_64-w64-mingw32-clang -shared -O2 -Wall -Wno-unused-function -o out/d3d12.dll \
        src/pe/madeira_d3d12.c src/pe/madeira_ext.c src/pe/d3d12.def -Isrc/pe -Isrc \
        -I~/wine-lab/dxmt-src/src/winemetal -L~/wine-lab/dxmt-src/build64/src/winemetal -l:winemetal.dll.a \
        $B/libs/winecrt0/x86_64-windows/libwinecrt0.a $B/dlls/ntdll/x86_64-windows/libntdll.a -luuid -lole32
    $B/tools/winebuild/winebuild --builtin out/d3d12.dll

Unix side:

    clang -arch x86_64 -O2 -Wall -fPIC -dynamiclib -o out/d3d12.so src/unix/d3d12_unix.m -Isrc \
        -I~/wine-lab/dxmt-src/src/winemetal -Wl,-install_name,@rpath/d3d12.so -framework Foundation -framework Metal

Install: d3d12.dll -> wine11/lib/wine/x86_64-windows/ (replaces Wine's vkd3d d3d12, which needs Vulkan this build
lacks; Wine picks builtins by export name, so a copy in system32 alone is not used), d3d12.so -> wine11/lib/wine/x86_64-unix/.

Config: madeira.cfg keys are read from the environment, e.g. fence-chain -> MADEIRA_FENCE_CHAIN.
