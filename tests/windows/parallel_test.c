/*  One ExecuteCommandLists of many lists, checked exactly, for the wine-lab D3D12 port's
 *  parallel encoding (madeira.cfg parallel-encode = N: a batch is cut into runs of lists,
 *  one Metal command buffer each, encoded on worker threads and committed in order).
 *
 *  Every case below depends on the ORDER of work across list boundaries, so a run that
 *  starts before the previous one finished (a missing fence at a run boundary), a run
 *  committed out of order or work landing in the wrong command buffer changes a result:
 *    - compute chain: every list runs DPL dispatches, each a read-modify-write
 *      (x = x * 1664525 + 1013904223 + k) of a 1024-word window that overlaps other
 *      lists' windows, with UAV barriers; the CPU computes the same chain exactly
 *    - copies between them: after its dispatches every list copies the whole UAV buffer's
 *      first window into its own slot of a snapshot buffer (UAV -> COPY_SOURCE -> UAV),
 *      so each snapshot must equal the CPU chain at that list's end
 *    - render passes across list boundaries: list j draws a full-target triangle into
 *      target (j / 2) % 3 (pairs of lists share a target: pass-carry), blended
 *      dst = src + dst * 0.5 with src = (j + 1) / 32 -- order dependent; checked against
 *      the CPU blend within 3/255
 *    - occlusion queries: even lists count their draw (Begin/End), the next list resolves
 *      it, and every fourth list resolves the query of three lists before (possibly in an
 *      earlier run) into a READBACK buffer; each must be 64 * 64 = 4096 samples
 *  ROUNDS submissions of NLISTS lists each, results checked after every one.
 *
 *  Use with MADEIRA_PARALLEL_ENCODE=4 MADEIRA_PARALLEL_MIN_CMDS=1 (so every boundary may be
 *  cut) and again without (serial); both must pass. Exit code = number of failed checks.
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

static int checks, fails;
#define CHECK(cond, ...) do {                                                  \
    checks++;                                                                  \
    if (!(cond)) { fails++; printf("  FAIL  " __VA_ARGS__); printf("\n"); }     \
    else         { printf("  ok    " __VA_ARGS__); printf("\n"); }             \
    fflush(stdout);                                                            \
} while (0)

typedef HRESULT (WINAPI *pfn_create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *pfn_serialize_rs)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);

enum { NLISTS = 12, DPL = 6, ROUNDS = 8, W = 64, H = 64, NWORDS = 8192, NRT = 3 };

static ID3D12Device *dev; static ID3D12CommandQueue *queue; static ID3D12Fence *fence; static UINT64 fence_v;
static pD3DCompile comp;

static const char hlsl_cs[] =
    "RWStructuredBuffer<uint> uo : register(u0);\n"
    "cbuffer K : register(b0) { uint kk; uint kbase; };\n"
    "[numthreads(64, 1, 1)] void cs_main(uint3 t : SV_DispatchThreadID) {\n"
    "    uint a = kbase + t.x;\n"
    "    uo[a] = uo[a] * 1664525u + 1013904223u + kk;\n"
    "}\n";
static const char hlsl_gfx[] =
    "cbuffer C : register(b0) { float4 col; };\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0); return float4(p, 0.5, 1.0); }\n"
    "float4 ps_main(float4 pos : SV_Position) : SV_Target { return col; }\n";

static ID3DBlob *compile(const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = comp(src, strlen(src), "parallel", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err) { printf("compile: %.*s\n", (int)ID3D10Blob_GetBufferSize(err), (const char *)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); }
    if (FAILED(hr)) { printf("parallel: compile %s failed %#lx\n", entry, hr); exit(100); }
    return code;
}
static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC d; ID3D12Resource *r = NULL;
    ZeroMemory(&hp, sizeof hp); hp.Type = heap;
    ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = fl;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **)&r))) { printf("parallel: buffer\n"); exit(100); }
    return r;
}
static ID3D12Resource *make_rt(void) {
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC d; ID3D12Resource *r = NULL; D3D12_CLEAR_VALUE cv;
    ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ZeroMemory(&cv, sizeof cv); cv.Format = d.Format;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void **)&r))) { printf("parallel: rt\n"); exit(100); }
    return r;
}
static void wait_idle(void) {
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    fence_v++;
    ID3D12CommandQueue_Signal(queue, fence, fence_v);
    ID3D12Fence_SetEventOnCompletion(fence, fence_v, ev);
    if (WaitForSingleObject(ev, 20000) != WAIT_OBJECT_0) { printf("parallel: GPU timeout\n"); exit(100); }
    CloseHandle(ev);
}
static void transition(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br; ZeroMemory(&br, sizeof br);
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &br);
}
static void uav_barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r) {
    D3D12_RESOURCE_BARRIER br; ZeroMemory(&br, sizeof br);
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; br.UAV.pResource = r;
    ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &br);
}
static unsigned blend8(unsigned dst, float src) {   /* dst = src + dst * 0.5 in UNORM8, as the GPU rounds (to nearest) */
    float v = src + (dst / 255.0f) * 0.5f;
    if (v > 1.0f) v = 1.0f;
    return (unsigned)(v * 255.0f + 0.5f);
}

