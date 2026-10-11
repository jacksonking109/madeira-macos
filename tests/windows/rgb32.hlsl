// Shader for tests/windows/rgb32_test.c, precompiled to DXIL in rgb32_dxil.h.
//
// Regenerate rgb32_dxil.h (zsh, from tests/windows, after `. ~/wine-lab/env11.sh`):
//   wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe -T cs_6_0 -E cs_main -Vn rgb_cs_dxil -Fh /tmp/rgb_cs.h rgb32.hlsl
//   sed '/^#if 0/,/^#endif/d' /tmp/rgb_cs.h | tr -d '\r' | sed '/^$/d'
// and replace the array in rgb32_dxil.h with that output (the sed drops dxc's
// disassembly listing).
//
// Reads element t of one typed buffer view, as TLOU1's mesh ray caster reads its
// vertex positions: Buffer<float4> (or uint4 / int4) from an unbounded
// descriptor array, over an R32G32B32 view, from element `base` on. D3D
// returns (x, y, z, 1). The four raw dwords go to the output.

cbuffer Sel : register(b0) { uint n; uint sel; uint kind; uint base; };
Buffer<float4> fa[] : register(t0, space0);
Buffer<uint4>  ua[] : register(t0, space1);
Buffer<int4>   ia[] : register(t0, space2);
RWStructuredBuffer<uint4> o : register(u0);

[numthreads(64, 1, 1)]
void cs_main(uint t : SV_DispatchThreadID)
{
    if (t >= n) return;
    uint4 v;
    if (kind == 0)      v = asuint(fa[sel][base + t]);
    else if (kind == 1) v = ua[sel][base + t];
    else                v = asuint(ia[sel][base + t]);
    o[t] = v;
}
