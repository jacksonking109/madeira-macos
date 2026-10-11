/*  64-bit atomics whose old value is read, for the wine-lab D3D12 port.
 *
 *  Metal's 64-bit atomics return nothing, and the Metal Shader Converter
 *  refuses a DXIL 64-bit atomic whose result is used. The runtime rewrites such
 *  an atomic (src/unix/madeira_dxil_rewrite.cpp): a plain load of the element gives
 *  the old value, then the atomic runs with its result unused. The Last of Us
 *  Part I's OIT k-buffer insert (InterlockedMax on RWTexture2D<uint64_t>) halts
 *  the game without it.
 *
 *  The DXIL shaders are precompiled by Microsoft's dxc and embedded
 *  (atomic64_dxil.h, from atomic64.hlsl; the regeneration commands are at the
 *  top of atomic64.hlsl).
 *
 *  Each draw is a fullscreen triangle into a 64x64 R32G32_UINT target: one
 *  fragment per pixel, so nothing races. The pixel at (x, y) does one 64-bit
 *  InterlockedMax on the element it owns, initialised to init_value(x, y),
 *  offering new_value(x, y) (a quarter each above / below in the high word, and
 *  above / below in the low word only), and writes the old value it got back.
 *  Every pixel of the target must hold init_value, and every element
 *  max(init_value, new_value).
 *
 *  Cases:
 *    1  RWTexture2D<uint64_t> (R32G32_UINT texture)
 *    2  RWByteAddressBuffer.InterlockedMax64
 *    3  RWStructuredBuffer<uint64_t>
 *    4  RWBuffer<uint64_t> (R32G32_UINT typed buffer)
 *    5  RWTexture3D, RWTexture2DArray, RWTexture1DArray: pipelines created
 *       (conversion only)
 *    6  InterlockedCompareExchange on RWTexture2D<uint64_t> with the old value
 *       used: refused at pipeline creation (the converter crashes on it)
 *    7  contention: 64 x 256 compute threads InterlockedMin (result unused) on
 *       4 addresses of a RWByteAddressBuffer and a RWStructuredBuffer<uint64_t>,
 *       32 rounds each: every element must be a consistent (hi, lo) pair and
 *       the true unsigned 64-bit minimum (a torn, signed or one-word compare
 *       shows); and the float-distance packing hi = asuint(t), lo = id, with
 *       256 distances so equal distances are decided by the id
 *
 *  Exit code = number of failed checks.
 */
#define COBJMACROS
#define INITGUID
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"   /* only test_pso_defaults() is used */
#include "root_sig.h"
#pragma clang diagnostic pop
#include "atomic64_dxil.h"

static int checks, fails;
#define CHECK(cond, ...) do {                                                  \
    checks++;                                                                  \
    if (!(cond)) { fails++; printf("  FAIL  " __VA_ARGS__); printf("\n"); }     \
    else         { printf("  ok    " __VA_ARGS__); printf("\n"); }             \
    fflush(stdout);                                                            \
} while (0)

typedef HRESULT (WINAPI *pfn_create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *pfn_serialize_v)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *, ID3DBlob **, ID3DBlob **);
typedef const char *(*pfn_marker)(void);

enum { W = 64, H = 64, N = W * H, NUAV = 7 };
static const UINT64 RES_SIZE = (UINT64)N * 8;   /* every resource: 64x64 x 8 bytes, row pitch 512 */

static pfn_serialize_v     p_serialize;
static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;
static ID3D12Resource     *rt, *tex, *raw, *sbuf, *tbuf, *up, *rb;
static ID3D12DescriptorHeap *rtvh, *uavh;
static D3D12_CPU_DESCRIPTOR_HANDLE rtv;
static ID3D12RootSignature *rs;

/* atomic64.hlsl's init_value and new_value */
static UINT64 init_value(UINT x, UINT y)
{
    return ((UINT64)(0x01000000u + (y << 12) + x) << 32) | (0x80000000u | (y << 8) | x);
}
static UINT64 new_value(UINT x, UINT y)
{
    UINT64 i = init_value(x, y);
    switch ((x + 2 * y) & 3) {
    case 0: return i + ((UINT64)1 << 32);
    case 1: return i - ((UINT64)1 << 32);
    case 2: return i + 3;
    default: return i - 3;
    }
}

/* ------------------------------------------------------------------ helpers */

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = heap;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = fl;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL,
                                         &IID_ID3D12Resource, (void **)&r);
    return r;
}

