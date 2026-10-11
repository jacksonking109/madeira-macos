// Second shader for tests/windows/rgb32_test.c, in rgb32_dxil.h as rgb_heap_dxil:
// the same reads through SM 6.6 ResourceDescriptorHeap indexing.
//   wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe -T cs_6_6 -E cs_main -Vn rgb_heap_dxil -Fh /tmp/rgb_heap.h rgb32_heap.hlsl
// (same sed as rgb32.hlsl's note).

cbuffer Sel : register(b0) { uint n; uint sel; uint kind; uint base; };
RWStructuredBuffer<uint4> o : register(u0);

[numthreads(64, 1, 1)]
void cs_main(uint t : SV_DispatchThreadID)
{
    if (t >= n) return;
    Buffer<float4> b = ResourceDescriptorHeap[sel];
    uint4 v;
    if (kind == 0) v = asuint(b[base + t]);
    else { uint w; b.GetDimensions(w); v = uint4(w, 0, 0, 0); }
    o[t] = v;
}
