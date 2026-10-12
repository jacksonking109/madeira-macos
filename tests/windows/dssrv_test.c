/*  Depth/stencil shader views and stencil-masked fullscreen passes through the
 *  SM 5.x DXBC (airconv) backend, for the wine-lab D3D12 port.
 *
 *  The shapes are Rise of the Tomb Raider's DX12 lighting setup (capture of
 *  its main menu, frame 781): a depth prepass writes an R24G8_TYPELESS target,
 *  a PS writing SV_Depth downsamples it, a void PS with discard marks "edge"
 *  pixels in the stencil (REPLACE), later fullscreen passes read the depth
 *  through an R24_UNORM_X8_TYPELESS view (Load and Gather) and the stencil
 *  through an X24_TYPELESS_G8_UINT view (.y), and the AO / light upsample passes
 *  run with stencil NOT_EQUAL (read mask 2, ref 2) and depth GREATER_EQUAL from
 *  a VS at z = 1 under a viewport of MaxDepth 0.98. Shadow maps are R16_TYPELESS
 *  (D16 DSV, R16_UNORM SRV) read with SampleCmp through static and heap
 *  comparison samplers.
 *
 *    1  fill: SV_Depth pattern + stencil 0x01 everywhere, 0x82 on even columns
 *       (a void PS that discards odd columns, stencil REPLACE)
 *    2  R24_UNORM_X8 Load, X24_G8_UINT Load(.y), Gather .w/.z vs the pattern
 *    3  R24_UNORM_X8 SampleLevel / Sample (point) vs the pattern
 *    4  stencil NOT_EQUAL & 2 + depth GREATER_EQUAL at z = 0.98 (read-only DSV)
 *    5  SampleCmpLevelZero / SampleCmp on D16 and D32 maps (LESS_EQUAL),
 *       static and descriptor-heap comparison samplers
 *    6  the light-data reads of its lighting shaders, as SM 5.0 AND SM 5.1
 *       (its DX12 shaders are 5.1, which the D3D11 path never sees): a
 *       StructuredBuffer of 48-byte lights, a ByteAddressBuffer, a typed
 *       Buffer<uint4> view with FirstElement 5, a Texture2DArray slice
 *    7  a MAXIMUM-reduction point sampler (ComparisonFunc LESS left set) used
 *       with SampleLevel: a plain sample of the texel, not a comparison
 *
 *  Exit code = number of failed checks.
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
#include <math.h>
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
typedef HRESULT (WINAPI *pfn_serialize_rs)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION,
                                           ID3DBlob **, ID3DBlob **);

enum { W = 64, H = 64, ROW = W * 16 };   /* RGBA32F: 1024-byte rows, already 256-aligned */

static pD3DCompile         p_compile;
static pfn_serialize_rs    p_serialize;
static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;
static ID3D12Resource     *rt, *rb;
static ID3D12DescriptorHeap *rtvh, *dsvh, *srvh, *smph;
static D3D12_CPU_DESCRIPTOR_HANDLE rtv;
static float out_px[W * H * 4];

static float pat_depth(UINT x, UINT y) { return ((float)(x + W * y) + 0.5f) / (float)(W * H); }
static UINT8 pat_stencil(UINT x) { return (x & 1) ? 0x01 : 0x82; }

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = type;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}
static ID3D12Resource *make_tex(DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE *cv) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1; d.Flags = fl;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, cv, &IID_ID3D12Resource, (void **)&r);
    return r;
}
static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b; ZeroMemory(&b, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}
static BOOL submit_and_wait(void) {
    ID3D12CommandList *l[1] = { (ID3D12CommandList *)list };
    if (FAILED(ID3D12GraphicsCommandList_Close(list))) { printf("        list Close failed\n"); return FALSE; }
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, l);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    fence_v++;
    ID3D12Fence_SetEventOnCompletion(fence, fence_v, ev);
    ID3D12CommandQueue_Signal(queue, fence, fence_v);
    DWORD w = WaitForSingleObject(ev, 15000);
    CloseHandle(ev);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    return w == WAIT_OBJECT_0;
}
static D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE t, UINT i) {
    D3D12_CPU_DESCRIPTOR_HANDLE c; h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &c);
    c.ptr += (SIZE_T)i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, t); return c;
}
static D3D12_GPU_DESCRIPTOR_HANDLE gpu_at(ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE t, UINT i) {
    D3D12_GPU_DESCRIPTOR_HANDLE g; h->lpVtbl->GetGPUDescriptorHandleForHeapStart(h, &g);
    g.ptr += (UINT64)i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, t); return g;
}