static ID3D12Resource *make_tex2d(D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_R32G32_UINT;
    d.SampleDesc.Count = 1; d.Flags = fl;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL,
                                         &IID_ID3D12Resource, (void **)&r);
    return r;
}

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b; ZeroMemory(&b, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

/* Close, execute, wait on the fence, and reopen the list. */
static BOOL submit_and_wait(void) {
    ID3D12CommandList *l[1] = { (ID3D12CommandList *)list };
    HRESULT hr = ID3D12GraphicsCommandList_Close(list);
    if (FAILED(hr)) { printf("        list Close failed (%#lx)\n", hr); return FALSE; }
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, l);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    fence_v++;
    ID3D12Fence_SetEventOnCompletion(fence, fence_v, ev);
    ID3D12CommandQueue_Signal(queue, fence, fence_v);
    DWORD w = WaitForSingleObject(ev, 15000);
    CloseHandle(ev);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    if (w != WAIT_OBJECT_0) { printf("        fence wait timed out\n"); return FALSE; }
    return TRUE;
}

/* A 64x64 R32G32 texture <-> a buffer at offset 0 with row pitch 512. */
static void copy_tex(ID3D12Resource *dst, ID3D12Resource *src, BOOL to_buffer) {
    D3D12_TEXTURE_COPY_LOCATION bl, tl; ZeroMemory(&bl, sizeof bl); ZeroMemory(&tl, sizeof tl);
    bl.pResource = to_buffer ? dst : src; bl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    bl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32_UINT;
    bl.PlacedFootprint.Footprint.Width = W; bl.PlacedFootprint.Footprint.Height = H;
    bl.PlacedFootprint.Footprint.Depth = 1; bl.PlacedFootprint.Footprint.RowPitch = W * 8;
    tl.pResource = to_buffer ? src : dst; tl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if (to_buffer) ID3D12GraphicsCommandList_CopyTextureRegion(list, &bl, 0, 0, 0, &tl, NULL);
    else ID3D12GraphicsCommandList_CopyTextureRegion(list, &tl, 0, 0, 0, &bl, NULL);
}

/* Reads `count` UINT64s out of the readback buffer at element `first`. */
static BOOL read_back(UINT64 *out, UINT first, UINT count) {
    UINT64 *p = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
    if (!p) { printf("        readback Map failed\n"); return FALSE; }
    memcpy(out, p + first, (size_t)count * 8);
    ID3D12Resource_Unmap(rb, 0, NULL);
    return TRUE;
}

