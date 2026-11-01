# madeira-macos

Direct3D 12 on Metal for Windows games running in Wine on macOS: an **unofficial** port of the D3D12 layer from
[Madeira](https://github.com/willfaust/Madeira) (Will Faust and contributors), which runs Windows games on
iPhone. This port targets x86_64 Wine under Rosetta 2 with [DXMT](https://github.com/3Shain/dxmt) on Apple Silicon.
It is not affiliated with or endorsed by the Madeira project.

Hobby project. Madeira's original README is in [README.madeira.md](README.madeira.md).

## Status

Tested on an M3 Pro MacBook Pro (macOS 26), Wine 11.18 under Rosetta 2:

| Game (D3D12) | State |
|---|---|
| PEAK | Playable, ~60 fps |
| Rise of the Tomb Raider (DX12 mode) | Built-in benchmark runs, ~48 fps at 1920x1200 |
| The Last of Us Part I | Playable at ~18-22 fps (3024x1688 output, FSR 3.1 at 50%); GPU-bound |
| Red Dead Redemption 2 | Boots and plays its intro; benchmark ~24 fps at 1920x1200 |

- Shaders: DXIL through Apple's Metal Shader Converter; DXBC (shader model 5.x) through DXMT's airconv, linked
  into `d3d12.so` (needs dxmt-src's `d3d12-airconv` APIs).
- Tests: `tests/windows/` holds the D3D12 test programs (API coverage, bindless, UAV clears, readback,
  typed-buffer views, RW formats, parallel encoding and more); they run under Wine against the installed DLL.
- Known issue: in The Last of Us Part I, a single very long frame (shader compilation at startup, extremely fast
  camera turns) can still tint an object for a frame.

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
Converter headers in `third_party/` are Copyright Apple Inc. under the Apache License 2.0 (full text in each
header folder); Apple's converter library itself is not included and is under Apple's own terms.

**Modified version.** This is a modified version of Madeira's D3D12 runtime, changed by Jackson King from
October 2026 onward (port changes are marked `wine-lab port:` in the source; see the git history). The
modifications are released under the same licence, and the Madeira Converter Exception applies to them as well.
