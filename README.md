# madeira-d3d12 for macOS (Wine)

Direct3D 12 on Metal for Windows games running in Wine on macOS: a port of the D3D12 layer from
[Madeira](https://github.com/willfaust/Madeira) (Will Faust and contributors), which runs Windows games on
iPhone. This port targets x86_64 Wine under Rosetta 2 with [DXMT](https://github.com/3Shain/dxmt) on Apple Silicon.

Hobby project, early stage. Madeira's original README is in [README.madeira.md](README.madeira.md).

## Status

- Madeira's API test passes 89/90 (the one failure is a stale synchronous-fence expectation); its D3D12 cube renders.
- Shaders: DXIL through Apple's Metal Shader Converter; DXBC (shader model 5.x) through DXMT's airconv, linked
  into `d3d12.so` (needs dxmt-src's `d3d12-airconv` APIs). The DXBC test (root constants/CBV, SRV table + static
  sampler, vertex-buffer input layout) passes; real games' DXBC is still largely untried.
- Geometry/tessellation emulation pipelines build but are untested with real shaders.

## How it differs from Madeira

- `d3d12.dll` is built for x86_64, not ARM64EC.
- The Metal calls Madeira added to its own DXMT fork (placement heaps, residency sets, vertex-descriptor and
  geometry-emulation pipelines) are served by this DLL's own unix library, `d3d12.so`, so stock DXMT v0.80 works
  unchanged. See [src/madeira_ext.h](src/madeira_ext.h).
- The converter is loaded from next to `d3d12.so` (or `$MADEIRA_MSC_DYLIB`) instead of an iOS app bundle.
- Settings come from `madeira.cfg` in `$MADEIRA_DOCS_DIR`, with `MADEIRA_<KEY>` environment overrides.
  Additions: `d3d12-max-feature-level` (0xc100 claims 12_1) and `trace-features` (logs CheckFeatureSupport).
- Port changes are marked `wine-lab port:` in the source.

## Requirements

- Wine with DXMT support on current Mac drivers: [jacksonking109/wine `dxmt-macdrv`](https://github.com/jacksonking109/wine/tree/dxmt-macdrv)
- DXMT v0.80 (D3D11/DXGI/winemetal), optionally [jacksonking109/dxmt `cross-process-swapchain`](https://github.com/jacksonking109/dxmt/tree/cross-process-swapchain)
- Apple's Metal Shader Converter 4.0 beta 2 (`libmetalirconverter.dylib`, universal); download it from Apple,
  it is not included here.

## Building and installing

See [BUILD-WINE-LAB.md](BUILD-WINE-LAB.md). In short: build `d3d12.dll` with llvm-mingw and mark it as a Wine
builtin, build `d3d12.so` with Apple clang, and install both into Wine's `lib/wine` directories (Wine picks builtin
DLLs by export name, so this replaces Wine's own `d3d12.dll`).

## Human Comments

Do not take this as some sort of poke at crossover or other vibecoded translation layers.  This was more of a learning experience with a bit of fun added in for myself.  Have fun with it as it does work fairly well for being a Rosetta/DXMT/Wine whatever with a bit of Madeira in the mix.  This does have a one off DXMT port in order to handoff windows and proton based work correctly without containerization.  No I will not be doing the 50th crossover clone or FEX port or any of that.  Also, if you want to send in game bugs and whatnot I cannot promise I will work on them but I probably will. Seperating a bunch of the debuggers I made and publishing them seperately later. -JK

## License

GPL-3.0-or-later with the Madeira Converter Exception ([LICENSE](LICENSE), [LICENSE-EXCEPTION.md](LICENSE-EXCEPTION.md)),
as in Madeira. Parts of `src/unix/d3d12_unix.m` follow DXMT's winemetal (MIT, Feifan He). Apple's Metal Shader
Converter headers in `third_party/` are Apple's.