/* The four resources start at init_value; all are left in UNORDERED_ACCESS. */
static BOOL init_resources(void) {
    UINT64 *p = NULL;
    UINT x, y;
    ID3D12Resource_Map(up, 0, NULL, (void **)&p);
    if (!p) return FALSE;
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) p[y * W + x] = init_value(x, y);
    ID3D12Resource_Unmap(up, 0, NULL);
    copy_tex(tex, up, FALSE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, raw, 0, up, 0, RES_SIZE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, sbuf, 0, up, 0, RES_SIZE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, tbuf, 0, up, 0, RES_SIZE);
    barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(raw, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(sbuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(tbuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return submit_and_wait();
}

/* u0 texture, u1 raw, u2 structured, u3 typed; u4-u6 null views of the
 * conversion-only shapes. */
static void make_views(void) {
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c;
    D3D12_UNORDERED_ACCESS_VIEW_DESC u;
    uavh->lpVtbl->GetCPUDescriptorHandleForHeapStart(uavh, &c);

    ZeroMemory(&u, sizeof u);
    u.Format = DXGI_FORMAT_R32G32_UINT; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateUnorderedAccessView(dev, tex, NULL, &u, c); c.ptr += inc;
    ZeroMemory(&u, sizeof u);
    u.Format = DXGI_FORMAT_R32_TYPELESS; u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = N * 2; u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(dev, raw, NULL, &u, c); c.ptr += inc;
    ZeroMemory(&u, sizeof u);
    u.Format = DXGI_FORMAT_UNKNOWN; u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = N; u.Buffer.StructureByteStride = 8;
    ID3D12Device_CreateUnorderedAccessView(dev, sbuf, NULL, &u, c); c.ptr += inc;
    ZeroMemory(&u, sizeof u);
    u.Format = DXGI_FORMAT_R32G32_UINT; u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = N;
    ID3D12Device_CreateUnorderedAccessView(dev, tbuf, NULL, &u, c); c.ptr += inc;
    ZeroMemory(&u, sizeof u);
    u.Format = DXGI_FORMAT_R32G32_UINT; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; u.Texture3D.WSize = 1;
    ID3D12Device_CreateUnorderedAccessView(dev, NULL, NULL, &u, c); c.ptr += inc;
    ZeroMemory(&u, sizeof u);
    u.Format = DXGI_FORMAT_R32G32_UINT; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; u.Texture2DArray.ArraySize = 1;
    ID3D12Device_CreateUnorderedAccessView(dev, NULL, NULL, &u, c); c.ptr += inc;
    ZeroMemory(&u, sizeof u);
    u.Format = DXGI_FORMAT_R32G32_UINT; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY; u.Texture1DArray.ArraySize = 1;
    ID3D12Device_CreateUnorderedAccessView(dev, NULL, NULL, &u, c);
}

/* One descriptor table: UAVs u0-u6, pixel stage. */
static ID3D12RootSignature *make_rs(void) {
    D3D12_DESCRIPTOR_RANGE1 r; ZeroMemory(&r, sizeof r);
    r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r.NumDescriptors = NUAV;
    D3D12_ROOT_PARAMETER1 p; ZeroMemory(&p, sizeof p);
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = 1; p.DescriptorTable.pDescriptorRanges = &r;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v; ZeroMemory(&v, sizeof v);
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    v.Desc_1_1.NumParameters = 1; v.Desc_1_1.pParameters = &p;
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature *r_out = NULL;
    HRESULT hr = p_serialize(&v, &blob, &err);
    if (err) ID3D10Blob_Release(err);
    if (SUCCEEDED(hr) && blob)
        hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                              &IID_ID3D12RootSignature, (void **)&r_out);
    if (blob) ID3D10Blob_Release(blob);
    CHECK(SUCCEEDED(hr) && r_out, "root signature with UAVs u0-u%d created (%#lx)", NUAV - 1, hr);
    return r_out;
}

static HRESULT make_pso(const void *ps, SIZE_T nps, ID3D12PipelineState **pso) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = a64_vs_dxil; pd.VS.BytecodeLength = sizeof a64_vs_dxil;
    pd.PS.pShaderBytecode = ps; pd.PS.BytecodeLength = nps;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R32G32_UINT;
    pd.SampleDesc.Count = 1;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    test_pso_defaults(&pd, FALSE);
    *pso = NULL;
    return ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)pso);
}

/* ------------------------------------------------------------- draw cases */

/* Draws `ps`, then checks the target (old values) and `res` (what is left). */
static void case_draw(int num, const char *what, const void *ps, SIZE_T nps, ID3D12Resource *res, BOOL is_tex) {
    static UINT64 olds[N], left[N];
    ID3D12PipelineState *pso = NULL;
    HRESULT hr;
    UINT i, bad_old = 0, bad_left = 0, first_old = N, first_left = N;
    printf("\n[%d] %s\n", num, what);

    hr = make_pso(ps, nps, &pso);
    CHECK(SUCCEEDED(hr) && pso, "%s: graphics PSO created (%#lx)", what, hr);
    if (!pso) return;

    D3D12_VIEWPORT vp; ZeroMemory(&vp, sizeof vp);
    vp.Width = (FLOAT)W; vp.Height = (FLOAT)H; vp.MaxDepth = 1.0f;
    D3D12_RECT sc = { 0, 0, W, H };
    const FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &uavh);
    {
        D3D12_GPU_DESCRIPTOR_HANDLE g;
        uavh->lpVtbl->GetGPUDescriptorHandleForHeapStart(uavh, &g);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, g);
    }
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);

    /* target -> rb[0, N), resource -> rb[N, 2N) */
    barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_tex(rb, rt, TRUE);
    barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (is_tex) {
        D3D12_TEXTURE_COPY_LOCATION bl, tl; ZeroMemory(&bl, sizeof bl); ZeroMemory(&tl, sizeof tl);
        bl.pResource = rb; bl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; bl.PlacedFootprint.Offset = RES_SIZE;
        bl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32_UINT;
        bl.PlacedFootprint.Footprint.Width = W; bl.PlacedFootprint.Footprint.Height = H;
        bl.PlacedFootprint.Footprint.Depth = 1; bl.PlacedFootprint.Footprint.RowPitch = W * 8;
        tl.pResource = res; tl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &bl, 0, 0, 0, &tl, NULL);
    } else {
        ID3D12GraphicsCommandList_CopyBufferRegion(list, rb, RES_SIZE, res, 0, RES_SIZE);
    }
    barrier(res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!submit_and_wait() || !read_back(olds, 0, N) || !read_back(left, N, N)) {
        CHECK(0, "%s: draw submitted and read back", what);
        ID3D12PipelineState_Release(pso);
        return;
    }
    for (i = 0; i < N; i++) {
        UINT x = i % W, y = i / W;
        UINT64 a = init_value(x, y), b = new_value(x, y), m = a > b ? a : b;
        if (olds[i] != a && !bad_old++) first_old = i;
        if (left[i] != m && !bad_left++) first_left = i;
    }