static ID3DBlob *compile(const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = p_compile(src, strlen(src), entry, NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err) { printf("        %s: %.*s\n", entry, (int)ID3D10Blob_GetBufferSize(err), (const char *)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); }
    CHECK(SUCCEEDED(hr) && code, "%s compiled to %s DXBC (%#lx)", entry, target, hr);
    return SUCCEEDED(hr) ? code : NULL;
}
static ID3D12RootSignature *make_rs(const D3D12_ROOT_SIGNATURE_DESC *rd) {
    ID3DBlob *blob = NULL, *err = NULL; ID3D12RootSignature *rs = NULL;
    HRESULT hr = p_serialize(rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err);
    if (err) ID3D10Blob_Release(err);
    if (SUCCEEDED(hr) && blob)
        hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                              &IID_ID3D12RootSignature, (void **)&rs);
    if (blob) ID3D10Blob_Release(blob);
    CHECK(SUCCEEDED(hr) && rs, "root signature created (%#lx)", hr);
    return rs;
}

/* Fullscreen triangle; z is a compile-time constant of the entry point. */
static const char hlsl[] =
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs_at(uint id, float z) { V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), z, 1.0); o.uv = uv; return o; }\n"
    "V vs_near(uint id : SV_VertexID) { return vs_at(id, 0.0); }\n"
    "V vs_far(uint id : SV_VertexID) { return vs_at(id, 1.0); }\n"
    "float ps_fill(V i) : SV_Depth { uint2 c = uint2(i.pos.xy); return (c.x + 64 * c.y + 0.5) / 4096.0; }\n"
    "void ps_mark(V i) { if ((uint(i.pos.x) & 1) != 0) discard; }\n"
    "Texture2D<float> dep : register(t0);\n"
    "Texture2D<uint2> stn : register(t1);\n"
    "SamplerState pt : register(s0);\n"
    "SamplerComparisonState cmp : register(s1);\n"
    "float4 ps_load(V i) : SV_Target {\n"
    "    int3 c = int3(i.pos.xy, 0); float2 g = (i.pos.xy + 0.25) / 64.0;\n"
    "    float4 q = dep.Gather(pt, g);\n"
    "    return float4(dep.Load(c), (float)stn.Load(c).y, q.w, q.z); }\n"
    "float4 ps_sample(V i) : SV_Target {\n"
    "    return float4(dep.SampleLevel(pt, i.uv, 0), dep.Sample(pt, i.uv), 0, 1); }\n"
    "float4 ps_white(V i) : SV_Target { return float4(1, 1, 1, 1); }\n"
    "float4 ps_cmp(V i) : SV_Target {\n"
    "    return float4(dep.SampleCmpLevelZero(cmp, i.uv, 0.4), dep.SampleCmpLevelZero(cmp, i.uv, 0.6),\n"
    "                  dep.SampleCmp(cmp, i.uv, 0.4), dep.SampleCmp(cmp, i.uv, 0.6)); }\n";

/* Case 6: per pixel x, element i = x % 8 of each buffer; expected values are
 * functions of (i, component) only. */
static const char hlsl_buf[] =
    "struct L { float4 a; float4 b; float4 c; };\n"
    "StructuredBuffer<L> lights : register(t0);\n"
    "ByteAddressBuffer raw : register(t1);\n"
    "Buffer<uint4> list : register(t2);\n"
    "Texture2DArray<float4> arr : register(t3);\n"
    "float4 ps_buf(float4 p : SV_Position) : SV_Target {\n"
    "    uint i = uint(p.x) & 7;\n"
    "    return float4(lights[i].c.y, asfloat(raw.Load(4 * (i * 12 + 9))), (float)list[i].z,\n"
    "                  arr.Load(int4(i, 0, 2, 0)).w); }\n"
    "float4 vs_buf(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); }\n";
static float buf_val(UINT i, UINT comp) { return (float)(i * 16 + comp) + 0.25f; }   /* float k of light i */

static ID3D12PipelineState *make_pso(ID3D12RootSignature *rs, ID3DBlob *vs, ID3DBlob *ps, BOOL color, DXGI_FORMAT dsv,
                                     const D3D12_DEPTH_STENCIL_DESC *ds, const char *what) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
    ID3D12PipelineState *pso = NULL;
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.NumRenderTargets = color ? 1 : 0;
    if (color) pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.DSVFormat = dsv; pd.SampleDesc.Count = 1;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    test_pso_defaults(&pd, FALSE);
    if (ds) pd.DepthStencilState = *ds;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    CHECK(SUCCEEDED(hr) && pso, "%s: pipeline created (%#lx)", what, hr);
    return pso;
}

