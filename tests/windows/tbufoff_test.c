/*  Typed buffer views at any FirstElement, for the wine-lab D3D12 port.
 *
 *  Metal makes a texture-buffer view start on the device's linear-texture
 *  alignment (16 bytes here); the runtime starts it on the boundary below the
 *  view and writes the elements in between into descriptor metadata bits
 *  32..39, the converter's textureViewOffsetInElements. The Metal Shader
 *  Converter never applies that field, so before the AIR rewrite added it
 *  (madeira_air_rewrite.cpp, madeira.cfg tbuf-view-offset) every access through
 *  such a view landed up to 15 bytes early, and GetDimensions counted the
 *  elements in between.
 *
 *  The kernel (tbufoff.hlsl, DXIL in tbufoff_dxil.h) does one access kind per
 *  dispatch and writes what it got to o[t].
 *
 *  Cases:
 *    1  SRVs of 7 formats (1-, 2-, 4-, 8- and 16-byte elements) at
 *       FirstElement 0, 1, 2, 3, 5, 7, 13, 17, 31: 32 elements each read
 *       exactly, with (x, 0, 0, 1) defaults; GetDimensions 32
 *    2  an R32_UINT UAV at FirstElement 0, 1, 3, 5, 13, 17: a store, an atomic
 *       add (it returns the stored value), a load, GetDimensions 16; every
 *       dword of the buffer then holds the result inside the view and its
 *       old value outside
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
#include "tbufoff_dxil.h"

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

enum { SRC_BYTES = 4096, UAV_DWORDS = 256, N_SRV = 32, N_UAV = 16, REGION = 64 * 16, NREG = 4 };
enum { M_SRV_LOAD, M_SRV_DIMS, M_UAV_STORE, M_UAV_ATOMIC, M_UAV_LOAD, M_UAV_DIMS };

static pfn_serialize_v     p_serialize;
static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;
static ID3D12DescriptorHeap *heap;
static UINT                inc;
static ID3D12RootSignature *rs;
static ID3D12PipelineState *pso;
static ID3D12Resource *stage, *src, *ubuf, *outbuf, *rb;

static BYTE src_byte(UINT i) { return (BYTE)(i * 7u + 3u + (i >> 8)); }

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE heap_type, UINT64 size, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = heap_type;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = fl;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **)&r);
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
static void uav_barrier(ID3D12Resource *r) {
    D3D12_RESOURCE_BARRIER b; ZeroMemory(&b, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = r;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

static BOOL submit_and_wait(void) {
    ID3D12CommandList *l[1] = { (ID3D12CommandList *)list };
    HRESULT hr = ID3D12GraphicsCommandList_Close(list);
    if (FAILED(hr)) { printf("        list Close failed (%#lx)\n", hr); return FALSE; }
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, l);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    fence_v++;
    ID3D12Fence_SetEventOnCompletion(fence, fence_v, ev);
    ID3D12CommandQueue_Signal(queue, fence, fence_v);
    DWORD w = WaitForSingleObject(ev, 30000);
    CloseHandle(ev);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    if (w != WAIT_OBJECT_0) { printf("        fence wait timed out\n"); return FALSE; }
    return TRUE;
}

/* b0: 2 constants (n, mode); t0: SRV table (slot 0); u1: UAV table (slot 1); u0: root UAV. */
static ID3D12RootSignature *make_rs(void) {
    D3D12_DESCRIPTOR_RANGE1 r[2]; D3D12_ROOT_PARAMETER1 p[4];
    ZeroMemory(r, sizeof r); ZeroMemory(p, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.Num32BitValues = 2;
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r[0].NumDescriptors = 1; r[0].BaseShaderRegister = 0;
    r[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r[1].NumDescriptors = 1; r[1].BaseShaderRegister = 1;
    r[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1; p[1].DescriptorTable.pDescriptorRanges = &r[0];
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable.NumDescriptorRanges = 1; p[2].DescriptorTable.pDescriptorRanges = &r[1];
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v; ZeroMemory(&v, sizeof v);
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    v.Desc_1_1.NumParameters = 4; v.Desc_1_1.pParameters = p;
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature *o = NULL;
    HRESULT hr = p_serialize(&v, &blob, &err);
    if (err) ID3D10Blob_Release(err);
    if (SUCCEEDED(hr) && blob)
        hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                              &IID_ID3D12RootSignature, (void **)&o);
    if (blob) ID3D10Blob_Release(blob);
    CHECK(SUCCEEDED(hr) && o, "root signature (constants, SRV table, UAV table, root UAV) created (%#lx)", hr);
    return o;
}

static D3D12_CPU_DESCRIPTOR_HANDLE slot(UINT k) {
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &h);
    h.ptr += (SIZE_T)k * inc;
    return h;
}

/* One dispatch of `mode` over n threads into output region `region`. */
static void run(UINT mode, UINT n, UINT region) {
    const UINT c[2] = { n, mode };
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &g);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, 2, c, 0);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, g);
    g.ptr += inc;
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 2, g);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 3, ID3D12Resource_GetGPUVirtualAddress(outbuf) + (UINT64)region * REGION);
    ID3D12GraphicsCommandList_Dispatch(list, (n + 63) / 64, 1, 1);
    uav_barrier(NULL);
}

