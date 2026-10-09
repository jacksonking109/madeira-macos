/*  Replay micro-benchmark for the wine-lab D3D12 port (not a correctness test).
 *
 *  Records ONE direct list that looks like a Unity frame's draw stream (DXBC SM 5.0
 *  shaders; a root signature with three root CBVs, an SRV table, a sampler table and
 *  root constants; four pipelines; a few vertex/index buffers) and executes it many
 *  times. The cost that matters is the runtime's replay of the list into Metal
 *  encoders, so the target is tiny and nearly every draw is clipped away.
 *
 *  Use:   MADEIRA_PERF_REPLAY=1 wine d3d12-bench-x64.exe [draws=2000] [frames=200] 2>&1 | grep -a "perf-replay\|bench"
 *  Prints the app-side ExecuteCommandLists time per draw (caller thread) and, with
 *  perf-replay, the runtime's per-phase breakdown. Draw k varies like a scene would:
 *    - the per-object CBV (b1) and 4 root constants change every draw
 *    - the material CBV (b2) and SRV/sampler tables change every 6 draws
 *    - the pipeline changes every 12 draws (4 pipelines)
 *    - the vertex/index buffers change every 3 draws
 *  Each draw covers its own 2x2 pixel cell with a colour computed from its CBVs, root
 *  constants and material, and the last frame is read back and hashed: an optimisation
 *  that mishandles any binding changes the hash (the image must be identical).
 */
#define COBJMACROS
#define INITGUID
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "root_sig.h"
#pragma clang diagnostic pop

typedef HRESULT (WINAPI *pfn_create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *pfn_serialize_rs)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);

enum { RT_W = 64, RT_H = 64, NPSO = 4, NMAT = 96, NVB = 8, OBJ_STRIDE = 256 };

static ID3D12Device *dev; static ID3D12CommandQueue *queue; static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list; static ID3D12Fence *fence; static UINT64 fence_v;

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC d; ID3D12Resource *r = NULL;
    ZeroMemory(&hp, sizeof hp); hp.Type = heap;
    ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}
static ID3D12Resource *make_tex2d(UINT w, UINT h, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC d; ID3D12Resource *r = NULL;
    ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Flags = fl;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}
static void wait_idle(void) {
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    fence_v++;
    ID3D12CommandQueue_Signal(queue, fence, fence_v);
    ID3D12Fence_SetEventOnCompletion(fence, fence_v, ev);
    WaitForSingleObject(ev, 60000);
    CloseHandle(ev);
}

static const char hlsl[] =
    "cbuffer Frame : register(b0) { float4 frame; };\n"
    "cbuffer Obj   : register(b1) { float4 obj; float4 obj2; };\n"
    "cbuffer Mat   : register(b2) { float4 mat; };\n"
    "cbuffer RC    : register(b3) { float4 rc; };\n"
    "Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2);\n"
    "SamplerState s0 : register(s0);\n"
    "struct VI { float3 pos : POSITION; float2 uv : TEXCOORD0; float3 n : NORMAL; };\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 n : TEXCOORD1; };\n"
    "VO vs_main(VI i) { VO o; o.pos = float4(i.pos.xy * obj.w + obj.xy, i.pos.z, 1.0); o.uv = i.uv; o.n = float3(obj2.y, frame.x, i.n.z * 0.0); return o; }\n"
    "float4 ps_main(VO i) : SV_Target {\n"
    "    float4 a = t0.Sample(s0, i.uv), b = t1.Sample(s0, i.uv), c = t2.Sample(s0, i.uv);\n"
    "    return float4(i.n.x, mat.x, rc.x + i.n.y, 1.0) + (a + b + c) * 1e-4 * mat.w;\n"
    "}\n";

static ID3DBlob *compile(pD3DCompile fn, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = fn(hlsl, strlen(hlsl), "bench", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err) { printf("compile: %.*s\n", (int)ID3D10Blob_GetBufferSize(err), (const char *)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); }
    if (FAILED(hr)) { printf("bench: compile failed %#lx\n", hr); exit(2); }
    return code;
}

