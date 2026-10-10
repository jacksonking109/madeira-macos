// Shaders for tests/windows/bindless_test.c, precompiled to DXIL in bindless_dxil.h.
//
// Regenerate bindless_dxil.h (zsh, from tests/windows, after `. ~/wine-lab/env11.sh`):
//   dxc() { wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe "$@"; }
//   dxc -T vs_6_0 -E vs_main  -Vn bl_vs_dxil       -Fh /tmp/bl_vs.h       bindless.hlsl
//   dxc -T ps_6_6 -E ps_heap  -Vn bl_ps_heap_dxil  -Fh /tmp/bl_ps_heap.h  bindless.hlsl
//   dxc -T ps_6_0 -E ps_array -Vn bl_ps_array_dxil -Fh /tmp/bl_ps_array.h bindless.hlsl
//   for f in /tmp/bl_{vs,ps_heap,ps_array}.h; do sed '/^#if 0/,/^#endif/d' $f | tr -d '\r' | sed '/^$/d'; echo; done
// and replace the arrays in bindless_dxil.h with that output (the sed drops dxc's
// disassembly listing).
//
// Every texture the tests sample is 2x1 (left texel A, right texel B) and every
// sample is taken at u = 1.25, outside the texture: a CLAMP sampler returns B,
// a WRAP sampler returns A. So the colour read back names both the texture and
// the sampler the shader reached.

struct V { float4 pos : SV_Position; };

// Fullscreen triangle from SV_VertexID: no vertex buffer, no input layout.
V vs_main(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    V o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

static const float2 OUTSIDE = float2(1.25, 0.5);

// (a) SM 6.6 dynamic resources: both heaps indexed directly, the indices from
// root constants. Needs CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED and
// SAMPLER_HEAP_DIRECTLY_INDEXED on the root signature. The heaps only exist
// from shader model 6.6, so the other targets do not see this entry point.
#if __SHADER_TARGET_MAJOR == 6 && __SHADER_TARGET_MINOR >= 6
cbuffer HeapIdx : register(b0) { uint tex_index; uint smp_index; };

float4 ps_heap(V i) : SV_Target
{
    Texture2D<float4> t = ResourceDescriptorHeap[tex_index];
    SamplerState s = SamplerDescriptorHeap[smp_index];
    return t.SampleLevel(s, OUTSIDE, 0);
}
#endif

// (b) SM 6.0 bounded descriptor-table array in a non-zero space, indexed
// dynamically, next to a single texture at the same register in space 0.
cbuffer ArrIdx : register(b1) { uint arr_index; };
Texture2D<float4> texs[4] : register(t0, space1);
Texture2D<float4> plain   : register(t0);
SamplerState      clamp_s : register(s0);

float4 ps_array(V i) : SV_Target
{
    float4 a = texs[arr_index].SampleLevel(clamp_s, OUTSIDE, 0);
    float4 b = plain.SampleLevel(clamp_s, OUTSIDE, 0);
    return float4(a.rg, b.b, 1.0);
}
