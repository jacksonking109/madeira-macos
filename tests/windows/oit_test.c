/*  Order-independent transparency k-buffer under contention, for the wine-lab
 *  D3D12 port.
 *
 *  The Last of Us Part I's MainPsOitAppend inserts each translucent fragment
 *  into a k-buffer of four RWTexture2D<uint64_t> layers (R32G32_UINT): 64-bit
 *  InterlockedMax returning the OLD value, the smaller of old and new carried
 *  to the next layer, stop at an empty (0) slot. The Metal Shader Converter
 *  refuses a 64-bit atomic whose result is used, so the runtime rewrites each
 *  one into "load the texel (R = lo, G = hi) + DeviceMemoryBarrier + atomic max
 *  without return" (src/unix/madeira_dxil_rewrite.cpp). Two fragments of one
 *  pixel can both load the same old value between the load and the atomic: a
 *  value can be lost or duplicated, but never invented, and the layers stay
 *  sorted. This test pins that down, and that the k-buffer clear (a 64-bit
 *  pattern through ClearUnorderedAccessViewUint on an R32G32_UINT view) takes.
 *
 *  oit.hlsl: every instance is a fullscreen triangle, so each of the 32x32
 *  pixels gets NINST racing fragments; fragment (inst, x, y) offers
 *  hi = key_hash(inst * 4096 + y * 32 + x), lo = inst << 20 | y << 10 | x.
 *
 *  Cases:
 *    1  ClearUnorderedAccessViewUint {0xDEADBEEF, 0x0BADF00D} on all four
 *       layers: every texel reads 0x0BADF00D_DEADBEEF (R = low word)
 *    2  rounds of: stale data copied into the layers, the zero clear, a UAV
 *       barrier and NINST instances, all in ONE command list (1, 4, 16, 64,
 *       256, 256, 256 instances). Per pixel, FAILS on: a nonzero value that is
 *       not one of that pixel's inputs (garbage, stale data, torn or swapped
 *       words); layers not sorted (l0 >= l1 >= l2 >= l3, empties last); layer
 *       0 not the true maximum (every fragment reaches layer 0, so the race
 *       cannot lose it); with 1 instance, anything but (v, 0, 0, 0).
 *       REPORTS (not failures): pixels whose layers are exactly the true top 4,
 *       values lost to the race, duplicated values.
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
#include "oit_dxil.h"

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

enum { W = 32, H = 32, N = W * H, K = 4, PITCH = W * 8, LAYER_BYTES = PITCH * H, MAXINST = 256 };

static pfn_serialize_v     p_serialize;
static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;
static ID3D12Resource     *layer[K], *rt, *up, *rb;
static ID3D12DescriptorHeap *rtvh, *uavh, *cpuh;
static D3D12_CPU_DESCRIPTOR_HANDLE rtv;
static ID3D12RootSignature *rs;
static ID3D12PipelineState *pso;

static UINT key_hash(UINT x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x | 1u;
}
static UINT64 frag_value(UINT inst, UINT x, UINT y) {
    return ((UINT64)key_hash(inst * 4096u + y * W + x) << 32) | ((inst << 20) | (y << 10) | x);
}

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = heap;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}

static ID3D12Resource *make_tex2d(DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt;
    d.SampleDesc.Count = 1; d.Flags = fl;
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
static void uav_barrier_all(void) {
    D3D12_RESOURCE_BARRIER b; ZeroMemory(&b, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
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
    DWORD w = WaitForSingleObject(ev, 15000);
    CloseHandle(ev);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    if (w != WAIT_OBJECT_0) { printf("        fence wait timed out\n"); return FALSE; }
    return TRUE;
}

/* Layer `i` <-> buffer `buf` at i * LAYER_BYTES. */
static void copy_layer(ID3D12Resource *buf, UINT i, BOOL to_buffer) {
    D3D12_TEXTURE_COPY_LOCATION bl, tl; ZeroMemory(&bl, sizeof bl); ZeroMemory(&tl, sizeof tl);
    bl.pResource = buf; bl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    bl.PlacedFootprint.Offset = (UINT64)i * LAYER_BYTES;
    bl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32_UINT;
    bl.PlacedFootprint.Footprint.Width = W; bl.PlacedFootprint.Footprint.Height = H;
    bl.PlacedFootprint.Footprint.Depth = 1; bl.PlacedFootprint.Footprint.RowPitch = PITCH;
    tl.pResource = layer[i]; tl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if (to_buffer) ID3D12GraphicsCommandList_CopyTextureRegion(list, &bl, 0, 0, 0, &tl, NULL);
    else ID3D12GraphicsCommandList_CopyTextureRegion(list, &tl, 0, 0, 0, &bl, NULL);
}

