// Shaders for tests/windows/occlusion_test.c, precompiled to DXIL in occlusion_dxil.h.
//
// Regenerate occlusion_dxil.h (zsh, from tests/windows, after `. ~/wine-lab/env11.sh`):
//   dxc() { wine ~/wine-lab/deps/dxc/bin/x64/dxc.exe "$@"; }
//   dxc -T vs_6_0 -E vs_main -Vn oc_vs_dxil -Fh /tmp/oc_vs_main.h occlusion.hlsl
//   dxc -T ps_6_0 -E ps_main -Vn oc_ps_dxil -Fh /tmp/oc_ps_main.h occlusion.hlsl
//   for e in vs_main ps_main; do sed '/^#if 0/,/^#endif/d' /tmp/oc_$e.h | tr -d '\r' | sed '/^$/d'; echo; done
// and replace the arrays in occlusion_dxil.h with that output (the sed drops dxc's
// disassembly listing). The root signature (empty) is embedded in vs_main.

struct V { float4 pos : SV_Position; };

// Fullscreen triangle from SV_VertexID: no vertex buffer, no input layout.
[RootSignature("")]
V vs_main(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    V o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 ps_main(V i) : SV_Target
{
    return float4(1.0, 0.5, 0.25, 1.0);
}
