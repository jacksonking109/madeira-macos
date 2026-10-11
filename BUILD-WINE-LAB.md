# Building the wine-lab port (x86_64 Wine 11.18 + DXMT v0.80, macOS)

PE side (needs llvm-mingw, DXMT's winemetal import lib, Wine's winecrt0/ntdll from the Wine build tree):

    M=~/wine-lab/toolchains/llvm-mingw-20260421-ucrt-macos-universal/bin; B=~/wine-lab/build-wine11
    $M/x86_64-w64-mingw32-clang -shared -O2 -Wall -Wno-unused-function -o out/d3d12.dll \
        src/pe/madeira_d3d12.c src/pe/madeira_ext.c src/pe/d3d12.def -Isrc/pe -Isrc \
        -I~/wine-lab/dxmt-src/src/winemetal -L~/wine-lab/dxmt-src/build64/src/winemetal -l:winemetal.dll.a \
        $B/libs/winecrt0/x86_64-windows/libwinecrt0.a $B/dlls/ntdll/x86_64-windows/libntdll.a -luuid -lole32
    $B/tools/winebuild/winebuild --builtin out/d3d12.dll

Unix side (DXIL via Apple's Metal Shader Converter; shader-model 5.x DXBC via DXMT's airconv, linked in statically
with LLVM 15, which also runs the DXIL rewrites of src/unix/madeira_dxil_rewrite.cpp (64-bit atomics, loop watchdog);
AGS rewriting stays compiled out). Needs dxmt-src on branch wine-lab-local (or d3d12-airconv), which adds the airconv APIs this runtime
uses (SM50GetRangeInfo, SM50_SHADER_PSO_VERTEX_INTERFACE) and SM 5.1 constant-buffer operands, built by
~/wine-lab/build-dxmt.sh into dxmt-src/build-full (libairconv.a, libDXBCParserNative.a):

    DX=~/wine-lab/dxmt-src; LL=~/wine-lab/deps/llvm15-x86_64
    D=(-DMADEIRA_WINE_MACOS -DMADEIRA_NO_AGS)
    I=(-Isrc -Isrc/unix -I$DX/src/winemetal -I$DX/src/airconv -Ithird_party/metal-shader-converter/include)
    IA=(-Isrc -I$DX/src/airconv -I$DX/libs -I$DX/include/native/windows -I$DX/include/native/directx)
    clang -arch x86_64 -O2 -fPIC -c src/unix/d3d12_unix.m -o out/d3d12_unix.o $D $I
    clang++ -arch x86_64 -O2 -fPIC -std=c++17 -x objective-c++ -c src/unix/madeira_ir_unix.mm -o out/madeira_ir_unix.o $D $I
    clang++ -arch x86_64 -O2 -fPIC -std=c++20 -Wno-extern-c-compat -Wno-unknown-pragmas \
        -c src/unix/madeira_sm5_ia.cpp -o out/madeira_sm5_ia.o $D $IA
    clang++ -arch x86_64 -O2 -fPIC -std=c++17 -fno-rtti -c src/unix/madeira_dxil_rewrite.cpp -o out/madeira_dxil_rewrite.o \
        -I$LL/include
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
        out/madeira_dxil_rewrite.o $AIRCONV $EXP -Wl,-install_name,@rpath/d3d12.so -framework Foundation -framework Metal

