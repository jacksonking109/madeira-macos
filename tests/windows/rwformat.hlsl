// Shaders for tests/windows/rwformat_test.c, precompiled to DXIL in rwformat_dxil.h.
//
// One compile per variant (-D selects the resource declarations, so each DXIL blob
// holds exactly one RW/SRV pair), several entry points each. Regenerate
// rwformat_dxil.h (zsh, from tests/windows, after `. ~/wine-lab/env11.sh`):
//   dxc() { wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe "$@"; }
//   for v in A_S A_U; do for e in wr rot w srv; do
//     dxc -T cs_6_0 -E $e -D$v -Vn rwf_${v}_${e}_dxil -Fh /tmp/rwf_${v}_$e.h rwformat.hlsl; done; done
//   for v in B_R11 B_F16; do for e in init add srv_add; do
//     dxc -T cs_6_0 -E $e -D$v -Vn rwf_${v}_${e}_dxil -Fh /tmp/rwf_${v}_$e.h rwformat.hlsl; done; done
//   for f in /tmp/rwf_*.h; do sed '/^#if 0/,/^#endif/d' $f | tr -d '\r' | sed '/^$/d'; echo; done
// and replace the arrays in rwformat_dxil.h with that output (the sed drops dxc's
// disassembly listing).
//
// Variants:
//   A_S  256x16x2 3D texture, R8G8B8A8_SNORM   (RWTexture3D<snorm float4>; NOT a Metal read_write tier-2 format)
//   A_U  same, R8G8B8A8_UNORM                  (control: tier-2 format)
//   B_R11 960x600 2D, R11G11B10_FLOAT          (RWTexture2D<float3>; NOT tier-2 read_write)
//   B_F16 960x600 2D, R16G16B16A16_FLOAT       (control, float4; w carried along untouched)
//
// Root signature: root constants b0 (4 dwords), a descriptor table holding the UAV
// u0 and one holding the SRV t0.
#define RS_RW "RootConstants(num32BitConstants=4, b0), DescriptorTable(UAV(u0)), DescriptorTable(SRV(t0))"
cbuffer C : register(b0) { uint seed; uint d0; uint d1; uint d2; };

uint hsh(uint a, uint b) {
    uint h = a * 0x9e3779b1u ^ b * 0x85ebca6bu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

#if defined(A_S) || defined(A_U)
// ---- A: 3D, 4 x 8-bit. Per-texel channel codes from a hash; a stored value is
// code/127 (snorm) or code/255 (unorm), which the format stores back as exactly code.
#if defined(A_S)
  #define TEX_T snorm float4
  float code2f(uint h) { return (float)((int)(h % 255u) - 127) / 127.0f; }
#else
  #define TEX_T unorm float4
  float code2f(uint h) { return (float)(h & 255u) / 255.0f; }
#endif
RWTexture3D<TEX_T> tex : register(u0);
Texture3D<TEX_T> stex : register(t0);

float4 texel_a(uint3 p) {
    uint lin = p.x + 256u * (p.y + 16u * p.z);
    return float4(code2f(hsh(seed, lin * 4u + 0u)), code2f(hsh(seed, lin * 4u + 1u)),
                  code2f(hsh(seed, lin * 4u + 2u)), code2f(hsh(seed, lin * 4u + 3u)));
}

// write A, read it back in the SAME thread through the UAV, store it rotated: yzwx
[RootSignature(RS_RW)] [numthreads(8, 8, 1)]
void wr(uint3 id : SV_DispatchThreadID) {
    tex[id] = texel_a(id);
    float4 r = tex[id];
    tex[id] = r.yzwx;
}
// a later dispatch: typed UAV load of what is there, store it rotated again
[RootSignature(RS_RW)] [numthreads(8, 8, 1)]
void rot(uint3 id : SV_DispatchThreadID) {
    float4 v = tex[id];
    tex[id] = v.yzwx;
}
// plain write of A (the SRV comparison path)
[RootSignature(RS_RW)] [numthreads(8, 8, 1)]
void w(uint3 id : SV_DispatchThreadID) { tex[id] = texel_a(id); }
// read the other texture through an SRV, store it rotated
[RootSignature(RS_RW)] [numthreads(8, 8, 1)]
void srv(uint3 id : SV_DispatchThreadID) {
    float4 v = stex.Load(int4(id, 0));
    tex[id] = v.yzwx;
}
#endif

#if defined(B_R11) || defined(B_F16)
// ---- B: 2D 960x600. Integer-valued texels (initial 0..7, deltas 1..3, <= 8 adds:
// max 31, exact in 11/11/10-bit floats and in half).
#if defined(B_R11)
  #define VEC float3
  #define MKV(a, b, c, d) float3(a, b, c)
#else
  #define VEC float4
  #define MKV(a, b, c, d) float4(a, b, c, d)
#endif
RWTexture2D<VEC> tex : register(u0);
Texture2D<VEC> stex : register(t0);

[RootSignature(RS_RW)] [numthreads(8, 8, 1)]
void init(uint3 id : SV_DispatchThreadID) {
    uint lin = id.x + 960u * id.y;
    tex[id.xy] = MKV((float)(hsh(seed, lin * 4u + 0u) & 7u), (float)(hsh(seed, lin * 4u + 1u) & 7u),
                     (float)(hsh(seed, lin * 4u + 2u) & 7u), (float)(hsh(seed, lin * 4u + 3u) & 7u));
}
// read-modify-write in place through the UAV
[RootSignature(RS_RW)] [numthreads(8, 8, 1)]
void add(uint3 id : SV_DispatchThreadID) {
    VEC v = tex[id.xy];
    tex[id.xy] = v + MKV((float)d0, (float)d1, (float)d2, 0.0f);
}
// the same add, read through an SRV from the other texture, stored to this one
[RootSignature(RS_RW)] [numthreads(8, 8, 1)]
void srv_add(uint3 id : SV_DispatchThreadID) {
    VEC v = stex.Load(int3(id.xy, 0));
    tex[id.xy] = v + MKV((float)d0, (float)d1, (float)d2, 0.0f);
}
#endif
