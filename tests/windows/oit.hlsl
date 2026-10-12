// Shaders for tests/windows/oit_test.c, precompiled to DXIL in oit_dxil.h.
//
// Regenerate oit_dxil.h (zsh, from tests/windows, after `. ~/wine-lab/env11.sh`; T = a scratch dir):
//   dxc() { wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe "$@"; }
//   dxc -T vs_6_0 -E vs_main -Vn oit_vs_dxil -Fh $T/oit_vs.h oit.hlsl
//   dxc -T ps_6_6 -E ps_insert -Vn oit_ps_dxil -Fh $T/oit_ps.h oit.hlsl
//   for e in vs ps; do sed '/^#if 0/,/^#endif/d' $T/oit_$e.h | tr -d '\r' | sed '/^$/d'; echo; done
// and replace the arrays in oit_dxil.h with that output.
//
// The Last of Us Part I's order-independent transparency (MainPsOitAppend) as a
// contention test: a k-buffer of 4 RWTexture2D<uint64_t> layers, each fragment
// inserted with 64-bit InterlockedMax that returns the OLD value, the smaller of
// the two carried to the next layer, stopping on an empty (0) slot -- the same
// shape as the game's DXIL (atomicBinOp UMax, then UMin with the carried value).
// Every instance is a fullscreen triangle, so every pixel gets NINST fragments
// that race on its four slots. No loops: the insert is unrolled.

struct V
{
    float4 pos : SV_Position;
    nointerpolation uint inst : INST;
};

V vs_main(uint id : SV_VertexID, uint inst : SV_InstanceID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    V o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.inst = inst;
    return o;
}

#define W 32

// The test recomputes this on the CPU: hi = a nonzero sort key, lo = who wrote it.
uint key_hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x | 1u;
}

uint64_t frag_value(uint inst, uint2 p)
{
    uint hi = key_hash(inst * 4096u + p.y * W + p.x);
    uint lo = (inst << 20) | (p.y << 10) | p.x;
    return ((uint64_t)hi << 32) | lo;
}

#if __SHADER_TARGET_MAJOR == 6 && __SHADER_TARGET_MINOR >= 6
RWTexture2D<uint64_t> k0 : register(u0);
RWTexture2D<uint64_t> k1 : register(u1);
RWTexture2D<uint64_t> k2 : register(u2);
RWTexture2D<uint64_t> k3 : register(u3);

uint ps_insert(V i) : SV_Target
{
    uint2 p = uint2(i.pos.xy);
    uint64_t v = frag_value(i.inst, p);
    uint64_t o;
    InterlockedMax(k0[p], v, o); v = min(o, v);
    if (v != 0) {
        InterlockedMax(k1[p], v, o); v = min(o, v);
        if (v != 0) {
            InterlockedMax(k2[p], v, o); v = min(o, v);
            if (v != 0)
                InterlockedMax(k3[p], v);
        }
    }
    return i.inst;
}
#endif
