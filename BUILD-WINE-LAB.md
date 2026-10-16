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
    clang++ -arch x86_64 -O2 -fPIC -std=c++17 -fno-rtti -c src/unix/madeira_air_rewrite.cpp -o out/madeira_air_rewrite.o \
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
        out/madeira_dxil_rewrite.o out/madeira_air_rewrite.o $AIRCONV $EXP -Wl,-install_name,@rpath/d3d12.so \
        -framework Foundation -framework Metal

(zsh: the flag lists are arrays; `${^LLVM_LIBS}` expands one path per component. LLVM's static libraries are full
paths on purpose: `-L$LL/lib` would also make the linker pick LLVM's own libc++.dylib.) To build without LLVM, add
-DMADEIRA_NO_AIRCONV to D, drop madeira_sm5_ia.o, madeira_dxil_rewrite.o, madeira_air_rewrite.o, $AIRCONV and $EXP; DXBC
shaders are then refused, and so are DXIL 64-bit atomics whose old value is read; loops are not guarded; RGB32 typed
buffers fall back to shadow copies.

64-bit atomics whose old value is read: the converter refuses them (code 7); madeira_dxil_rewrite.cpp reads the old value
with a plain load (and a device memory fence for textures and typed buffers) before the atomic, whose result is then
unused. Not atomic with the update: an invocation between the load and the atomic is missed. madeira.cfg
dxil-atomic64-emulation = 0 turns it off. Its output is keyed in the DXIL cache by MAD_A64_VERSION (madeira_ir_unix.mm),
which a change to madeira_dxil_rewrite.cpp must bump (that file is not part of the build stamp). Host test, the three saved
TLOU Part I shaders included: see the header of tests/native/atomic64_rewrite_test.cpp.

RGB32 typed buffers (madeira.cfg rgb32-shader, default 1): Metal has no 3-channel 32-bit texture-buffer format. A
typed SRV of R32G32B32_{FLOAT,UINT,SINT,TYPELESS} is bound as an R32 texture buffer of 3 x NumElements over the same
memory with bit 62 set in the descriptor's metadata (bits MSC never reads). src/unix/madeira_air_rewrite.cpp rewrites
the converter's OUTPUT (AIR, LLVM 15, after MSC): every typed-buffer read whose texture is loaded from a descriptor's
textureViewID field (argument buffer, descriptor table, or the SM 6.6 resource heap at 24 x n + 8) tests that bit and
then loads x, y, z from the descriptor's gpuVA at element x 12 (w = 1 or 1.0f, 0 out of range); GetDimensions returns
size / 12; a texture chosen through a phi or select of such fields is followed. Typed UAV accesses (a write, an
atomic, a fence, or a read with read_write access on the texture) are left alone and never counted: D3D12 has no RGB32
UAVs. The metallib is repacked (HASH = SHA-256 of the bitcode, MDSZ, header sizes, RLST offset). A pipeline with typed
SRV reads the rewrite still cannot trace is logged and keeps that risk alone (such a read of an RGB32 view sees R32
data); rgb32-shader-strict = 1 instead makes every view created afterwards an RGBA32 shadow copy. SM 5.x (DXBC)
shaders reading such a view get a shadow made on first use. MAD_RGB32_VERSION (madeira_ir_unix.mm) keys the DXIL cache and must be bumped
with every change to madeira_air_rewrite.cpp. rgb32-shader = 0 restores the shadows for every view (refresh rules:
struct mad_shadow; rgb32-big-mb, rgb32-big-frames, rgb32-big-exact).

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
check, and "unclaimed feature level 12_1" since madeira.cfg claims 12_1); d3d12-dxbc-x64.exe (68 checks; SM 5.0 DXBC through
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
compare-exchange with the old value used is refused; 64 x 256 compute threads InterlockedMin (result unused) on 4
addresses of raw and structured buffers give consistent pairs and the unsigned 64-bit minimum, also for the
asuint(distance) | id packing with ties; 36 checks; precompiled DXIL from atomic64.hlsl); d3d12-uavclear-x64.exe
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
always finite, 0.5 s on an M3 Pro with the watchdog off; precompiled DXIL from loopwd.hlsl);
d3d12-rgb32-x64.exe (tests/windows/rgb32_test.c, 0 failures, 46 checks (44 with rgb32-shader = 0): R32G32B32_FLOAT/_UINT/_SINT typed SRVs, which
Metal has no texture-buffer format for, read as Buffer<float4/uint4/int4> from unbounded descriptor arrays, bit-exact
with w = 1: CopyBufferRegion + barrier (40000 elements), FirstElement 37, a second copy + barrier in the same list, a
view made after the data, a copy with no barrier read in the next list, an UPLOAD source the CPU rewrites, and an UPLOAD
ring of 3 per-frame slots rewritten every frame with 3 frames in flight, read through per-slot views and one whole-ring
view; refresh cost: 40 small copies into a 12 MB buffer with 81 views copy exactly the bytes they wrote that views
cover (MadeiraD3D12ShadowStats export), two UAV -> SRV barriers with no work between refresh every view once; a SINT
view shares a UINT view's shadow; a big (>= rgb32-big-mb, default 4) view is refreshed once per frame after shader writes
(every time with rgb32-big-exact = 1; rgb32-big-frames = 0, never, fails 3 checks by design); a genuine R32_FLOAT view
through the same read site, GetDimensions of both kinds, and SM 6.6 ResourceDescriptorHeap indexing; with
rgb32-shader = 1 (default) the run must make no shadow and copy nothing; typed UAV writes, an atomic and a typed UAV
load next to an RGB32 read in one kernel; precompiled DXIL from rgb32.hlsl, rgb32_heap.hlsl and rgb32_uav.hlsl);
d3d12-parallel-x64.exe (tests/windows/parallel_test.c, 0 failures, 32 checks: 8 rounds of ONE ExecuteCommandLists of 12
lists whose results depend on order across list boundaries -- a compute chain with a CPU reference, per-list snapshot
copies, order-dependent blending into targets shared by consecutive lists, occlusion queries resolved one and three
lists later; run it serially and with `MADEIRA_PARALLEL_ENCODE=4 MADEIRA_PARALLEL_MIN_CMDS=1`; fence-chain 3 fails it
serially too); d3d12-dsplane-x64.exe (tests/windows/dsplane_test.c, 0 failures, 19 checks: CopyTextureRegion of ONE
plane of a D32_FLOAT_S8X24_UINT texture -- texture to texture, upload buffer to plane, colour texture to plane and back --
leaves the other plane intact; both planes read back per texel after each copy. Needs d3d12.so's blitB2T unix call:
DXMT's winemetal drops the MTLBlitOption on buffer-to-texture copies, so a stencil-plane copy landed in the DEPTH plane;
and a clear followed by a barrier and a copy of its target runs before the copy); d3d12-oit-x64.exe (tests/windows/oit_test.c,
0 failures, 29 checks: The Last of Us Part I's OIT k-buffer insert -- 4 RWTexture2D<uint64_t> layers, InterlockedMax
returning the old value, the smaller carried down -- from 1 to 256 racing fullscreen instances, after stale data and the
zero clear in the same list, plus a 64-bit pattern clear of R32G32_UINT; fails on a stored value that is no input of
its pixel, unsorted layers or a wrong layer 0; prints, not fails, how many true top-4 values the load+atomic emulation
loses to the race; precompiled DXIL from oit.hlsl); d3d12-dssrv-x64.exe (tests/windows/dssrv_test.c, 0 failures, 58 checks,
HLSL compiled at run time to SM 5.0/5.1 DXBC: Rise of the Tomb Raider's DX12 lighting shapes -- an SV_Depth pattern and
a discard + stencil REPLACE edge mark read back through R24_UNORM_X8_TYPELESS (Load, Gather .w/.z, SampleLevel, Sample)
and X24_TYPELESS_G8_UINT (.y), a fullscreen pass masked by stencil NOT_EQUAL (read mask 2) and depth GREATER_EQUAL from
z = 1 under a viewport of MaxDepth 0.98, SampleCmp(LevelZero) on D16 and D32 maps through static and heap comparison
samplers, StructuredBuffer / ByteAddressBuffer / Buffer<uint4> with FirstElement / Texture2DArray reads as SM 5.0 and
5.1, a MAXIMUM-reduction sampler, and ClearDepthStencilView with RECTS: three tiles of a D16 atlas cleared one after
another keep their own depths, depth-only and stencil-only rect clears leave the other aspect and the rest alone, and SRVs with a component mapping on R8 and RGBA16F render targets; needs
d3d12.so's newClearPipeline unix call). Replay bench: `d3d12-bench-x64.exe draws frames draws-per-list dispatches-per-list`, e.g. `2400 100 150 12`
(16 lists, TLOU-like mix), prints ExecuteCommandLists wall time, submit-to-GPU-done latency and image/UAV hashes.
d3d12-regress-x64.exe (tests/windows/regress_test.c, 0 failures, 51 checks, also with ds-rect-inpass/ds-rect-defer/
store-defer = 0: the 2026-10-12/13 ROTTR fixes - buffer GetCopyableFootprints total = width at the base offset; NULL
texture and buffer SRVs read zeros; per-instance step rate 0 = element 0 for every instance (DXBC); rect depth clears
in an open depth-only pass and held for the next pass, ordered against draws, a full clear and a barrier; and the
2026-10-16 stub audit - SetName stored and read back through GetPrivateData on the main objects, GetHeapProperties,
PIX markers, SetPredication (ignored: predicated work runs) and OMSetDepthBounds accepted, CheckDriverMatchingIdentifier
answers UNRECOGNIZED). Stubs left by the audit, by reason: no capability claimed (raytracing, mesh shaders, VRS,
protected sessions, meta commands, sampler feedback, view instancing, programmable sample positions, depth bounds,
WriteBufferImmediate) or a real project (bundles, shared handles, stream output, tiled CopyTiles, AtomicCopyBuffer,
ResolveSubresourceRegion, ClearState, enhanced barriers, Write/ReadFromSubresource with UMA = FALSE). Case 6 runs two compute
pipelines across UAV barriers (A, B, A, B, ...) and checks every value B read; run it once with gpu-pass-profile = 1,
which splits compute encoders per pipeline for timing (40 -> 45 encoders, same results).

The tests build with `$M/x86_64-w64-mingw32-clang -O2 -Wall -o out/<exe> tests/windows/<file>.c -Itests/windows -luuid -lole32` (the cube demo, cube_window.c, adds -mwindows).

Config: madeira.cfg keys are read from the environment, e.g. fence-chain -> MADEIRA_FENCE_CHAIN.
Parallel encoding: `parallel-encode = N` (default 0, off; max 7) cuts one ExecuteCommandLists into up to N+1 runs of
consecutive lists (run 0 half-size, `parallel-min-cmds` commands per run, default 96), gives each run its own command
buffer, enqueues them all in order, encodes run 0 on the caller and the rest on N worker threads, and commits them in
order. Serial fallback: fence-chain 3/5, captures/dumps, debug-readback, a list twice in one call, new RGB32 shadows.
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