#define XY(i) (i) % W, (i) / W
    CHECK(!bad_old, "%s: returned old values exact (%u of %u wrong%s)", what, bad_old, N, bad_old ? "; see below" : "");
    if (bad_old)
        printf("        (%u,%u): got %#llx, expected %#llx\n", XY(first_old),
               (unsigned long long)olds[first_old], (unsigned long long)init_value(XY(first_old)));
    CHECK(!bad_left, "%s: elements hold max(init, new) (%u of %u wrong%s)", what, bad_left, N, bad_left ? "; see below" : "");
    if (bad_left) {
        UINT64 a = init_value(XY(first_left)), b = new_value(XY(first_left));
        printf("        (%u,%u): got %#llx, expected %#llx\n", XY(first_left),
               (unsigned long long)left[first_left], (unsigned long long)(a > b ? a : b));
    }
#undef XY
    ID3D12PipelineState_Release(pso);
}

/* ------------------------------------------------------- case 7: contention */

enum { MIN_SLOTS = 4, MIN_GROUPS = 256, MIN_THREADS = MIN_GROUPS * 64, MIN_ROUNDS = 32 };

/* atomic64.hlsl's hash32 and min_pair, and cs_min_float's value */
static UINT hash32(UINT x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
static UINT64 min_pair(UINT t) {
    UINT hi = hash32(t);
    return ((UINT64)hi << 32) | ((hi * 2654435761u) ^ 0xA5A5A5A5u);
}
static UINT64 min_float_pair(UINT t) {
    float d = 0.25f + (float)(hash32(t) & 0xFFu) * (1.0f / 4096.0f);   /* exact: no rounding to differ */
    UINT bits;
    memcpy(&bits, &d, 4);
    return ((UINT64)bits << 32) | t;
}

/* Root UAVs u0 and u1 in space 1. */
static ID3D12RootSignature *make_cs_rs(void) {
    D3D12_ROOT_PARAMETER1 p[2]; ZeroMemory(p, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[0].Descriptor.RegisterSpace = 1;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[1].Descriptor.ShaderRegister = 1; p[1].Descriptor.RegisterSpace = 1;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v; ZeroMemory(&v, sizeof v);
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    v.Desc_1_1.NumParameters = 2; v.Desc_1_1.pParameters = p;
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature *r = NULL;
    HRESULT hr = p_serialize(&v, &blob, &err);
    if (err) ID3D10Blob_Release(err);
    if (SUCCEEDED(hr) && blob)
        hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                              &IID_ID3D12RootSignature, (void **)&r);
    if (blob) ID3D10Blob_Release(blob);
    CHECK(SUCCEEDED(hr) && r, "compute root signature (root UAVs u0, u1 space 1) created (%#lx)", hr);
    return r;
}

static void case_min(ID3D12RootSignature *crs, const char *what, const void *cs, SIZE_T ncs,
                     ID3D12Resource *res, UINT64 (*pair)(UINT)) {
    ID3D12PipelineState *pso = NULL;
    UINT64 want[MIN_SLOTS], got[MIN_SLOTS];
    UINT t, round, k, torn = 0, wrong = 0;
    HRESULT hr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd; ZeroMemory(&cd, sizeof cd);
    cd.pRootSignature = crs;
    cd.CS.pShaderBytecode = cs; cd.CS.BytecodeLength = ncs;
    hr = ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **)&pso);
    CHECK(SUCCEEDED(hr) && pso, "%s: compute PSO created (%#lx)", what, hr);
    if (!pso) return;

    for (k = 0; k < MIN_SLOTS; k++) want[k] = ~(UINT64)0;
    for (t = 0; t < MIN_THREADS; t++) { UINT64 v = pair(t); if (v < want[t % MIN_SLOTS]) want[t % MIN_SLOTS] = v; }

    for (round = 0; round < MIN_ROUNDS; round++) {
        UINT64 *p = NULL;
        ID3D12Resource_Map(up, 0, NULL, (void **)&p);
        if (!p) break;
        for (k = 0; k < MIN_SLOTS; k++) p[k] = ~(UINT64)0;
        ID3D12Resource_Unmap(up, 0, NULL);
        barrier(res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, res, 0, up, 0, MIN_SLOTS * 8);
        barrier(res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12GraphicsCommandList_SetComputeRootSignature(list, crs);
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 0, ID3D12Resource_GetGPUVirtualAddress(raw));
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 1, ID3D12Resource_GetGPUVirtualAddress(sbuf));
        ID3D12GraphicsCommandList_Dispatch(list, MIN_GROUPS, 1, 1);
        barrier(res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, rb, 0, res, 0, MIN_SLOTS * 8);
        barrier(res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!submit_and_wait() || !read_back(got, 0, MIN_SLOTS)) break;
        for (k = 0; k < MIN_SLOTS; k++) {
            /* consistent: the pair a real thread of this slot offered */
            UINT owner = pair == min_pair ? 0 : (UINT)got[k];
            BOOL whole = pair == min_pair ? ((UINT)got[k] == (((UINT)(got[k] >> 32) * 2654435761u) ^ 0xA5A5A5A5u))
                                          : (owner < MIN_THREADS && owner % MIN_SLOTS == k && pair(owner) == got[k]);
            if (!whole && !torn++)
                printf("        round %u slot %u: %#llx is not a pair any thread offered (torn)\n", round, k,
                       (unsigned long long)got[k]);
            if (got[k] != want[k] && !wrong++)
                printf("        round %u slot %u: got %#llx, expected the minimum %#llx\n", round, k,
                       (unsigned long long)got[k], (unsigned long long)want[k]);
        }
    }
    CHECK(round == MIN_ROUNDS, "%s: %u rounds of %u threads on %u addresses ran", what, round, MIN_THREADS, MIN_SLOTS);
    CHECK(!torn, "%s: every result a consistent pair (%u torn)", what, torn);
    CHECK(!wrong, "%s: every result the unsigned 64-bit minimum (%u wrong)", what, wrong);
    ID3D12PipelineState_Release(pso);
}

