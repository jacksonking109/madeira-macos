// Shader for tests/windows/tbufoff_test.c, precompiled to DXIL in tbufoff_dxil.h.
//
// Regenerate tbufoff_dxil.h (zsh, from tests/windows, after `. ~/wine-lab/env11.sh`):
//   wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe -T cs_6_0 -E cs_main -Vn tbufoff_cs_dxil -Fh /tmp/tbufoff_cs.h tbufoff.hlsl
//   sed '/^#if 0/,/^#endif/d' /tmp/tbufoff_cs.h | tr -d '\r' | sed '/^$/d'
// and replace the array in tbufoff_dxil.h with that output.
//
// Every typed-buffer access kind through views whose FirstElement is anywhere
// (not only on a 16- or 64-byte boundary): element t of the SRV, its
// GetDimensions, a store, an atomic add, a load and GetDimensions of the UAV.
// What each returns goes to o[t].

cbuffer C : register(b0) { uint n; uint mode; };
Buffer<uint4>             srv : register(t0);
RWBuffer<uint>            uav : register(u1);
RWStructuredBuffer<uint4> o   : register(u0);

[numthreads(64, 1, 1)]
void cs_main(uint t : SV_DispatchThreadID)
{
    if (t >= n) return;
    uint w, old;
    if (mode == 0)      o[t] = srv[t];
    else if (mode == 1) { srv.GetDimensions(w); o[t] = uint4(w, 0, 0, 0); }
    else if (mode == 2) { uav[t] = 0x1000u + t; o[t] = uint4(0, 0, 0, 0); }
    else if (mode == 3) { InterlockedAdd(uav[t], 5u, old); o[t] = uint4(old, 0, 0, 0); }
    else if (mode == 4) o[t] = uint4(uav[t], 0, 0, 0);
    else                { uav.GetDimensions(w); o[t] = uint4(w, 0, 0, 0); }
}
