// Shaders for tests/windows/atomic64_test.c, precompiled to DXIL in atomic64_dxil.h.
//
// Regenerate atomic64_dxil.h (zsh, from tests/windows, after `. ~/wine-lab/env11.sh`):
//   dxc() { wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe "$@"; }
//   dxc -T vs_6_0 -E vs_main -Vn a64_vs_dxil -Fh /tmp/a64_vs_main.h atomic64.hlsl
//   for e in ps_tex ps_raw ps_struct ps_typed ps_tex3d ps_tex2darray ps_tex1darray ps_cmpxchg; do
//     dxc -T ps_6_6 -E $e -Vn a64_${e}_dxil -Fh /tmp/a64_$e.h atomic64.hlsl; done
//   for e in vs_main ps_tex ps_raw ps_struct ps_typed ps_tex3d ps_tex2darray ps_tex1darray ps_cmpxchg; do
//     sed '/^#if 0/,/^#endif/d' /tmp/a64_$e.h | tr -d '\r' | sed '/^$/d'; echo; done
// and replace the arrays in atomic64_dxil.h with that output (the sed drops dxc's
// disassembly listing).
//
// Every pixel shader does one 64-bit InterlockedMax (or compare-exchange) on
// the element its pixel owns and returns the OLD value as uint2(lo, hi) to an
// R32G32_UINT target. A fullscreen triangle gives each pixel exactly one
// fragment, so both the returned value and the element left behind are exact.
// The Metal Shader Converter refuses every one of these as written (the old
// value is used); the runtime's rewrite (src/unix/madeira_atomic64.cpp) makes
// them convert, except ps_cmpxchg, which must still be refused.

struct V { float4 pos : SV_Position; };

// Fullscreen triangle from SV_VertexID: no vertex buffer, no input layout.
V vs_main(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    V o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

#define W 64

// What the test uploads into element (x, y): distinct high and low words.
uint64_t init_value(uint2 p)
{
    return ((uint64_t)(0x01000000u + (p.y << 12) + p.x) << 32) | (0x80000000u | (p.y << 8) | p.x);
}

// What the shader offers: a quarter of the pixels each above / below the
// initial value in the high word, and above / below it in the low word only.
uint64_t new_value(uint2 p)
{
    uint64_t i = init_value(p);
    uint k = (p.x + 2 * p.y) & 3;
    if (k == 0) return i + ((uint64_t)1 << 32);
    if (k == 1) return i - ((uint64_t)1 << 32);
    if (k == 2) return i + 3;
    return i - 3;
}

uint2 split(uint64_t v) { return uint2((uint)v, (uint)(v >> 32)); }

#if __SHADER_TARGET_MAJOR == 6 && __SHADER_TARGET_MINOR >= 6
RWTexture2D<uint64_t>        tex  : register(u0);
RWByteAddressBuffer          raw  : register(u1);
RWStructuredBuffer<uint64_t> sbuf : register(u2);
RWBuffer<uint64_t>           tbuf : register(u3);

uint2 ps_tex(V i) : SV_Target
{
    uint2 p = uint2(i.pos.xy);
    uint64_t o;
    InterlockedMax(tex[p], new_value(p), o);
    return split(o);
}

uint2 ps_raw(V i) : SV_Target
{
    uint2 p = uint2(i.pos.xy);
    uint64_t o;
    raw.InterlockedMax64((p.y * W + p.x) * 8, new_value(p), o);
    return split(o);
}

uint2 ps_struct(V i) : SV_Target
{
    uint2 p = uint2(i.pos.xy);
    uint64_t o;
    InterlockedMax(sbuf[p.y * W + p.x], new_value(p), o);
    return split(o);
}

uint2 ps_typed(V i) : SV_Target
{
    uint2 p = uint2(i.pos.xy);
    uint64_t o;
    InterlockedMax(tbuf[p.y * W + p.x], new_value(p), o);
    return split(o);
}

// Conversion only: the other texture shapes the rewrite reads back.
RWTexture3D<uint64_t>        tex3d  : register(u4);
RWTexture2DArray<uint64_t>   tex2da : register(u5);
RWTexture1DArray<uint64_t>   tex1da : register(u6);

uint2 ps_tex3d(V i) : SV_Target
{
    uint2 p = uint2(i.pos.xy);
    uint64_t o;
    InterlockedMax(tex3d[uint3(p, p.x & 3)], new_value(p), o);
    return split(o);
}

uint2 ps_tex2darray(V i) : SV_Target
{
    uint2 p = uint2(i.pos.xy);
    uint64_t o;
    InterlockedMax(tex2da[uint3(p, 1)], new_value(p), o);
    return split(o);
}

uint2 ps_tex1darray(V i) : SV_Target
{
    uint2 p = uint2(i.pos.xy);
    uint64_t o;
    InterlockedMax(tex1da[p], new_value(p), o);
    return split(o);
}

// A 64-bit compare-exchange whose old value is used: a load cannot stand in
// for it, so the rewrite leaves it and the pipeline is refused.
uint2 ps_cmpxchg(V i) : SV_Target
{
    uint2 p = uint2(i.pos.xy);
    uint64_t o;
    InterlockedCompareExchange(tex[p], init_value(p), new_value(p), o);
    return split(o);
}
#endif