int main(int argc, char **argv) {
    int ndraws = argc > 1 ? atoi(argv[1]) : 2000, nframes = argc > 2 ? atoi(argv[2]) : 200, i, f;
    int dpl = argc > 3 ? atoi(argv[3]) : 0, nl, li;   /* draws per list: 0 = one list (Unity records ~33) */
    ID3D12CommandAllocator *allocs[512]; ID3D12GraphicsCommandList *lists[512]; ID3D12CommandList *cl[512];
    HMODULE dcc = LoadLibraryA("d3dcompiler_47.dll"), d12 = LoadLibraryA("d3d12.dll");
    pD3DCompile comp = dcc ? (pD3DCompile)GetProcAddress(dcc, "D3DCompile") : NULL;
    pfn_create_device create = d12 ? (pfn_create_device)GetProcAddress(d12, "D3D12CreateDevice") : NULL;
    pfn_serialize_rs ser = d12 ? (pfn_serialize_rs)GetProcAddress(d12, "D3D12SerializeRootSignature") : NULL;
    ID3DBlob *vs, *ps, *rsb = NULL, *err = NULL;
    ID3D12RootSignature *rs = NULL; ID3D12PipelineState *pso[NPSO];
    ID3D12DescriptorHeap *rtvh, *srvh, *smph; D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    ID3D12Resource *rt, *pix, *cbuf, *vbuf[NVB], *ibuf[NVB], *tex[NMAT * 3];
    D3D12_DESCRIPTOR_RANGE r_srv, r_smp; D3D12_ROOT_PARAMETER rp[7]; D3D12_ROOT_SIGNATURE_DESC rd;
    D3D12_INPUT_ELEMENT_DESC il[3];
    HRESULT hr; UINT inc_srv, inc_smp;
    D3D12_GPU_DESCRIPTOR_HANDLE sg, mg; D3D12_CPU_DESCRIPTOR_HANDLE sc_, mc_;
    LARGE_INTEGER q0, q1, qf;
    double tot = 0; int bad = 0;

    if (!comp || !create || !ser) { printf("bench: missing d3dcompiler/d3d12\n"); return 2; }
    hr = create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    if (FAILED(hr)) { printf("bench: device %#lx\n", hr); return 2; }
    {
        D3D12_COMMAND_QUEUE_DESC qd; ZeroMemory(&qd, sizeof qd);
        ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
        ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list);
        ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    }
    vs = compile(comp, "vs_main", "vs_5_0"); ps = compile(comp, "ps_main", "ps_5_0");

    ZeroMemory(&r_srv, sizeof r_srv); r_srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r_srv.NumDescriptors = 3; r_srv.OffsetInDescriptorsFromTableStart = 0;
    ZeroMemory(&r_smp, sizeof r_smp); r_smp.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; r_smp.NumDescriptors = 1; r_smp.OffsetInDescriptorsFromTableStart = 0;
    ZeroMemory(rp, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[0].Descriptor.ShaderRegister = 0; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[1].Descriptor.ShaderRegister = 1; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[2].Descriptor.ShaderRegister = 2; rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[3].DescriptorTable.NumDescriptorRanges = 1; rp[3].DescriptorTable.pDescriptorRanges = &r_srv; rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[4].DescriptorTable.NumDescriptorRanges = 1; rp[4].DescriptorTable.pDescriptorRanges = &r_smp; rp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[5].Constants.ShaderRegister = 3; rp[5].Constants.Num32BitValues = 4; rp[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    ZeroMemory(&rd, sizeof rd); rd.NumParameters = 6; rd.pParameters = rp; rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    hr = ser(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsb, &err);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&rs);
    if (FAILED(hr)) { printf("bench: root signature %#lx\n", hr); return 2; }

    ZeroMemory(il, sizeof il);
    il[0].SemanticName = "POSITION"; il[0].Format = DXGI_FORMAT_R32G32B32_FLOAT; il[0].AlignedByteOffset = 0; il[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    il[1].SemanticName = "TEXCOORD"; il[1].Format = DXGI_FORMAT_R32G32_FLOAT; il[1].AlignedByteOffset = 12; il[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    il[2].SemanticName = "NORMAL"; il[2].Format = DXGI_FORMAT_R32G32B32_FLOAT; il[2].AlignedByteOffset = 20; il[2].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    for (i = 0; i < NPSO; i++) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
        pd.pRootSignature = rs;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
        pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 3;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        test_pso_defaults(&pd, FALSE);
        pd.RasterizerState.CullMode = (i & 1) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;   /* distinct state per pipeline */
        pd.BlendState.RenderTarget[0].BlendEnable = (i & 2) ? TRUE : FALSE;
        if (i & 2) { pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA; pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA; }
        hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso[i]);
        if (FAILED(hr)) { printf("bench: PSO %d %#lx\n", i, hr); return 2; }
    }

    /* resources */
    rt = make_tex2d(RT_W, RT_H, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    pix = make_buffer(D3D12_HEAP_TYPE_READBACK, RT_W * RT_H * 4, D3D12_RESOURCE_STATE_COPY_DEST);
    cbuf = make_buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)OBJ_STRIDE * (ndraws + NMAT + 8), D3D12_RESOURCE_STATE_GENERIC_READ);
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = NMAT * 3 + 64; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srvh);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; hd.NumDescriptors = NMAT + 4;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&smph);
    }
    rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    inc_srv = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    inc_smp = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    srvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(srvh, &sc_); srvh->lpVtbl->GetGPUDescriptorHandleForHeapStart(srvh, &sg);
    smph->lpVtbl->GetCPUDescriptorHandleForHeapStart(smph, &mc_); smph->lpVtbl->GetGPUDescriptorHandleForHeapStart(smph, &mg);
    for (i = 0; i < NMAT * 3; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv; D3D12_CPU_DESCRIPTOR_HANDLE h = sc_; h.ptr += (SIZE_T)i * inc_srv;
        tex[i] = make_tex2d(16, 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ZeroMemory(&sv, sizeof sv); sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        ID3D12Device_CreateShaderResourceView(dev, tex[i], &sv, h);
    }
    for (i = 0; i < NMAT; i++) {
        D3D12_SAMPLER_DESC sd; D3D12_CPU_DESCRIPTOR_HANDLE h = mc_; h.ptr += (SIZE_T)i * inc_smp;
        ZeroMemory(&sd, sizeof sd); sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sd.MaxAnisotropy = 1; sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER; sd.MaxLOD = D3D12_FLOAT32_MAX; sd.MipLODBias = (float)i * 0.01f;
        ID3D12Device_CreateSampler(dev, &sd, h);
    }
    {   /* per-object / material constants: object k at cbuf + k*256; object k covers its own 2x2 pixel cell, so the image depends on every binding */
        float *m = NULL; ID3D12Resource_Map(cbuf, 0, NULL, (void **)&m);
        if (m) {
            for (i = 0; i < ndraws + NMAT + 8; i++) {
                float *o = m + (size_t)i * (OBJ_STRIDE / 4);
                o[0] = (float)((i % 32) - 15.5) / 16.0f * 0.95f; o[1] = (float)(((i / 32) % 32) - 15.5) / 16.0f * 0.95f; o[2] = 0; o[3] = 0.04f;
                o[4] = 1; o[5] = (float)(i % 7) / 7.0f; o[6] = o[7] = 0;
                o[8] = (float)(i % NMAT) / 16.0f; o[9] = o[10] = 0; o[11] = 0.0f;
            }
            ID3D12Resource_Unmap(cbuf, 0, NULL);
        }
    }
    for (i = 0; i < NVB; i++) {
        float v[3 * 8]; unsigned short ix[3] = { 0, 1, 2 }; BYTE *m = NULL; int k;
        vbuf[i] = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_STATE_GENERIC_READ);
        ibuf[i] = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
        for (k = 0; k < 3; k++) { v[k*8+0] = k == 0 ? -0.9f : k == 1 ? 0.0f : 0.9f; v[k*8+1] = k == 1 ? 0.9f : -0.9f; v[k*8+2] = 0.5f; v[k*8+3] = (float)k * 0.5f; v[k*8+4] = 0.0f; v[k*8+5] = v[k*8+6] = 0; v[k*8+7] = 1; }
        ID3D12Resource_Map(vbuf[i], 0, NULL, (void **)&m); if (m) { memcpy(m, v, sizeof v); ID3D12Resource_Unmap(vbuf[i], 0, NULL); }
        ID3D12Resource_Map(ibuf[i], 0, NULL, (void **)&m); if (m) { memcpy(m, ix, sizeof ix); ID3D12Resource_Unmap(ibuf[i], 0, NULL); }
    }

    /* the lists */
    if (dpl <= 0 || dpl > ndraws) dpl = ndraws;
    nl = (ndraws + dpl - 1) / dpl; if (nl > 512) { printf("bench: too many lists\n"); return 2; }
    for (li = 0; li < nl; li++) {
        const int first = li * dpl, last = first + dpl < ndraws ? first + dpl : ndraws;

        const FLOAT clear[4] = { 0, 0, 1, 1 };
        D3D12_VIEWPORT vp; D3D12_RECT sc = { 0, 0, RT_W, RT_H }; ID3D12DescriptorHeap *heaps[2];
        UINT64 cva = ID3D12Resource_GetGPUVirtualAddress(cbuf);
        ZeroMemory(&vp, sizeof vp); vp.Width = RT_W; vp.Height = RT_H; vp.MaxDepth = 1;
        heaps[0] = srvh; heaps[1] = smph;
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocs[li]);
        ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[li], NULL, &IID_ID3D12GraphicsCommandList, (void **)&lists[li]);
        list = lists[li];
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
        if (li == 0) ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear, 0, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, cva + (UINT64)(ndraws + NMAT) * OBJ_STRIDE);
        for (i = first; i < last; i++) {
            if (i % 12 == 0 || i == first) ID3D12GraphicsCommandList_SetPipelineState(list, pso[(i / 12) % NPSO]);
            if (i % 3 == 0 || i == first) {
                D3D12_VERTEX_BUFFER_VIEW vbv; D3D12_INDEX_BUFFER_VIEW ibv; int b = (i / 3) % NVB;
                vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vbuf[b]); vbv.SizeInBytes = 96; vbv.StrideInBytes = 32;
                ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(ibuf[b]); ibv.SizeInBytes = 6; ibv.Format = DXGI_FORMAT_R16_UINT;
                ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
                ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv);
            }
            if (i % 6 == 0 || i == first) {
                int m = (i / 6) % NMAT; D3D12_GPU_DESCRIPTOR_HANDLE g = sg, s = mg;
                g.ptr += (UINT64)m * 3 * inc_srv; s.ptr += (UINT64)m * inc_smp;
                ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 2, cva + (UINT64)(ndraws + m) * OBJ_STRIDE);
                ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 3, g);
                ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 4, s);
            }
            ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 1, cva + (UINT64)i * OBJ_STRIDE);
            { float rc[4] = { 1, 1, 1, 1 }; rc[0] = (float)(i % 5) / 5.0f; ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 5, 4, rc, 0); }
            ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 3, 1, 0, 0, 0);
        }
        if (li == nl - 1) {   /* RT -> readback, back to RT, so every frame ends with the same image */
            D3D12_RESOURCE_BARRIER b; D3D12_TEXTURE_COPY_LOCATION dl, sl;
            ZeroMemory(&b, sizeof b); b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = rt;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
            ZeroMemory(&dl, sizeof dl); ZeroMemory(&sl, sizeof sl);
            dl.pResource = pix; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dl.PlacedFootprint.Footprint.Width = RT_W;
            dl.PlacedFootprint.Footprint.Height = RT_H; dl.PlacedFootprint.Footprint.Depth = 1; dl.PlacedFootprint.Footprint.RowPitch = RT_W * 4;
            sl.pResource = rt; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            ID3D12GraphicsCommandList_CopyTextureRegion(list, &dl, 0, 0, 0, &sl, NULL);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
        }
        ID3D12GraphicsCommandList_Close(list);
        cl[li] = (ID3D12CommandList *)list;
    }

    QueryPerformanceFrequency(&qf);
    for (f = 0; f < nframes; f++) {
        QueryPerformanceCounter(&q0);
        ID3D12CommandQueue_ExecuteCommandLists(queue, nl, cl);
        QueryPerformanceCounter(&q1);
        if (f >= 10) tot += (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
        wait_idle();
    }
    printf("bench: %d draws in %d lists x %d frames: ExecuteCommandLists %.3f ms/frame = %.3f us/draw (caller thread, frames 10..)\n",
           ndraws, nl, nframes, 1000.0 * tot / (nframes - 10), 1e6 * tot / ((double)(nframes - 10) * ndraws));
    {   /* image hash of the last frame: must not change when the replay is optimised */
        BYTE *p = NULL; unsigned long long h = 1469598103934665603ull; int nz = 0;
        ID3D12Resource_Map(pix, 0, NULL, (void **)&p);
        if (p) { for (i = 0; i < RT_W * RT_H * 4; i++) { h = (h ^ p[i]) * 1099511628211ull; nz += p[i] != 0 && !(i % 4 == 2 && p[i] == 255); }
                 ID3D12Resource_Unmap(pix, 0, NULL); }
        printf("bench: image hash %016llx (%d non-background bytes)\n", h, nz);
    }
    (void)bad;
    return 0;
}