/* All four layers (in UNORDERED_ACCESS) into the readback buffer; they are left in UNORDERED_ACCESS. */
static void record_readback(void) {
    UINT i;
    for (i = 0; i < K; i++) barrier(layer[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (i = 0; i < K; i++) copy_layer(rb, i, TRUE);
    for (i = 0; i < K; i++) barrier(layer[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

static void clear_layers(UINT lo, UINT hi) {
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV), i;
    const UINT v[4] = { lo, hi, 0, 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE c; D3D12_GPU_DESCRIPTOR_HANDLE g;
    cpuh->lpVtbl->GetCPUDescriptorHandleForHeapStart(cpuh, &c);
    uavh->lpVtbl->GetGPUDescriptorHandleForHeapStart(uavh, &g);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &uavh);
    for (i = 0; i < K; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE ci = c; D3D12_GPU_DESCRIPTOR_HANDLE gi = g;
        ci.ptr += (SIZE_T)i * inc; gi.ptr += (UINT64)i * inc;
        ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, gi, ci, layer[i], v, 0, NULL);
    }
}

static UINT64 *map_rb(void) {
    UINT64 *p = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
    return p;
}

/* ----------------------------------------------------------------- cases */

static void case_clear_pattern(void) {
    UINT64 *p; UINT i, bad = 0; UINT64 first = 0;
    printf("\n[1] 64-bit pattern clear of the four R32G32_UINT layers\n");
    clear_layers(0xDEADBEEFu, 0x0BADF00Du);
    uav_barrier_all();
    record_readback();
    if (!submit_and_wait()) { CHECK(0, "pattern clear: submitted"); return; }
    p = map_rb();
    if (!p) { CHECK(0, "pattern clear: readback mapped"); return; }
    for (i = 0; i < K * N; i++) if (p[i] != 0x0BADF00DDEADBEEFull) { if (!bad) first = p[i]; bad++; }
    ID3D12Resource_Unmap(rb, 0, NULL);
    CHECK(bad == 0, "every texel of every layer is 0x0BADF00D_DEADBEEF (%u/%u wrong, first %#llx)",
          bad, K * N, (unsigned long long)first);
}

static int cmp_desc(const void *a, const void *b) {
    UINT64 x = *(const UINT64 *)a, y = *(const UINT64 *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void case_round(int round, UINT ninst) {
    static UINT64 offered[MAXINST];
    UINT64 *p; UINT x, y, i, k;
    UINT garbage = 0, unsorted = 0, notmax = 0, single_bad = 0, exact = 0, lost = 0, dups = 0, filled[K] = { 0 };
    UINT64 first_garbage = 0;
    printf("\n[2.%d] stale data, zero clear, %u racing instances, one command list\n", round, ninst);

    for (i = 0; i < K; i++) barrier(layer[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    for (i = 0; i < K; i++) copy_layer(up, i, FALSE);   /* stale k-buffer contents from "last frame" */
    for (i = 0; i < K; i++) barrier(layer[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    clear_layers(0, 0);
    uav_barrier_all();
    {
        D3D12_VIEWPORT vp; ZeroMemory(&vp, sizeof vp);
        D3D12_RECT sc = { 0, 0, W, H };
        D3D12_GPU_DESCRIPTOR_HANDLE g;
        vp.Width = (FLOAT)W; vp.Height = (FLOAT)H; vp.MaxDepth = 1.0f;
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &uavh);
        uavh->lpVtbl->GetGPUDescriptorHandleForHeapStart(uavh, &g);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, g);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, ninst, 0, 0);
    }
    uav_barrier_all();
    record_readback();
    if (!submit_and_wait()) { CHECK(0, "round %d: submitted", round); return; }
    p = map_rb();
    if (!p) { CHECK(0, "round %d: readback mapped", round); return; }

    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            UINT64 v[K];
            int pix_exact = 1;
            for (k = 0; k < K; k++) v[k] = p[(UINT64)k * N + y * W + x];
            for (i = 0; i < ninst; i++) offered[i] = frag_value(i, x, y);
            qsort(offered, ninst, sizeof offered[0], cmp_desc);
            for (k = 0; k < K; k++) {
                if (v[k]) {
                    UINT lo = (UINT)v[k], inst = lo >> 20;
                    filled[k]++;
                    if (inst >= ninst || ((lo >> 10) & 1023) != y || (lo & 1023) != x || v[k] != frag_value(inst, x, y)) {
                        if (!garbage) first_garbage = v[k];
                        garbage++;
                    }
                }
                if (k && v[k] > v[k - 1]) unsorted++;
                if (v[k] != (k < ninst ? offered[k] : 0)) pix_exact = 0;
                if (k && v[k] && v[k] == v[k - 1]) dups++;
            }
            if (v[0] != offered[0]) notmax++;
            if (ninst == 1 && (v[0] != offered[0] || v[1] || v[2] || v[3])) single_bad++;
            if (pix_exact) exact++;
            if (x == 5 && y == 3 && ninst <= K) {   /* one pixel in full, for diagnosis */
                printf("        info: pixel (5,3): layers");
                for (k = 0; k < K; k++) printf(" %u", v[k] ? (UINT)v[k] >> 20 : 999u);
                printf(" | true order");
                for (k = 0; k < ninst; k++) printf(" %u", (UINT)offered[k] >> 20);
                printf("  (instance ids, 999 = empty)\n");
            }
            for (k = 0; k < K && k < ninst; k++) {   /* true top-k values missing from the layers */
                UINT j, found = 0;
                for (j = 0; j < K; j++) if (v[j] == offered[k]) found = 1;
                if (!found) lost++;
            }
        }
    ID3D12Resource_Unmap(rb, 0, NULL);

    CHECK(garbage == 0, "round %d: every stored value is one of its pixel's inputs (%u garbage, first %#llx)",
          round, garbage, (unsigned long long)first_garbage);
    CHECK(unsorted == 0, "round %d: layers sorted, empties last (%u inversions)", round, unsorted);
    CHECK(notmax == 0, "round %d: layer 0 holds the true maximum (%u pixels wrong)", round, notmax);
    if (ninst == 1) CHECK(single_bad == 0, "round %d: one fragment per pixel lands exactly (v, 0, 0, 0) (%u wrong)", round, single_bad);
    printf("        info: exact top-%u in %u/%u pixels; %u true top-%u values lost, %u duplicated; filled per layer %u %u %u %u\n",
           ninst < K ? ninst : K, exact, N, lost, ninst < K ? ninst : K, dups, filled[0], filled[1], filled[2], filled[3]);
}

/* ------------------------------------------------------------------ main */

int main(void) {
    static const UINT rounds[] = { 1, 4, 16, 64, 256, 256, 256 };
    HRESULT hr;
    UINT i;
    printf("wine-lab d3d12: OIT k-buffer insert (64-bit InterlockedMax with the old value) under contention\n");

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
        D3D12_COMMAND_QUEUE_DESC qd; ZeroMemory(&qd, sizeof qd);
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (alloc)
            ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                           &IID_ID3D12GraphicsCommandList, (void **)&list);
        ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    }
    CHECK(queue && alloc && list && fence, "queue, allocator, list and fence created");
    if (!queue || !alloc || !list || !fence) goto done;

    for (i = 0; i < K; i++)
        layer[i] = make_tex2d(DXGI_FORMAT_R32G32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    rt = make_tex2d(DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    up = make_buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)K * LAYER_BYTES, D3D12_RESOURCE_STATE_GENERIC_READ);
    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, (UINT64)K * LAYER_BYTES, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(layer[0] && layer[1] && layer[2] && layer[3] && rt && up && rb, "k-buffer layers, target and buffers created");
    if (!layer[0] || !layer[1] || !layer[2] || !layer[3] || !rt || !up || !rb) goto done;
    {   /* stale contents: large values that are nobody's input and would win every max */
        UINT64 *s = NULL;
        ID3D12Resource_Map(up, 0, NULL, (void **)&s);
        if (s) { for (i = 0; i < K * N; i++) s[i] = 0xFFFFFFF0FFFFFFF0ull - i; ID3D12Resource_Unmap(up, 0, NULL); }
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
        D3D12_UNORDERED_ACCESS_VIEW_DESC u; ZeroMemory(&u, sizeof u);
        D3D12_CPU_DESCRIPTOR_HANDLE c, gc;
        UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = K;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&cpuh);
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&uavh);
        if (!rtvh || !cpuh || !uavh) { CHECK(0, "descriptor heaps created"); goto done; }
        rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv);
        ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
        cpuh->lpVtbl->GetCPUDescriptorHandleForHeapStart(cpuh, &c);
        uavh->lpVtbl->GetCPUDescriptorHandleForHeapStart(uavh, &gc);
        u.Format = DXGI_FORMAT_R32G32_UINT; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (i = 0; i < K; i++) {
            ID3D12Device_CreateUnorderedAccessView(dev, layer[i], NULL, &u, c);
            ID3D12Device_CreateUnorderedAccessView(dev, layer[i], NULL, &u, gc);
            c.ptr += inc; gc.ptr += inc;
        }
    }
    {   /* one descriptor table: UAVs u0-u3, pixel stage */
        D3D12_DESCRIPTOR_RANGE1 r; ZeroMemory(&r, sizeof r);
        D3D12_ROOT_PARAMETER1 prm; ZeroMemory(&prm, sizeof prm);
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC v; ZeroMemory(&v, sizeof v);
        ID3DBlob *blob = NULL, *err = NULL;
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r.NumDescriptors = K;
        prm.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        prm.DescriptorTable.NumDescriptorRanges = 1; prm.DescriptorTable.pDescriptorRanges = &r;
        prm.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        v.Desc_1_1.NumParameters = 1; v.Desc_1_1.pParameters = &prm;
        hr = p_serialize(&v, &blob, &err);
        if (err) ID3D10Blob_Release(err);
        if (SUCCEEDED(hr) && blob)
            hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                                  &IID_ID3D12RootSignature, (void **)&rs);
        if (blob) ID3D10Blob_Release(blob);
        CHECK(SUCCEEDED(hr) && rs, "root signature with UAVs u0-u3 created (%#lx)", hr);
        if (!rs) goto done;
    }
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
        pd.pRootSignature = rs;
        pd.VS.pShaderBytecode = oit_vs_dxil; pd.VS.BytecodeLength = sizeof oit_vs_dxil;
        pd.PS.pShaderBytecode = oit_ps_dxil; pd.PS.BytecodeLength = sizeof oit_ps_dxil;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
        pd.SampleDesc.Count = 1;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        test_pso_defaults(&pd, FALSE);
        hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
        CHECK(SUCCEEDED(hr) && pso, "k-buffer insert PSO created: 4 x 64-bit InterlockedMax with the old value used (%#lx)", hr);
        if (!pso) goto done;
    }

    case_clear_pattern();
    for (i = 0; i < sizeof rounds / sizeof rounds[0]; i++) case_round((int)i + 1, rounds[i]);

done:
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails;
}
