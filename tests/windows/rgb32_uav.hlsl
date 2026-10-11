// Third shader for tests/windows/rgb32_test.c, in rgb32_dxil.h as rgb_uav_dxil:
// typed UAV writes, an atomic, GetDimensions and a typed UAV load next to an
// RGB32 SRV read in one kernel. The UAV accesses must be left alone by the RGB32
// rewrite (D3D12 has no RGB32 UAVs) and must not count against it.
//   wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe -T cs_6_0 -E cs_main -Vn rgb_uav_dxil -Fh /tmp/rgb_uav.h rgb32_uav.hlsl
// (same sed as rgb32.hlsl's note).

cbuffer Sel : register(b0) { uint n; uint sel; uint kind; uint base; };
Buffer<float4> fa[] : register(t0, space0);
RWBuffer<float4> ruav : register(u1);
RWBuffer<uint> rcnt : register(u2);
RWStructuredBuffer<uint4> o : register(u0);

[numthreads(64, 1, 1)]
void cs_main(uint t : SV_DispatchThreadID)
{
    if (t >= n) return;
    float4 v = fa[sel][base + t];
    ruav[t] = v * 2.0f;
    uint old; InterlockedAdd(rcnt[0], 1, old);
    uint w; ruav.GetDimensions(w);
    float4 back = ruav[t];
    o[t] = asuint(back);
    o[n + t] = uint4(w, old < n ? 1u : 0u, 0, 0);
}