/* Copies the output regions (and, when `with_ubuf`, ubuf after them) to the readback buffer and submits. */
static const UINT *finish(BOOL with_ubuf) {
    UINT *o = NULL;
    barrier(outbuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, rb, 0, outbuf, 0, (UINT64)NREG * REGION);
    barrier(outbuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (with_ubuf) {
        barrier(ubuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, rb, (UINT64)NREG * REGION, ubuf, 0, UAV_DWORDS * 4);
        barrier(ubuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (!submit_and_wait()) return NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&o);
    return o;
}

struct fmt { DXGI_FORMAT f; const char *name; UINT bytes, nch, chbytes; };
static const struct fmt FMTS[] = {
    { DXGI_FORMAT_R8_UINT, "R8_UINT", 1, 1, 1 },
    { DXGI_FORMAT_R16_UINT, "R16_UINT", 2, 1, 2 },
    { DXGI_FORMAT_R32_UINT, "R32_UINT", 4, 1, 4 },
    { DXGI_FORMAT_R16G16_UINT, "R16G16_UINT", 4, 2, 2 },
    { DXGI_FORMAT_R8G8B8A8_UINT, "R8G8B8A8_UINT", 4, 4, 1 },
    { DXGI_FORMAT_R32G32_UINT, "R32G32_UINT", 8, 2, 4 },
    { DXGI_FORMAT_R32G32B32A32_UINT, "R32G32B32A32_UINT", 16, 4, 4 },
};

static void expect_elem(const struct fmt *f, UINT e, UINT v[4]) {
    UINT c, k;
    v[0] = v[1] = v[2] = 0; v[3] = 1;
    for (c = 0; c < f->nch; c++) {
        UINT x = 0, at = e * f->bytes + c * f->chbytes;
        for (k = 0; k < f->chbytes; k++) x |= (UINT)src_byte(at + k) << (8 * k);
        v[c] = x;
    }
}

static void srv_cases(void) {
    static const UINT firsts[] = { 0, 1, 2, 3, 5, 7, 13, 17, 31 };
    UINT i, j, t;
    printf("\n[1] typed SRVs at any FirstElement: loads and GetDimensions\n");
    for (i = 0; i < sizeof FMTS / sizeof FMTS[0]; i++)
        for (j = 0; j < sizeof firsts / sizeof firsts[0]; j++) {
            const struct fmt *f = &FMTS[i]; UINT first = firsts[j], bad = 0;
            D3D12_SHADER_RESOURCE_VIEW_DESC d; ZeroMemory(&d, sizeof d);
            const UINT *o;
            d.Format = f->f; d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Buffer.FirstElement = first; d.Buffer.NumElements = N_SRV;
            ID3D12Device_CreateShaderResourceView(dev, src, &d, slot(0));
            run(M_SRV_LOAD, N_SRV, 0);
            run(M_SRV_DIMS, 1, 1);
            o = finish(FALSE);
            if (!o) { CHECK(0, "%s FirstElement %u: submitted and read back", f->name, first); continue; }
            for (t = 0; t < N_SRV; t++) {
                UINT v[4];
                expect_elem(f, first + t, v);
                if (memcmp(&o[4 * t], v, 16) && !bad++)
                    printf("        element %u: got (%#x %#x %#x %#x), expected (%#x %#x %#x %#x)\n", t,
                           o[4 * t], o[4 * t + 1], o[4 * t + 2], o[4 * t + 3], v[0], v[1], v[2], v[3]);
            }
            CHECK(!bad && o[REGION / 4] == N_SRV, "%s FirstElement %u (byte %u): %u elements exact (%u wrong), GetDimensions %u",
                  f->name, first, first * f->bytes, N_SRV, bad, o[REGION / 4]);
            ID3D12Resource_Unmap(rb, 0, NULL);
        }
}

static void uav_cases(void) {
    static const UINT firsts[] = { 0, 1, 3, 5, 13, 17 };
    UINT j, t;
    printf("\n[2] a typed R32_UINT UAV at any FirstElement: store, atomic, load, GetDimensions\n");
    for (j = 0; j < sizeof firsts / sizeof firsts[0]; j++) {
        UINT first = firsts[j], bad_atomic = 0, bad_load = 0, bad_mem = 0;
        D3D12_UNORDERED_ACCESS_VIEW_DESC u; ZeroMemory(&u, sizeof u);
        const UINT *o;
        u.Format = DXGI_FORMAT_R32_UINT; u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.FirstElement = first; u.Buffer.NumElements = N_UAV;
        ID3D12Device_CreateUnorderedAccessView(dev, ubuf, NULL, &u, slot(1));
        barrier(ubuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, ubuf, 0, stage, SRC_BYTES, UAV_DWORDS * 4);   /* 0xAAAAAAAA */
        barrier(ubuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        run(M_UAV_STORE, N_UAV, 0);
        run(M_UAV_ATOMIC, N_UAV, 1);
        run(M_UAV_LOAD, N_UAV, 2);
        run(M_UAV_DIMS, 1, 3);
        o = finish(TRUE);
        if (!o) { CHECK(0, "UAV FirstElement %u: submitted and read back", first); continue; }
        for (t = 0; t < N_UAV; t++) {
            if (o[REGION / 4 + 4 * t] != 0x1000u + t && !bad_atomic++)
                printf("        atomic %u returned %#x, expected %#x\n", t, o[REGION / 4 + 4 * t], 0x1000u + t);
            if (o[2 * REGION / 4 + 4 * t] != 0x1005u + t && !bad_load++)
                printf("        load %u returned %#x, expected %#x\n", t, o[2 * REGION / 4 + 4 * t], 0x1005u + t);
        }
        for (t = 0; t < UAV_DWORDS; t++) {
            UINT got = o[NREG * REGION / 4 + t], want = t >= first && t < first + N_UAV ? 0x1005u + (t - first) : 0xAAAAAAAAu;
            if (got != want && !bad_mem++) printf("        buffer dword %u: %#x, expected %#x\n", t, got, want);
        }
        CHECK(!bad_atomic && !bad_load && !bad_mem && o[3 * REGION / 4] == N_UAV,
              "UAV FirstElement %u: atomic (%u wrong), load (%u wrong), buffer (%u wrong), GetDimensions %u",
              first, bad_atomic, bad_load, bad_mem, o[3 * REGION / 4]);
        ID3D12Resource_Unmap(rb, 0, NULL);
    }
}

int main(void) {
    HRESULT hr;
    printf("wine-lab d3d12: typed buffer views at any FirstElement\n\n");
    HMODULE d12 = LoadLibraryA("d3d12.dll");
    pfn_create_device create = d12 ? (pfn_create_device)GetProcAddress(d12, "D3D12CreateDevice") : NULL;
    p_serialize = d12 ? (pfn_serialize_v)GetProcAddress(d12, "D3D12SerializeVersionedRootSignature") : NULL;
    CHECK(create && p_serialize, "d3d12.dll loaded, D3D12CreateDevice and D3D12SerializeVersionedRootSignature resolved");
    if (!create || !p_serialize) goto done;
    pfn_marker marker = (pfn_marker)GetProcAddress(d12, "MadeiraD3D12GetBuildMarker");
    if (marker) printf("        implementation: %s\n", marker());
    hr = create(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev);
    CHECK(SUCCEEDED(hr) && dev, "device created (%#lx)", hr);
    if (!dev) goto done;
    {
        D3D12_COMMAND_QUEUE_DESC qd; ZeroMemory(&qd, sizeof qd);
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (alloc) ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                                  &IID_ID3D12GraphicsCommandList, (void **)&list);
        ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
        D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap);
        inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    CHECK(queue && alloc && list && fence && heap, "queue, allocator, list, fence and descriptor heap created");
    if (!queue || !alloc || !list || !fence || !heap) goto done;
    stage = make_buffer(D3D12_HEAP_TYPE_UPLOAD, SRC_BYTES + UAV_DWORDS * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    src = make_buffer(D3D12_HEAP_TYPE_DEFAULT, SRC_BYTES, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ubuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, UAV_DWORDS * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    outbuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)NREG * REGION, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, (UINT64)NREG * REGION + UAV_DWORDS * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(stage && src && ubuf && outbuf && rb, "buffers created");
    if (!stage || !src || !ubuf || !outbuf || !rb) goto done;
    {
        BYTE *p = NULL; UINT i;
        ID3D12Resource_Map(stage, 0, NULL, (void **)&p);
        if (!p) { CHECK(0, "upload buffer mapped"); goto done; }
        for (i = 0; i < SRC_BYTES; i++) p[i] = src_byte(i);
        memset(p + SRC_BYTES, 0xAA, UAV_DWORDS * 4);
        ID3D12Resource_Unmap(stage, 0, NULL);
    }
    ID3D12GraphicsCommandList_CopyBufferRegion(list, src, 0, stage, 0, SRC_BYTES);
    barrier(src, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    rs = make_rs();
    if (!rs) goto done;
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd; ZeroMemory(&cd, sizeof cd);
        cd.pRootSignature = rs;
        cd.CS.pShaderBytecode = tbufoff_cs_dxil; cd.CS.BytecodeLength = sizeof tbufoff_cs_dxil;
        hr = ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **)&pso);
        CHECK(SUCCEEDED(hr) && pso, "compute PSO created (%#lx)", hr);
        if (!pso) goto done;
    }
    srv_cases();
    uav_cases();

done:
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rb) ID3D12Resource_Release(rb);
    if (outbuf) ID3D12Resource_Release(outbuf);
    if (ubuf) ID3D12Resource_Release(ubuf);
    if (src) ID3D12Resource_Release(src);
    if (stage) ID3D12Resource_Release(stage);
    if (heap) ID3D12DescriptorHeap_Release(heap);
    if (fence) ID3D12Fence_Release(fence);
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (dev) ID3D12Device_Release(dev);
    printf("\n%d checks, %d failures\n", checks, fails);
    fflush(stdout);
    return fails;
}