(zsh: the flag lists are arrays; `${^LLVM_LIBS}` expands one path per component. LLVM's static libraries are full
paths on purpose: `-L$LL/lib` would also make the linker pick LLVM's own libc++.dylib.) To build without LLVM, add
-DMADEIRA_NO_AIRCONV to D, drop madeira_sm5_ia.o, madeira_dxil_rewrite.o, $AIRCONV and $EXP; DXBC shaders are then
refused, and so are DXIL 64-bit atomics whose old value is read; loops are not guarded.

64-bit atomics whose old value is read: the converter refuses them (code 7); madeira_dxil_rewrite.cpp reads the old value
with a plain load (and a device memory fence for textures and typed buffers) before the atomic, whose result is then
unused. Not atomic with the update: an invocation between the load and the atomic is missed. madeira.cfg
dxil-atomic64-emulation = 0 turns it off. Its output is keyed in the DXIL cache by MAD_A64_VERSION (madeira_ir_unix.mm),
which a change to madeira_dxil_rewrite.cpp must bump (that file is not part of the build stamp). Host test, the three saved
TLOU Part I shaders included: see the header of tests/native/atomic64_rewrite_test.cpp.

Loop watchdog: Metal never times out a command buffer, so a compute kernel stuck in a loop holds the GPU until the
WindowServer watchdog reboots the Mac (TLOU Part I's CS_ComputeVolProbeBufferWriteCache BVH walk). Every natural loop of
a compute shader gets a pass counter at its header; at loop-watchdog-cap passes per loop entry (default 1048576) the
loop leaves through one of its own exits, an exiting branch that every iteration passes, whose condition is forced
(no new edges, so the IR stays valid; a phi, an add, a compare and an or/and per pass). Loops without such a branch
are left alone and logged. madeira.cfg loop-watchdog = 0 turns it off, loop-watchdog-ps = 1 also guards pixel shaders.
MAD_LWD_VERSION and the cap are keyed in the DXIL cache of the shaders it guards. The rewrite costs ~0.3-1 ms per
compute shader (parse + write, Rosetta), only on a cache miss. Host test, conversion only, with the captured TLOU
kernels: see the header of tests/native/loop_watchdog_test.cpp.

libmetalirconverter.dylib (MSC 4.0 beta 2, x86_64 slice) goes next to d3d12.so, or set MADEIRA_MSC_DYLIB.

Install: d3d12.dll -> wine11/lib/wine/x86_64-windows/ (replaces Wine's vkd3d d3d12, which needs Vulkan this build
lacks; Wine picks builtins by export name, so a copy in system32 alone is not used), d3d12.so -> wine11/lib/wine/x86_64-unix/,
then re-sign it ad hoc: `codesign --force -s - ~/wine-lab/wine11/lib/wine/x86_64-unix/d3d12.so`. While a game may be
running, copy each file to a temporary name in the same directory (sign the .so there) and rename it over the
installed one, so the running process keeps its mapped copy intact.

Tests (from out/, after `. ~/wine-lab/env11.sh`): d3d12-m2-x64.exe (expect 2 known failures: the stale fence-cycles
check, and "unclaimed feature level 12_1" since madeira.cfg claims 12_1); d3d12-dxbc-x64.exe (SM 5.0 DXBC through
airconv: root constants/CBV, SRV table + static sampler, vertex buffer input layout;
plus tier-0 render passes, GetCachedBlob/CachedPSO, the pipeline library, and a bound render target the pipeline does
not write keeping its contents); d3d12-bindless-x64.exe (tests/windows/bindless_test.c, 0 failures: SM 6.6
ResourceDescriptorHeap/SamplerDescriptorHeap, an SM 6.0 descriptor array in space 1, SM 5.1 DXBC registers in spaces 1
and 2, root signature 1.2, SM 5.1 DXBC constant buffers from root constants, a root CBV and a table in spaces 0-2, and
the refusal of an SM 5.1 constant-buffer array, an unbounded SRV range starting above register 0, and a pipeline
with pRootSignature = NULL using the root signature embedded in its shader; 59 checks; precompiled DXIL from
bindless.hlsl, regeneration commands at its top);
d3d12-atomic64-x64.exe (tests/windows/atomic64_test.c, 0 failures: 64-bit InterlockedMax with the old value used on a
RWTexture2D, raw, structured and typed buffers, exact per pixel; other texture shapes convert; a texture
compare-exchange with the old value used is refused; precompiled DXIL from atomic64.hlsl); d3d12-uavclear-x64.exe
(tests/windows/uavclear_test.c, 0 failures: texture UAV clears read back per texel, Uint clears of R16_FLOAT,
R16_TYPELESS-as-R16_FLOAT and R32_FLOAT views write the raw bits, Float clears of R16_UINT and R32_SINT views convert,
a descriptor holding another resource's view clears the named resource in its own format, and buffer UAV clears: typed
views (R32G32B32_UINT dispatch arguments, RGBA32, RG16, RG32 by a Float clear) element by element in their format over
their own range, raw views Values[0] per dword; 17 checks);
d3d12-occlusion-x64.exe (tests/windows/occlusion_test.c, 0 failures, 15 checks: occlusion queries resolved into a
DEFAULT-heap buffer are written on the GPU (a query inside one render pass gets its sample count, one spanning passes
reads visible), a draw with an empty scissor counts no samples, and the readback-buffer path still gives exact counts;
precompiled DXIL from occlusion.hlsl);
d3d12-readback-x64.exe (tests/windows/readback_test.c, 0 failures, 15 checks: a dispatch + copy into a READBACK buffer is
read exactly the moment its fence passes, 150 iterations each of one list, lists batched together, the copy on a copy
queue behind Wait, a compute queue, and frames in flight in a 16 MB readback buffer; 64-bit InterlockedMin without the
old value is exact on raw, structured and 2 MB typed R32G32_UINT buffers; groupshared 64-bit atomics only while
D3D12_OPTIONS9 claims them, which it no longer does by default; precompiled DXIL from readback.hlsl);
d3d12-loopwd-x64.exe (tests/windows/loopwd_test.c, 0 failures, 11 checks: a compute kernel with a counted loop and a
data-dependent loop, bounds from a buffer, exact for normal bounds and stopped at the default cap for 1 << 24 passes;
always finite, 0.5 s on an M3 Pro with the watchdog off; precompiled DXIL from loopwd.hlsl).
The tests build with `$M/x86_64-w64-mingw32-clang -O2 -Wall -o out/<exe> tests/windows/<file>.c -Itests/windows -luuid -lole32` (the cube demo, cube_window.c, adds -mwindows).

Config: madeira.cfg keys are read from the environment, e.g. fence-chain -> MADEIRA_FENCE_CHAIN.
Readback diagnostics: `debug-readback = 1` puts every READBACK buffer on our own storage and makes the pages each GPU
copy into one writes PAGE_NOACCESS until the CPU next touches them; a touch before that copy's command buffer has
completed is logged once per buffer ("debug-readback: EARLY read ..." with the command buffer's status and the reading
instruction's exe offset), on-time touches are counted in the 5 s report. It never waits and never changes data.
`dump-dxil = <entry names, comma list, or *>` writes those shaders' DXIL to C:\madeira-cs\<stage>_<entry>_<bytes>.dxil
of the running prefix, once per entry and size (disassemble with `wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe -dumpbin`).
`d3d12-atomic64-groupshared = 1` claims D3D12_OPTIONS9 AtomicInt64OnGroupShared again (Apple's compiler cannot build
such a kernel here).

Geometry/tessellation emulation (DXIL GS, DXIL HS+DS): d3d12.so builds the converter's object/mesh pipeline
(src/unix/d3d12_unix.m, new_geometry_emulation_pipeline_state). Host test, real converter VS/PS with stand-in
GS/HS/DS (no DXIL for those in this tree): see the header of tests/native/geom_emu_test.m. Known gap: DXIL
tessellation draws need 15360 bytes of object threadgroup memory, which madeira_d3d12.c asks for through
reserved words of the DrawMeshThreadgroups command that only Madeira's DXMT fork reads; stock DXMT v0.80 ignores them.