int main(void) {
    HMODULE dcc = LoadLibraryA("d3dcompiler_47.dll"), d12 = LoadLibraryA("d3d12.dll");
    pfn_create_device create = d12 ? (pfn_create_device)GetProcAddress(d12, "D3D12CreateDevice") : NULL;
    pfn_serialize_rs ser = d12 ? (pfn_serialize_rs)GetProcAddress(d12, "D3D12SerializeRootSignature") : NULL;
    ID3D12CommandAllocator *alloc[NLISTS + 1]; ID3D12GraphicsCommandList *lists[NLISTS + 1]; ID3D12CommandList *cl[NLISTS];
    ID3D12RootSignature *crs, *grs; ID3D12PipelineState *cpso, *gpso;
    ID3D12Resource *ubuf, *snap, *snap_rb, *rt[NRT], *rt_rb, *q_rb, *zero;
    ID3D12DescriptorHeap *srvh, *rtvh; ID3D12QueryHeap *qh;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[NRT]; D3D12_GPU_DESCRIPTOR_HANDLE ug;
    UINT inc_rtv, rt_pitch = W * 4;
    static UINT32 ref[NWORDS], ref_snap[NLISTS][1024];
    unsigned ref_rt[NRT][3];
    int round, j, d, i;
    HRESULT hr;

    comp = dcc ? (pD3DCompile)GetProcAddress(dcc, "D3DCompile") : NULL;
    if (!comp || !create || !ser) { printf("parallel: missing d3dcompiler/d3d12\n"); return 100; }
    if (FAILED(create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev))) { printf("parallel: no device\n"); return 100; }
    {
        D3D12_COMMAND_QUEUE_DESC qd; ZeroMemory(&qd, sizeof qd);
        ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
        ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
        for (j = 0; j <= NLISTS; j++) {
            ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc[j]);
            ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[j], NULL, &IID_ID3D12GraphicsCommandList, (void **)&lists[j]);
            ID3D12GraphicsCommandList_Close(lists[j]);
        }
    }
    {   /* compute: UAV table (u0) + 2 root constants (b0) */
        static D3D12_DESCRIPTOR_RANGE r; D3D12_ROOT_PARAMETER p[2]; D3D12_ROOT_SIGNATURE_DESC rd; ID3DBlob *b = NULL, *e = NULL;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd; ID3DBlob *cs = compile(hlsl_cs, "cs_main", "cs_5_0");
        ZeroMemory(&r, sizeof r); r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r.NumDescriptors = 1;
        ZeroMemory(p, sizeof p);
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[0].DescriptorTable.NumDescriptorRanges = 1; p[0].DescriptorTable.pDescriptorRanges = &r;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[1].Constants.Num32BitValues = 2;
        ZeroMemory(&rd, sizeof rd); rd.NumParameters = 2; rd.pParameters = p;
        hr = ser(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &b, &e);
        if (SUCCEEDED(hr)) hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), &IID_ID3D12RootSignature, (void **)&crs);
        if (FAILED(hr)) { printf("parallel: compute root signature %#lx\n", hr); return 100; }
        ZeroMemory(&pd, sizeof pd); pd.pRootSignature = crs;
        pd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs); pd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs);
        if (FAILED(ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&cpso))) { printf("parallel: compute PSO\n"); return 100; }
    }
    {   /* graphics: 4 root constants (b0), dst = src + dst * blend factor */
        D3D12_ROOT_PARAMETER p[1]; D3D12_ROOT_SIGNATURE_DESC rd; ID3DBlob *b = NULL, *e = NULL, *vs, *ps;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
        ZeroMemory(p, sizeof p);
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.Num32BitValues = 4;
        ZeroMemory(&rd, sizeof rd); rd.NumParameters = 1; rd.pParameters = p;
        hr = ser(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &b, &e);
        if (SUCCEEDED(hr)) hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), &IID_ID3D12RootSignature, (void **)&grs);
        if (FAILED(hr)) { printf("parallel: graphics root signature %#lx\n", hr); return 100; }
        vs = compile(hlsl_gfx, "vs_main", "vs_5_0"); ps = compile(hlsl_gfx, "ps_main", "ps_5_0");
        ZeroMemory(&pd, sizeof pd);
        pd.pRootSignature = grs;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        test_pso_defaults(&pd, FALSE);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE; pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_BLEND_FACTOR;
        pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE; pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (FAILED(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&gpso))) { printf("parallel: graphics PSO\n"); return 100; }
    }
    ubuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, NWORDS * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    snap = make_buffer(D3D12_HEAP_TYPE_DEFAULT, NLISTS * 4096, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    snap_rb = make_buffer(D3D12_HEAP_TYPE_READBACK, NLISTS * 4096, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    rt_rb = make_buffer(D3D12_HEAP_TYPE_READBACK, NRT * W * H * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    q_rb = make_buffer(D3D12_HEAP_TYPE_READBACK, 2 * NLISTS * 8, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    zero = make_buffer(D3D12_HEAP_TYPE_UPLOAD, NWORDS * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    { BYTE *m = NULL; ID3D12Resource_Map(zero, 0, NULL, (void **)&m); if (m) { memset(m, 0, NWORDS * 4); ID3D12Resource_Unmap(zero, 0, NULL); } }
    for (i = 0; i < NRT; i++) rt[i] = make_rt();
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd; D3D12_UNORDERED_ACCESS_VIEW_DESC uv; D3D12_CPU_DESCRIPTOR_HANDLE h; D3D12_QUERY_HEAP_DESC qd;
        ZeroMemory(&hd, sizeof hd); hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srvh);
        srvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(srvh, &h); srvh->lpVtbl->GetGPUDescriptorHandleForHeapStart(srvh, &ug);
        ZeroMemory(&uv, sizeof uv); uv.Format = DXGI_FORMAT_UNKNOWN; uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uv.Buffer.NumElements = NWORDS; uv.Buffer.StructureByteStride = 4;
        ID3D12Device_CreateUnorderedAccessView(dev, ubuf, NULL, &uv, h);
        ZeroMemory(&hd, sizeof hd); hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = NRT;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
        rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &h);
        inc_rtv = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (i = 0; i < NRT; i++) { rtv[i] = h; rtv[i].ptr += (SIZE_T)i * inc_rtv; ID3D12Device_CreateRenderTargetView(dev, rt[i], NULL, rtv[i]); }
        ZeroMemory(&qd, sizeof qd); qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION; qd.Count = NLISTS;
        if (FAILED(ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&qh))) { printf("parallel: query heap\n"); return 100; }
    }
    {   /* zero the UAV buffer once */
        ID3D12GraphicsCommandList *l = lists[NLISTS];
        ID3D12CommandAllocator_Reset(alloc[NLISTS]); ID3D12GraphicsCommandList_Reset(l, alloc[NLISTS], NULL);
        ID3D12GraphicsCommandList_CopyBufferRegion(l, ubuf, 0, zero, 0, NWORDS * 4);
        transition(l, ubuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12GraphicsCommandList_Close(l);
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&l);
        wait_idle();
    }
    memset(ref, 0, sizeof ref);

    for (round = 0; round < ROUNDS; round++) {
        int bad_chain = 0, bad_snap = 0, bad_rt = 0, bad_q = 0; unsigned worst_rt = 0;
        UINT64 qv[2 * NLISTS];
        for (i = 0; i < NRT; i++) ref_rt[i][0] = ref_rt[i][1] = ref_rt[i][2] = 0;
        for (j = 0; j < NLISTS; j++) {
            ID3D12GraphicsCommandList *l = lists[j];
            ID3D12DescriptorHeap *heaps[1] = { srvh };
            const float clear[4] = { 0, 0, 0, 0 }, bf[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
            D3D12_VIEWPORT vp; D3D12_RECT sc = { 0, 0, W, H };
            int t = (j / 2) % NRT, draw = (j % 3) != 2;   /* every third list is compute + copy only */
            ID3D12CommandAllocator_Reset(alloc[j]); ID3D12GraphicsCommandList_Reset(l, alloc[j], NULL);
            if (j == 0) for (i = 0; i < NRT; i++) ID3D12GraphicsCommandList_ClearRenderTargetView(l, rtv[i], clear, 0, NULL);
            /* compute chain */
            uav_barrier(l, ubuf);
            ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 1, heaps);
            ID3D12GraphicsCommandList_SetComputeRootSignature(l, crs);
            ID3D12GraphicsCommandList_SetPipelineState(l, cpso);
            ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(l, 0, ug);
            for (d = 0; d < DPL; d++) {
                UINT k[2]; k[0] = (UINT)(round * 1000 + j * DPL + d); k[1] = (UINT)(((j + d + round) % 7) * 1024);
                ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(l, 1, 2, k, 0);
                ID3D12GraphicsCommandList_Dispatch(l, 16, 1, 1);
                uav_barrier(l, ubuf);
                for (i = 0; i < 1024; i++) ref[k[1] + i] = ref[k[1] + i] * 1664525u + 1013904223u + k[0];
            }
            memcpy(ref_snap[j], ref, sizeof ref_snap[j]);
            /* snapshot of window 0 into slot j */
            transition(l, ubuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ID3D12GraphicsCommandList_CopyBufferRegion(l, snap, (UINT64)j * 4096, ubuf, 0, 4096);
            transition(l, ubuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (draw) {   /* blended draw into target t, counted by query j on even lists */
                float col[4]; col[0] = (float)(j + 1) / 32.0f; col[1] = (float)(round + 1) / 16.0f; col[2] = 0.25f; col[3] = 1.0f;
                ZeroMemory(&vp, sizeof vp); vp.Width = W; vp.Height = H; vp.MaxDepth = 1;
                ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv[t], FALSE, NULL);
                ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
                ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
                ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, grs);
                ID3D12GraphicsCommandList_SetPipelineState(l, gpso);
                ID3D12GraphicsCommandList_OMSetBlendFactor(l, bf);
                ID3D12GraphicsCommandList_IASetPrimitiveTopology(l, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, 0, 4, col, 0);
                if (!(j & 1)) ID3D12GraphicsCommandList_BeginQuery(l, qh, D3D12_QUERY_TYPE_OCCLUSION, j);
                ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
                if (!(j & 1)) ID3D12GraphicsCommandList_EndQuery(l, qh, D3D12_QUERY_TYPE_OCCLUSION, j);
                for (i = 0; i < 3; i++) ref_rt[t][i] = blend8(ref_rt[t][i], col[i]);
            }
            if ((j & 1) && ((j - 1) % 3) != 2)   /* resolve the previous list's query */
                ID3D12GraphicsCommandList_ResolveQueryData(l, qh, D3D12_QUERY_TYPE_OCCLUSION, j - 1, 1, q_rb, (UINT64)(j - 1) * 8);
            if ((j & 3) == 3 && j >= 3 && !((j - 3) & 1) && ((j - 3) % 3) != 2)   /* and one from three lists back */
                ID3D12GraphicsCommandList_ResolveQueryData(l, qh, D3D12_QUERY_TYPE_OCCLUSION, j - 3, 1, q_rb, (UINT64)(NLISTS + j - 3) * 8);
            if (j == NLISTS - 1) {   /* read everything back */
                transition(l, snap, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                ID3D12GraphicsCommandList_CopyBufferRegion(l, snap_rb, 0, snap, 0, NLISTS * 4096);
                transition(l, snap, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                for (i = 0; i < NRT; i++) {
                    D3D12_TEXTURE_COPY_LOCATION dl, sl;
                    transition(l, rt[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    ZeroMemory(&dl, sizeof dl); ZeroMemory(&sl, sizeof sl);
                    dl.pResource = rt_rb; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dl.PlacedFootprint.Offset = (UINT64)i * W * H * 4;
                    dl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dl.PlacedFootprint.Footprint.Width = W;
                    dl.PlacedFootprint.Footprint.Height = H; dl.PlacedFootprint.Footprint.Depth = 1; dl.PlacedFootprint.Footprint.RowPitch = rt_pitch;
                    sl.pResource = rt[i]; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    ID3D12GraphicsCommandList_CopyTextureRegion(l, &dl, 0, 0, 0, &sl, NULL);
                    transition(l, rt[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }
            ID3D12GraphicsCommandList_Close(l);
            cl[j] = (ID3D12CommandList *)l;
        }
        {   /* poison the query results */
            BYTE *m = NULL; ID3D12Resource_Map(q_rb, 0, NULL, (void **)&m);
            if (m) { memset(m, 0xcd, 2 * NLISTS * 8); ID3D12Resource_Unmap(q_rb, 0, NULL); }
        }
        ID3D12CommandQueue_ExecuteCommandLists(queue, NLISTS, cl);
        wait_idle();
        {   /* the whole chain: copy the UAV buffer out with its own list */
            ID3D12GraphicsCommandList *l = lists[NLISTS]; ID3D12Resource *full = make_buffer(D3D12_HEAP_TYPE_READBACK, NWORDS * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
            UINT32 *u = NULL;
            ID3D12CommandAllocator_Reset(alloc[NLISTS]); ID3D12GraphicsCommandList_Reset(l, alloc[NLISTS], NULL);
            transition(l, ubuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ID3D12GraphicsCommandList_CopyBufferRegion(l, full, 0, ubuf, 0, NWORDS * 4);
            transition(l, ubuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12GraphicsCommandList_Close(l);
            ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&l);
            wait_idle();
            ID3D12Resource_Map(full, 0, NULL, (void **)&u);
            if (u) { for (i = 0; i < NWORDS; i++) bad_chain += u[i] != ref[i]; ID3D12Resource_Unmap(full, 0, NULL); } else bad_chain = -1;
            ID3D12Resource_Release(full);
        }
        {
            UINT32 *u = NULL;
            ID3D12Resource_Map(snap_rb, 0, NULL, (void **)&u);
            if (u) { for (j = 0; j < NLISTS; j++) for (i = 0; i < 1024; i++) bad_snap += u[j * 1024 + i] != ref_snap[j][i]; ID3D12Resource_Unmap(snap_rb, 0, NULL); }
            else bad_snap = -1;
        }
        {
            BYTE *p = NULL;
            ID3D12Resource_Map(rt_rb, 0, NULL, (void **)&p);
            if (p) {
                for (i = 0; i < NRT; i++) {
                    int x;
                    for (x = 0; x < W * H; x++) {
                        unsigned c, dd;
                        for (c = 0; c < 3; c++) {
                            int v = p[(size_t)i * W * H * 4 + (size_t)x * 4 + c];
                            dd = (unsigned)abs(v - (int)ref_rt[i][c]);
                            if (dd > worst_rt) worst_rt = dd;
                            if (dd > 3) bad_rt++;
                        }
                    }
                }
                ID3D12Resource_Unmap(rt_rb, 0, NULL);
            } else bad_rt = -1;
        }
        {
            UINT64 *qp = NULL; int n = 0;
            ID3D12Resource_Map(q_rb, 0, NULL, (void **)&qp);
            if (qp) {
                memcpy(qv, qp, sizeof qv); ID3D12Resource_Unmap(q_rb, 0, NULL);
                for (j = 1; j < NLISTS; j += 2) if (((j - 1) % 3) != 2) { n++; if (qv[j - 1] != W * H) { bad_q++; if (bad_q <= 3) printf("    query %d (next list) = %llu\n", j - 1, (unsigned long long)qv[j - 1]); } }
                for (j = 3; j < NLISTS; j += 4) if (!((j - 3) & 1) && ((j - 3) % 3) != 2) { n++; if (qv[NLISTS + j - 3] != W * H) { bad_q++; if (bad_q <= 3) printf("    query %d (3 lists later) = %llu\n", j - 3, (unsigned long long)qv[NLISTS + j - 3]); } }
            } else bad_q = -1;
            (void)n;
        }
        CHECK(bad_chain == 0, "round %d: compute chain over %d lists x %d dispatches: %d of %d words differ from the CPU", round, NLISTS, DPL, bad_chain, NWORDS);
        CHECK(bad_snap == 0, "round %d: per-list snapshots (copy after each list's dispatches): %d of %d words differ", round, bad_snap, NLISTS * 1024);
        CHECK(bad_rt == 0, "round %d: order-dependent blending across lists into %d targets: %d channels off by > 3 (worst %u)", round, NRT, bad_rt, worst_rt);
        CHECK(bad_q == 0, "round %d: occlusion queries resolved in a later list: %d wrong (expected %d samples)", round, bad_q, W * H);
    }
    printf("%d checks, %d failures\n", checks, fails);
    return fails;
}
