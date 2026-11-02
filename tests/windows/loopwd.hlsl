// Shader for tests/windows/loopwd_test.c, precompiled to DXIL in loopwd_dxil.h.
//
// Regenerate loopwd_dxil.h (zsh, from tests/windows, after `. ~/wine-lab/env11.sh`):
//   wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe -T cs_6_0 -E cs_main -Vn lw_cs_dxil -Fh /tmp/lw_cs.h loopwd.hlsl
//   sed '/^#if 0/,/^#endif/d' /tmp/lw_cs.h | tr -d '\r' | sed '/^$/d'
// and replace the array in loopwd_dxil.h with that output (the sed drops dxc's
// disassembly listing).
//
// Four loops whose length comes from a buffer, all FINITE whatever the input
// (a few tens of millions of passes at most), so a run without the watchdog
// still ends:
//   - a canonical counted loop (`for i < n`: constant start, step 1,
//     loop-invariant bound), which the watchdog caps by clamping the bound once;
//   - the same with a non-zero start (the clamp is start + cap, not cap);
//   - a counted loop with step 2, which is not canonical and keeps the
//     per-pass counter;
//   - a data-dependent one that walks a full-period LCG until it meets a
//     target value (like a BVH walk that stops at an end marker).
// The LCG is x * mul + inc mod 2^32, mul and inc from root constants; the test
// knows how many steps any target is from the seed. The second half of the
// output buffer carries the extra loops' results.

struct Input { uint n; uint seed; uint target; uint pad; };

cbuffer Lcg : register(b0) { uint mul; uint inc; };
StructuredBuffer<Input>  inp  : register(t0);
RWStructuredBuffer<uint4> outp : register(u0);

[numthreads(4, 1, 1)]
void cs_main(uint t : SV_DispatchThreadID)
{
    Input p = inp[t];

    uint acc = p.seed;
    uint i;
    for (i = 0; i < p.n; i++)
        acc = acc * mul + inc;

    uint x = p.seed, steps = 0;
    while (x != p.target) {
        x = x * mul + inc;
        steps++;
    }
    uint a2 = p.seed ^ 0x5555u, j;
    for (j = 4; j < p.n; j++)
        a2 = a2 * mul + inc;

    uint a3 = p.seed ^ 0xaaaau, k;
    for (k = 0; k < p.n; k += 2)
        a3 = a3 * mul + inc;

    outp[t] = uint4(i, acc, steps, x);
    outp[4 + t] = uint4(j, a2, k, a3);
}