static void viewport(float maxz) {
    D3D12_VIEWPORT vp = { 0, 0, (FLOAT)W, (FLOAT)H, 0.0f, maxz };
    D3D12_RECT sc = { 0, 0, W, H };
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
}
static void clear_rt(float v) {
    const FLOAT c[4] = { v, v, v, v };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, c, 0, NULL);
}
/* RT -> readback, submit, copy into out_px. */
static BOOL read_rt(void) {
    D3D12_TEXTURE_COPY_LOCATION dl, sl; ZeroMemory(&dl, sizeof dl); ZeroMemory(&sl, sizeof sl);
    dl.pResource = rb; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    dl.PlacedFootprint.Footprint.Width = W; dl.PlacedFootprint.Footprint.Height = H;
    dl.PlacedFootprint.Footprint.Depth = 1; dl.PlacedFootprint.Footprint.RowPitch = ROW;
    sl.pResource = rt; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dl, 0, 0, 0, &sl, NULL);
    barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!submit_and_wait()) { printf("        fence wait timed out\n"); return FALSE; }
    void *p = NULL;
    ID3D12Resource_Map(rb, 0, NULL, &p);
    if (!p) return FALSE;
    memcpy(out_px, p, sizeof out_px);
    ID3D12Resource_Unmap(rb, 0, NULL);
    return TRUE;
}
static const float *px(UINT x, UINT y) { return out_px + 4 * (x + W * y); }

