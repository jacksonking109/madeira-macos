// Shaders for tests/windows/readback_test.c, precompiled to DXIL in readback_dxil.h.
//
// Regenerate readback_dxil.h (zsh, from tests/windows, after `. ~/wine-lab/env11.sh`):
//   dxc() { wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe "$@"; }
//   dxc -T cs_6_0 -E cs_fill -Vn rb_fill_dxil -Fh /tmp/rb_cs_fill.h readback.hlsl
//   for e in cs_min_raw cs_min_struct cs_min_typed cs_min_groupshared; do dxc -T cs_6_6 -E $e -Vn rb_${e#cs_}_dxil -Fh /tmp/rb_$e.h readback.hlsl; done
//   for e in cs_fill cs_min_raw cs_min_struct cs_min_typed cs_min_groupshared; do
//     sed '/^#if 0/,/^#endif/d' /tmp/rb_$e.h | tr -d '\r' | sed '/^$/d'; echo; done
// and replace the arrays in readback_dxil.h with that output (the sed drops dxc's
// disassembly listing).
//
// Every shader carries its root signature: root constants at b0, then a root
// UAV (cs_min_typed: a descriptor table holding one typed UAV).

// cs_fill: element i of u0 becomes fill_value(seed, i). Each thread spins a
// little first so the dispatch takes long enough on the GPU that a readback
// published before the GPU finished would see the previous iteration's data.
#define RS_FILL "RootConstants(num32BitConstants=4, b0), UAV(u0)"
cbuffer C : register(b0) { uint seed; uint count; uint spin; uint stride; };
RWByteAddressBuffer dst : register(u0);

uint fill_value(uint s, uint i) {
    uint h = s * 0x9e3779b1u ^ i * 0x85ebca6bu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
    return h | 1u;   // never 0, never the test's poison 0
}

[RootSignature(RS_FILL)]
[numthreads(64, 1, 1)]
void cs_fill(uint3 id : SV_DispatchThreadID) {
    uint i = id.x, v, k;
    if (i >= count) return;
    v = fill_value(seed, i);
    // spin rounds of an LCG step, then as many of its inverse (4276115653 =
    // 1664525^-1 mod 2^32): GPU time without a result the CPU has to replay
    for (k = 0; k < spin; k++) v = v * 1664525u + 1013904223u;
    for (k = 0; k < spin; k++) v = (v - 1013904223u) * 4276115653u;
    dst.Store(i * 4, v);
}

// cs_min_raw / cs_min_struct / cs_min_typed / cs_min_groupshared: SM 6.6 64-bit InterlockedMin without the old
// value, the closest-hit pattern of a GPU ray caster (TLOU Part I's
// Cs_CheckRayPolygonHit is cs_6_6): key = (asuint(t) << 32) | candidate, t >= 0
// so its bits order like the float. Thread i offers candidate i to slot
// i % slots. The CPU computes the expected minimum per slot; a min done as two
// 32-bit halves, or not atomically, leaves a t from one candidate with the
// index of another.
#define RS_MIN "RootConstants(num32BitConstants=4, b0), UAV(u0)"
uint64_t candidate_key(uint s, uint i) {
    uint h = s * 0x9e3779b1u ^ i * 0x85ebca6bu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
    float t = (float)(h >> 8) * (1.0f / 16777216.0f);   // [0, 1), exact
    return ((uint64_t)asuint(t) << 32) | (uint64_t)i;
}

RWStructuredBuffer<uint64_t> keys : register(u0);

[RootSignature(RS_MIN)]
[numthreads(64, 1, 1)]
void cs_min_raw(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    dst.InterlockedMin64((id.x % spin) * 8, candidate_key(seed, id.x));
}

[RootSignature(RS_MIN)]
[numthreads(64, 1, 1)]
void cs_min_struct(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    InterlockedMin(keys[id.x % spin], candidate_key(seed, id.x));
}

// The typed form: an R32G32_UINT view, the slots `stride` elements apart. TLOU
// Part I clears a 2 MB R32G32_UINT typed buffer view to all ones before its ray
// casts; the test clears one of that size and checks every element.
#define RS_MIN_TYPED "RootConstants(num32BitConstants=4, b0), DescriptorTable(UAV(u0))"
RWBuffer<uint64_t> tkeys : register(u0);

[RootSignature(RS_MIN_TYPED)]
[numthreads(64, 1, 1)]
void cs_min_typed(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    InterlockedMin(tkeys[(id.x % spin) * stride], candidate_key(seed, id.x));
}

// The groupshared form (D3D12_OPTIONS9 AtomicInt64OnGroupSharedSupported, which
// this runtime claimed; the test runs it only while the claim is made): each
// group of 256 threads takes the minimum over 4 groupshared slots, 64
// contenders each, then stores them to keys[group * 4 + s].
groupshared uint64_t gs_keys[4];

[RootSignature(RS_MIN)]
[numthreads(256, 1, 1)]
void cs_min_groupshared(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    if (gi < 4) gs_keys[gi] = 0xffffffffffffffffull;
    GroupMemoryBarrierWithGroupSync();
    InterlockedMin(gs_keys[gi % 4], candidate_key(seed, id.x));
    GroupMemoryBarrierWithGroupSync();
    if (gi < 4) keys[gid.x * 4 + gi] = gs_keys[gi];
}