/* --------------------------------------------------------------------- main */

int main(void) {
    HRESULT hr;
    printf("wine-lab d3d12: 64-bit atomics whose old value is read\n\n");

    HMODULE d12 = LoadLibraryA("d3d12.dll");
    pfn_create_device create = d12 ? (pfn_create_device)GetProcAddress(d12, "D3D12CreateDevice") : NULL;
    p_serialize = d12 ? (pfn_serialize_v)GetProcAddress(d12, "D3D12SerializeVersionedRootSignature") : NULL;
    CHECK(create && p_serialize, "d3d12.dll loaded, device creation and root signature serializer resolved");
    if (!create || !p_serialize) goto done;
    pfn_marker marker = (pfn_marker)GetProcAddress(d12, "MadeiraD3D12GetBuildMarker");
    if (marker) printf("        implementation: %s\n", marker());

    hr = create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    CHECK(SUCCEEDED(hr) && dev, "device created (%#lx)", hr);
    if (!dev) goto done;
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9; ZeroMemory(&o9, sizeof o9);
        hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof o9);
        CHECK(SUCCEEDED(hr) && o9.AtomicInt64OnTypedResourceSupported,
              "OPTIONS9 claims 64-bit atomics on typed resources (%#lx)", hr);
    }

    D3D12_COMMAND_QUEUE_DESC qd; ZeroMemory(&qd, sizeof qd);
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
    if (alloc)
        ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                       &IID_ID3D12GraphicsCommandList, (void **)&list);
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    CHECK(queue && alloc && list && fence, "queue, allocator, list and fence created");
    if (!queue || !alloc || !list || !fence) goto done;

    rt = make_tex2d(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    tex = make_tex2d(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    raw = make_buffer(D3D12_HEAP_TYPE_DEFAULT, RES_SIZE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    sbuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, RES_SIZE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    tbuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, RES_SIZE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    up = make_buffer(D3D12_HEAP_TYPE_UPLOAD, RES_SIZE, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, 2 * RES_SIZE, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = NUAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&uavh);
    }
    CHECK(rt && tex && raw && sbuf && tbuf && up && rb && rtvh && uavh,
          "64x64 R32G32_UINT target and texture, three 32 KB buffers, upload/readback buffers and heaps created");
    if (!rt || !tex || !raw || !sbuf || !tbuf || !up || !rb || !rtvh || !uavh) goto done;
    rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    make_views();
    CHECK(init_resources(), "texture and buffers initialised to init_value");
    rs = make_rs();
    if (!rs) goto done;

    case_draw(1, "RWTexture2D<uint64_t> InterlockedMax", a64_ps_tex_dxil, sizeof a64_ps_tex_dxil, tex, TRUE);
    case_draw(2, "RWByteAddressBuffer InterlockedMax64", a64_ps_raw_dxil, sizeof a64_ps_raw_dxil, raw, FALSE);
    case_draw(3, "RWStructuredBuffer<uint64_t> InterlockedMax", a64_ps_struct_dxil, sizeof a64_ps_struct_dxil, sbuf, FALSE);
    case_draw(4, "RWBuffer<uint64_t> InterlockedMax", a64_ps_typed_dxil, sizeof a64_ps_typed_dxil, tbuf, FALSE);

    printf("\n[5] other texture shapes (conversion only)\n");
    {
        static const struct { const char *name; const unsigned char *ps; SIZE_T n; } shapes[3] = {
            { "RWTexture3D<uint64_t>", a64_ps_tex3d_dxil, sizeof a64_ps_tex3d_dxil },
            { "RWTexture2DArray<uint64_t>", a64_ps_tex2darray_dxil, sizeof a64_ps_tex2darray_dxil },
            { "RWTexture1DArray<uint64_t>", a64_ps_tex1darray_dxil, sizeof a64_ps_tex1darray_dxil },
        };
        int k;
        for (k = 0; k < 3; k++) {
            ID3D12PipelineState *pso = NULL;
            hr = make_pso(shapes[k].ps, shapes[k].n, &pso);
            CHECK(SUCCEEDED(hr) && pso, "%s InterlockedMax: graphics PSO created (%#lx)", shapes[k].name, hr);
            if (pso) ID3D12PipelineState_Release(pso);
        }
    }

    printf("\n[6] 64-bit compare-exchange on a texture, old value used\n");
    {
        ID3D12PipelineState *pso = NULL;
        hr = make_pso(a64_ps_cmpxchg_dxil, sizeof a64_ps_cmpxchg_dxil, &pso);
        CHECK(FAILED(hr) && !pso, "RWTexture2D<uint64_t> InterlockedCompareExchange: refused at pipeline creation (%#lx)", hr);
        if (pso) ID3D12PipelineState_Release(pso);
    }

    printf("\n[7] 64-bit InterlockedMin under contention, result unused\n");
    {
        ID3D12RootSignature *crs = make_cs_rs();
        if (crs) {
            case_min(crs, "RWByteAddressBuffer InterlockedMin64", a64_cs_min_raw_dxil, sizeof a64_cs_min_raw_dxil,
                     raw, min_pair);
            case_min(crs, "RWStructuredBuffer<uint64_t> InterlockedMin", a64_cs_min_struct_dxil,
                     sizeof a64_cs_min_struct_dxil, sbuf, min_pair);
            case_min(crs, "RWByteAddressBuffer InterlockedMin64, asuint(distance) | id", a64_cs_min_float_dxil,
                     sizeof a64_cs_min_float_dxil, raw, min_float_pair);
            ID3D12RootSignature_Release(crs);
        }
    }

    ID3D12RootSignature_Release(rs);
done:
    if (uavh) ID3D12DescriptorHeap_Release(uavh);
    if (rtvh) ID3D12DescriptorHeap_Release(rtvh);
    if (rb) ID3D12Resource_Release(rb);
    if (up) ID3D12Resource_Release(up);
    if (tbuf) ID3D12Resource_Release(tbuf);
    if (sbuf) ID3D12Resource_Release(sbuf);
    if (raw) ID3D12Resource_Release(raw);
    if (tex) ID3D12Resource_Release(tex);
    if (rt) ID3D12Resource_Release(rt);
    if (fence) ID3D12Fence_Release(fence);
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (dev) ID3D12Device_Release(dev);
    printf("\n%d checks, %d failures\n", checks, fails);
    fflush(stdout);
    return fails;
}