int main(void) {
    HRESULT hr;
    printf("wine-lab d3d12: depth/stencil SRVs and stencil-masked passes (DXBC)\n\n");
    HMODULE dcc = LoadLibraryA("d3dcompiler_47.dll");
    p_compile = dcc ? (pD3DCompile)GetProcAddress(dcc, "D3DCompile") : NULL;
    HMODULE d12 = LoadLibraryA("d3d12.dll");
    pfn_create_device create = d12 ? (pfn_create_device)GetProcAddress(d12, "D3D12CreateDevice") : NULL;
    p_serialize = d12 ? (pfn_serialize_rs)GetProcAddress(d12, "D3D12SerializeRootSignature") : NULL;
    CHECK(p_compile && create && p_serialize, "d3dcompiler_47 + d3d12 entry points resolved");
    if (!p_compile || !create || !p_serialize) goto done;
    hr = create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    CHECK(SUCCEEDED(hr) && dev, "device created (%#lx)", hr);
    if (!dev) goto done;
    D3D12_COMMAND_QUEUE_DESC qd; ZeroMemory(&qd, sizeof qd); qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
    if (alloc) ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list);
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    if (!queue || !list || !fence) goto done;

    D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 4;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsvh);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 16; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srvh);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; hd.NumDescriptors = 4;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&smph);

    rt = make_tex(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL);
    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, (UINT64)ROW * H, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_CLEAR_VALUE cv; ZeroMemory(&cv, sizeof cv); cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.0f;
    ID3D12Resource *ds = make_tex(DXGI_FORMAT_R24G8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);
    cv.Format = DXGI_FORMAT_D16_UNORM;
    ID3D12Resource *sm16 = make_tex(DXGI_FORMAT_R16_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    ID3D12Resource *sm32 = make_tex(DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);
    CHECK(rtvh && dsvh && srvh && smph && rt && rb && ds && sm16 && sm32, "heaps, RGBA32F target, R24G8 / R16 / R32 depth textures created");
    if (!rtvh || !dsvh || !srvh || !smph || !rt || !rb || !ds || !sm16 || !sm32) goto done;

    rtv = cpu_at(rtvh, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 0);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dv; ZeroMemory(&dv, sizeof dv);
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_rw = cpu_at(dsvh, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 0), dsv_ro = cpu_at(dsvh, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv16 = cpu_at(dsvh, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2), dsv32 = cpu_at(dsvh, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 3);
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; ID3D12Device_CreateDepthStencilView(dev, ds, &dv, dsv_rw);
    dv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL; ID3D12Device_CreateDepthStencilView(dev, ds, &dv, dsv_ro);
    dv.Flags = 0; dv.Format = DXGI_FORMAT_D16_UNORM; ID3D12Device_CreateDepthStencilView(dev, sm16, &dv, dsv16);
    dv.Format = DXGI_FORMAT_D32_FLOAT; ID3D12Device_CreateDepthStencilView(dev, sm32, &dv, dsv32);

    /* SRV heap: [0] depth R24_UNORM_X8, [1] stencil X24_G8_UINT, [2] D16 as R16_UNORM, [3] null, [4] D32 as R32_FLOAT, [5] null */
    D3D12_SHADER_RESOURCE_VIEW_DESC sv; ZeroMemory(&sv, sizeof sv);
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; ID3D12Device_CreateShaderResourceView(dev, ds, &sv, cpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0));
    sv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT; sv.Texture2D.PlaneSlice = 1;
    ID3D12Device_CreateShaderResourceView(dev, ds, &sv, cpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1));
    sv.Texture2D.PlaneSlice = 0;
    sv.Format = DXGI_FORMAT_R16_UNORM; ID3D12Device_CreateShaderResourceView(dev, sm16, &sv, cpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2));
    sv.Format = DXGI_FORMAT_R32_FLOAT; ID3D12Device_CreateShaderResourceView(dev, sm32, &sv, cpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4));
    ID3D12Device_CreateShaderResourceView(dev, NULL, NULL, cpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3));
    ID3D12Device_CreateShaderResourceView(dev, NULL, NULL, cpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5));
    D3D12_SAMPLER_DESC sd; ZeroMemory(&sd, sizeof sd);   /* heap comparison sampler, as a table s1 */
    sd.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; sd.MaxLOD = D3D12_FLOAT32_MAX; sd.MaxAnisotropy = 1;
    ID3D12Device_CreateSampler(dev, &sd, cpu_at(smph, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 0));

    ID3DBlob *vs_near = compile(hlsl, "vs_near", "vs_5_0"), *vs_far = compile(hlsl, "vs_far", "vs_5_0");
    ID3DBlob *ps_fill = compile(hlsl, "ps_fill", "ps_5_0"), *ps_mark = compile(hlsl, "ps_mark", "ps_5_0");
    ID3DBlob *ps_load = compile(hlsl, "ps_load", "ps_5_0"), *ps_sample = compile(hlsl, "ps_sample", "ps_5_0");
    ID3DBlob *ps_white = compile(hlsl, "ps_white", "ps_5_0"), *ps_cmp = compile(hlsl, "ps_cmp", "ps_5_0");
    if (!vs_near || !vs_far || !ps_fill || !ps_mark || !ps_load || !ps_sample || !ps_white || !ps_cmp) goto done;

    /* Root signature A: table t0-t1, static point sampler s0, static LESS_EQUAL comparison sampler s1.
     * Root signature B: table t0-t1 and a SAMPLER table s1 (heap comparison sampler), static s0. */
    D3D12_DESCRIPTOR_RANGE r_srv = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
    D3D12_DESCRIPTOR_RANGE r_smp = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 1, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2]; ZeroMemory(rp, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &r_srv; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1] = rp[0]; rp[1].DescriptorTable.pDescriptorRanges = &r_smp;
    D3D12_STATIC_SAMPLER_DESC ss[2]; ZeroMemory(ss, sizeof ss);
    ss[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss[0].AddressU = ss[0].AddressV = ss[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss[0].MaxAnisotropy = 1; ss[0].MaxLOD = D3D12_FLOAT32_MAX; ss[0].ShaderRegister = 0;
    ss[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    ss[1] = ss[0]; ss[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    ss[1].AddressU = ss[1].AddressV = ss[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    ss[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    ss[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; ss[1].ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rd; ZeroMemory(&rd, sizeof rd);
    rd.NumParameters = 1; rd.pParameters = rp; rd.NumStaticSamplers = 2; rd.pStaticSamplers = ss;
    ID3D12RootSignature *rsA = make_rs(&rd);
    rd.NumParameters = 2; rd.NumStaticSamplers = 1;
    ID3D12RootSignature *rsB = make_rs(&rd);
    if (!rsA || !rsB) goto done;

    D3D12_DEPTH_STENCIL_DESC dfill; ZeroMemory(&dfill, sizeof dfill);
    dfill.DepthEnable = TRUE; dfill.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; dfill.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    dfill.StencilEnable = TRUE; dfill.StencilReadMask = 0xff; dfill.StencilWriteMask = 0xff;
    dfill.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    dfill.FrontFace.StencilFailOp = dfill.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    dfill.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    dfill.BackFace = dfill.FrontFace;
    D3D12_DEPTH_STENCIL_DESC dmark = dfill; dmark.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    D3D12_DEPTH_STENCIL_DESC dmask; ZeroMemory(&dmask, sizeof dmask);   /* ROTTR's upsample passes */
    dmask.DepthEnable = TRUE; dmask.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; dmask.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    dmask.StencilEnable = TRUE; dmask.StencilReadMask = 2; dmask.StencilWriteMask = 0;
    dmask.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL;
    dmask.FrontFace.StencilFailOp = dmask.FrontFace.StencilDepthFailOp = dmask.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    dmask.BackFace = dmask.FrontFace;
    D3D12_DEPTH_STENCIL_DESC dclear; ZeroMemory(&dclear, sizeof dclear);

    ID3D12PipelineState *p_fill = make_pso(rsA, vs_near, ps_fill, FALSE, DXGI_FORMAT_D24_UNORM_S8_UINT, &dfill, "fill (SV_Depth + stencil REPLACE)");
    ID3D12PipelineState *p_mark = make_pso(rsA, vs_near, ps_mark, FALSE, DXGI_FORMAT_D24_UNORM_S8_UINT, &dmark, "mark (void PS with discard, stencil REPLACE)");
    ID3D12PipelineState *p_load = make_pso(rsA, vs_near, ps_load, TRUE, DXGI_FORMAT_UNKNOWN, &dclear, "load/gather");
    ID3D12PipelineState *p_sample = make_pso(rsA, vs_near, ps_sample, TRUE, DXGI_FORMAT_UNKNOWN, &dclear, "sample");
    ID3D12PipelineState *p_mask = make_pso(rsA, vs_far, ps_white, TRUE, DXGI_FORMAT_D24_UNORM_S8_UINT, &dmask, "stencil NOT_EQUAL + depth GREATER_EQUAL");
    ID3D12PipelineState *p_cmpA = make_pso(rsA, vs_near, ps_cmp, TRUE, DXGI_FORMAT_UNKNOWN, &dclear, "SampleCmp, static sampler");
    ID3D12PipelineState *p_cmpB = make_pso(rsB, vs_near, ps_cmp, TRUE, DXGI_FORMAT_UNKNOWN, &dclear, "SampleCmp, heap sampler");
    if (!p_fill || !p_mark || !p_load || !p_sample || !p_mask || !p_cmpA || !p_cmpB) goto done;

    ID3D12DescriptorHeap *heaps[2] = { srvh, smph };

    /* ---- 1: fill */
    printf("\n[1] fill: SV_Depth pattern, stencil 0x01, then 0x82 on even columns (discard + REPLACE)\n");
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv_rw, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0x40, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 0, NULL, FALSE, &dsv_rw);
    viewport(1.0f);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rsA);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_SetPipelineState(list, p_fill);
    ID3D12GraphicsCommandList_OMSetStencilRef(list, 0x01);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_SetPipelineState(list, p_mark);
    ID3D12GraphicsCommandList_OMSetStencilRef(list, 0x82);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    barrier(ds, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    /* ---- 2: Load / stencil Load / Gather */
    printf("\n[2] R24_UNORM_X8 Load + Gather, X24_G8_UINT Load(.y)\n");
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    clear_rt(-1.0f);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rsA);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, gpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0));
    ID3D12GraphicsCommandList_SetPipelineState(list, p_load);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    if (read_rt()) {
        UINT bad_d = 0, bad_s = 0, bad_gw = 0, bad_gz = 0, x, y; const float *f0 = NULL;
        for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
            const float *f = px(x, y);
            if (!f0 && (fabsf(f[0] - pat_depth(x, y)) > 1e-5f || f[1] != (float)pat_stencil(x) || fabsf(f[2] - pat_depth(x, y)) > 1e-5f)) f0 = f;
            bad_d += fabsf(f[0] - pat_depth(x, y)) > 1e-5f;
            bad_s += f[1] != (float)pat_stencil(x);
            bad_gw += fabsf(f[2] - pat_depth(x, y)) > 1e-5f;
            if (x + 1 < W) bad_gz += fabsf(f[3] - pat_depth(x + 1, y)) > 1e-5f;
        }
        if (f0) printf("        first bad texel %ld: got (%g, %g, %g, %g)\n", (long)((f0 - out_px) / 4), f0[0], f0[1], f0[2], f0[3]);
        CHECK(!bad_d, "depth Load through R24_UNORM_X8_TYPELESS = the SV_Depth pattern (%u of %u texels wrong; (5,7) = %g, want %g)",
              bad_d, W * H, px(5, 7)[0], pat_depth(5, 7));
        CHECK(!bad_s, "stencil Load(.y) through X24_TYPELESS_G8_UINT = 0x82 even / 0x01 odd columns (%u wrong; (4,0) = %g, (5,0) = %g)",
              bad_s, px(4, 0)[1], px(5, 0)[1]);
        CHECK(!bad_gw, "depth Gather .w = texel (x, y) (%u wrong; (5,7) = %g)", bad_gw, px(5, 7)[2]);
        CHECK(!bad_gz, "depth Gather .z = texel (x + 1, y) (%u wrong; (5,7) = %g, want %g)", bad_gz, px(5, 7)[3], pat_depth(6, 7));
    } else CHECK(0, "load/gather pass read back");

    /* ---- 3: SampleLevel / Sample */
    printf("\n[3] R24_UNORM_X8 SampleLevel / Sample (point, static sampler)\n");
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    viewport(1.0f);
    clear_rt(-1.0f);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rsA);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, gpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0));
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_SetPipelineState(list, p_sample);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    if (read_rt()) {
        UINT bad_l = 0, bad_s = 0, x, y;
        for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
            bad_l += fabsf(px(x, y)[0] - pat_depth(x, y)) > 1e-5f;
            bad_s += fabsf(px(x, y)[1] - pat_depth(x, y)) > 1e-5f;
        }
        CHECK(!bad_l, "depth SampleLevel = the pattern (%u wrong; (5,7) = %g)", bad_l, px(5, 7)[0]);
        CHECK(!bad_s, "depth Sample = the pattern (%u wrong; (5,7) = %g)", bad_s, px(5, 7)[1]);
    } else CHECK(0, "sample pass read back");

    /* ---- 4: stencil + depth masked fullscreen pass, read-only DSV while the SRVs stay bound */
    printf("\n[4] stencil NOT_EQUAL (read mask 2, ref 2) + depth GREATER_EQUAL, VS z = 1, viewport MaxDepth 0.98\n");
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, &dsv_ro);
    viewport(0.98f);
    clear_rt(0.0f);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rsA);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, gpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0));
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_SetPipelineState(list, p_mask);
    ID3D12GraphicsCommandList_OMSetStencilRef(list, 2);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    if (read_rt()) {
        UINT bad = 0, lit = 0, want_lit = 0, x, y;
        for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
            BOOL want = (pat_stencil(x) & 2) != 2 && pat_depth(x, y) <= 0.98f;
            BOOL got = px(x, y)[0] > 0.5f;
            bad += want != got; lit += got; want_lit += want;
        }
        CHECK(!bad, "masked pass drew exactly the odd columns with depth <= 0.98 (%u wrong; %u drawn, %u expected)", bad, lit, want_lit);
    } else CHECK(0, "masked pass read back");

    /* ---- 5: comparison sampling of D16 / D32 shadow maps */
    printf("\n[5] SampleCmp(LevelZero) LESS_EQUAL on D16 and D32 maps cleared to 0.5\n");
    barrier(ds, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv16, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv32, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, NULL);
    barrier(sm16, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barrier(sm32, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (int m = 0; m < 4; m++) {
        const char *what[4] = { "D16, static sampler", "D16, heap sampler", "D32, static sampler", "D32, heap sampler" };
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
        viewport(1.0f);
        clear_rt(-1.0f);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, (m & 1) ? rsB : rsA);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, gpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m < 2 ? 2 : 4));
        if (m & 1) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, gpu_at(smph, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 0));
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_SetPipelineState(list, (m & 1) ? p_cmpB : p_cmpA);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        if (read_rt()) {
            const float *f = px(20, 30);
            CHECK(f[0] == 1.0f && f[1] == 0.0f && f[2] == 1.0f && f[3] == 0.0f,
                  "%s: ref 0.4 -> 1, ref 0.6 -> 0 (LevelZero, implicit LOD): got (%g, %g, %g, %g)", what[m], f[0], f[1], f[2], f[3]);
        } else CHECK(0, "%s: read back", what[m]);
    }

    /* ---- 7: MAXIMUM reduction filter is not a comparison sampler */
    printf("\n[7] D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_POINT (ComparisonFunc LESS) through SampleLevel\n");
    {
        D3D12_SAMPLER_DESC md; ZeroMemory(&md, sizeof md);
        md.Filter = D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_POINT;
        md.AddressU = md.AddressV = md.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        md.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS; md.MaxLOD = D3D12_FLOAT32_MAX; md.MaxAnisotropy = 1;
        ID3D12Device_CreateSampler(dev, &md, cpu_at(smph, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1));
        D3D12_DESCRIPTOR_RANGE r_s0 = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER mp[2]; ZeroMemory(mp, sizeof mp);
        mp[0] = rp[0]; mp[1] = rp[0]; mp[1].DescriptorTable.pDescriptorRanges = &r_s0;
        D3D12_ROOT_SIGNATURE_DESC mrd; ZeroMemory(&mrd, sizeof mrd); mrd.NumParameters = 2; mrd.pParameters = mp;
        ID3D12RootSignature *rsM = make_rs(&mrd);
        ID3D12PipelineState *pm = rsM ? make_pso(rsM, vs_near, ps_sample, TRUE, DXGI_FORMAT_UNKNOWN, &dclear, "sample, MAXIMUM sampler") : NULL;
        if (pm) {
            barrier(ds, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
            viewport(1.0f);
            clear_rt(-1.0f);
            ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
            ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rsM);
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, gpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0));
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, gpu_at(smph, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1));
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D12GraphicsCommandList_SetPipelineState(list, pm);
            ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
            barrier(ds, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            if (read_rt()) {
                UINT bad = 0, x, y;
                for (y = 0; y < H; y++) for (x = 0; x < W; x++) bad += fabsf(px(x, y)[0] - pat_depth(x, y)) > 1e-5f;
                CHECK(!bad, "MAXIMUM point sampler samples the texel (%u wrong; (5,7) = %g, want %g)", bad, px(5, 7)[0], pat_depth(5, 7));
            } else CHECK(0, "MAXIMUM sampler pass read back");
            ID3D12PipelineState_Release(pm);
        }
    }

    /* ---- 8: rect clears */
    printf("\n[8] ClearDepthStencilView with rects: only the rects change\n");
    {
        /* D16 atlas: whole clear to 1.0, then tile t (32x32 quadrant) cleared to 0.2 * (t + 1) by a rect. */
        UINT t, x, y, bad = 0, bad_d = 0, bad_s = 0;
        barrier(sm16, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv16, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
        for (t = 0; t < 3; t++) {   /* the fourth quadrant (bottom right) keeps 1.0 */
            D3D12_RECT rc = { (LONG)(t & 1) * 32, (LONG)(t >> 1) * 32, (LONG)(t & 1) * 32 + 32, (LONG)(t >> 1) * 32 + 32 };
            ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv16, D3D12_CLEAR_FLAG_DEPTH, 0.2f * (float)(t + 1), 0, 1, &rc);
        }
        barrier(sm16, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
        viewport(1.0f);
        clear_rt(-1.0f);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rsA);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, gpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2));
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_SetPipelineState(list, p_sample);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        if (read_rt()) {
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                UINT q = (x >= 32) + 2 * (y >= 32);
                float want = q == 3 ? 1.0f : 0.2f * (float)(q + 1);
                bad += fabsf(px(x, y)[0] - want) > 1.0f / 65535.0f;
            }
            CHECK(!bad, "D16 atlas: three tiles cleared by rects one after another keep 0.2 / 0.4 / 0.6, the fourth 1.0 "
                  "(%u wrong; (5,5) = %g, (40,5) = %g, (5,40) = %g, (40,40) = %g)", bad, px(5, 5)[0], px(40, 5)[0], px(5, 40)[0], px(40, 40)[0]);
        } else CHECK(0, "D16 atlas read back");

        /* R24G8: whole clear (1.0, 0x40); depth-only rect (8,8)-(24,16) to 0.25; stencil-only rect (40,30)-(64,64) to 0x33. */
        {
            D3D12_RECT rd = { 8, 8, 24, 16 }, rs2 = { 40, 30, 70, 64 };
            ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv_rw, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0x40, 0, NULL);
            ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv_rw, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0x99, 1, &rd);
            ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv_rw, D3D12_CLEAR_FLAG_STENCIL, 0.75f, 0x33, 1, &rs2);
            barrier(ds, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
            viewport(1.0f);
            clear_rt(-1.0f);
            ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
            ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rsA);
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, gpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0));
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D12GraphicsCommandList_SetPipelineState(list, p_load);
            ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
            barrier(ds, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            if (read_rt()) {
                for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                    BOOL in_d = x >= 8 && x < 24 && y >= 8 && y < 16, in_s = x >= 40 && y >= 30;
                    bad_d += px(x, y)[0] != (in_d ? 0.25f : 1.0f);
                    bad_s += px(x, y)[1] != (float)(in_s ? 0x33 : 0x40);
                }
                CHECK(!bad_d, "R24G8: a depth-only rect clear sets depth 0.25 inside (8,8)-(24,16) only (%u wrong; (10,10) = %g, (30,10) = %g, (50,50) = %g)",
                      bad_d, px(10, 10)[0], px(30, 10)[0], px(50, 50)[0]);
                CHECK(!bad_s, "R24G8: a stencil-only rect clear (rect past the edge) sets 0x33 inside (40,30)-(64,64) only (%u wrong; (50,50) = %g, (10,10) = %g)",
                      bad_s, px(50, 50)[1], px(10, 10)[1]);
            } else CHECK(0, "R24G8 rect clears read back");
        }
    }

    /* ---- 6: buffers + array slice, SM 5.0 and SM 5.1 */
    {
        ID3D12Resource *lb = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource *tb = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource *ta = NULL, *tup = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 3 * 8 * 256, D3D12_RESOURCE_STATE_GENERIC_READ);
        {   D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = 8; d.Height = 1; d.DepthOrArraySize = 3;
            d.MipLevels = 1; d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; d.SampleDesc.Count = 1;
            ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&ta); }
        CHECK(lb && tb && ta && tup, "case 6 buffers and 8x1x3 RGBA32F array created");
        if (lb && tb && ta && tup) {
            float *f = NULL; UINT32 *u = NULL; UINT i, k, sl;
            ID3D12Resource_Map(lb, 0, NULL, (void **)&f);
            for (i = 0; i < 64; i++) for (k = 0; k < 12; k++) f[i * 12 + k] = buf_val(i, k);   /* 48-byte lights */
            ID3D12Resource_Unmap(lb, 0, NULL);
            ID3D12Resource_Map(tb, 0, NULL, (void **)&u);
            for (i = 0; i < 256; i++) u[i] = i < 20 ? 0xdead : 1000 + i;   /* element e = u[4e..4e+3]; FirstElement 5 */
            ID3D12Resource_Unmap(tb, 0, NULL);
            ID3D12Resource_Map(tup, 0, NULL, (void **)&f);
            for (sl = 0; sl < 3; sl++) for (i = 0; i < 8; i++) for (k = 0; k < 4; k++) f[sl * 64 + i * 4 + k] = (float)(sl * 100 + i * 4 + k);
            ID3D12Resource_Unmap(tup, 0, NULL);
            for (sl = 0; sl < 3; sl++) {
                D3D12_TEXTURE_COPY_LOCATION dl, sl2; ZeroMemory(&dl, sizeof dl); ZeroMemory(&sl2, sizeof sl2);
                dl.pResource = ta; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = sl;
                sl2.pResource = tup; sl2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; sl2.PlacedFootprint.Offset = sl * 256;
                sl2.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sl2.PlacedFootprint.Footprint.Width = 8;
                sl2.PlacedFootprint.Footprint.Height = 1; sl2.PlacedFootprint.Footprint.Depth = 1; sl2.PlacedFootprint.Footprint.RowPitch = 256;
                ID3D12GraphicsCommandList_CopyTextureRegion(list, &dl, 0, 0, 0, &sl2, NULL);
            }
            barrier(ta, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            D3D12_SHADER_RESOURCE_VIEW_DESC bv; ZeroMemory(&bv, sizeof bv);
            bv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; bv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            bv.Format = DXGI_FORMAT_UNKNOWN; bv.Buffer.NumElements = 64; bv.Buffer.StructureByteStride = 48;
            ID3D12Device_CreateShaderResourceView(dev, lb, &bv, cpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8));
            bv.Format = DXGI_FORMAT_R32_TYPELESS; bv.Buffer.StructureByteStride = 0; bv.Buffer.NumElements = 1024; bv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            ID3D12Device_CreateShaderResourceView(dev, lb, &bv, cpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 9));
            bv.Format = DXGI_FORMAT_R32G32B32A32_UINT; bv.Buffer.Flags = 0; bv.Buffer.FirstElement = 5; bv.Buffer.NumElements = 40;
            ID3D12Device_CreateShaderResourceView(dev, tb, &bv, cpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 10));
            D3D12_SHADER_RESOURCE_VIEW_DESC av; ZeroMemory(&av, sizeof av);
            av.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; av.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            av.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; av.Texture2DArray.MipLevels = 1; av.Texture2DArray.ArraySize = 3;
            ID3D12Device_CreateShaderResourceView(dev, ta, &av, cpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 11));
            D3D12_DESCRIPTOR_RANGE r4 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0 };
            D3D12_ROOT_PARAMETER bp; ZeroMemory(&bp, sizeof bp);
            bp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; bp.DescriptorTable.NumDescriptorRanges = 1;
            bp.DescriptorTable.pDescriptorRanges = &r4; bp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_ROOT_SIGNATURE_DESC brd; ZeroMemory(&brd, sizeof brd); brd.NumParameters = 1; brd.pParameters = &bp;
            ID3D12RootSignature *rsC = make_rs(&brd);
            for (int v = 0; v < 2 && rsC; v++) {
                const char *sm = v ? "5.1" : "5.0";
                printf("\n[6.%d] StructuredBuffer(48) / ByteAddressBuffer / Buffer<uint4> FirstElement 5 / Texture2DArray slice 2, SM %s\n", v, sm);
                ID3DBlob *bvs = compile(hlsl_buf, "vs_buf", v ? "vs_5_1" : "vs_5_0"), *bps = compile(hlsl_buf, "ps_buf", v ? "ps_5_1" : "ps_5_0");
                ID3D12PipelineState *pb = (bvs && bps) ? make_pso(rsC, bvs, bps, TRUE, DXGI_FORMAT_UNKNOWN, &dclear, "buffers") : NULL;
                if (pb) {
                    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
                    viewport(1.0f);
                    clear_rt(-1.0f);
                    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
                    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rsC);
                    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, gpu_at(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8));
                    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ID3D12GraphicsCommandList_SetPipelineState(list, pb);
                    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
                    if (read_rt()) {
                        UINT bs = 0, br = 0, bt = 0, ba = 0, x;
                        for (x = 0; x < W; x++) {
                            const float *q = px(x, 9); UINT n = x & 7;
                            bs += q[0] != buf_val(n, 9); br += q[1] != buf_val(n, 9);
                            bt += q[2] != (float)(1000 + 4 * (n + 5) + 2); ba += q[3] != (float)(200 + n * 4 + 3);
                        }
                        CHECK(!bs, "SM %s: StructuredBuffer lights[i].c.y (%u wrong; i = 3: %g, want %g)", sm, bs, px(3, 9)[0], buf_val(3, 9));
                        CHECK(!br, "SM %s: ByteAddressBuffer Load (%u wrong; i = 3: %g, want %g)", sm, br, px(3, 9)[1], buf_val(3, 9));
                        CHECK(!bt, "SM %s: Buffer<uint4>[i].z after FirstElement 5 (%u wrong; i = 3: %g, want %u)", sm, bt, px(3, 9)[2], 1000 + 4 * 8 + 2);
                        CHECK(!ba, "SM %s: Texture2DArray Load slice 2 .w (%u wrong; i = 3: %g, want %u)", sm, ba, px(3, 9)[3], 200 + 15);
                    } else CHECK(0, "SM %s: buffers pass read back", sm);
                    ID3D12PipelineState_Release(pb);
                }
            }
        }
    }

done:
    printf("\n%d checks, %d failures\n", checks, fails);
    fflush(stdout);
    return fails;
}
