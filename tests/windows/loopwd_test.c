/*  Loop watchdog for the wine-lab D3D12 port.
 *
 *  Metal never times a command buffer out, so a compute kernel stuck in a loop
 *  holds the GPU until the Mac's watchdog reboots it. The runtime caps every
 *  loop of a compute shader (src/unix/madeira_dxil_rewrite.cpp; madeira.cfg
 *  loop-watchdog, loop-watchdog-cap, default 1048576 passes per loop entry).
 *
 *  The kernel (loopwd.hlsl, DXIL from dxc in loopwd_dxil.h) runs two loops
 *  whose length comes from a buffer: a counted one (`for i < n`, folding an
 *  LCG into acc) and a data-dependent one (walk the LCG from a seed until it
 *  meets a target). Both are FINITE for every input this test feeds -- at most
 *  16.8 million passes each, on one threadgroup of four threads -- so even
 *  with loop-watchdog = 0 the dispatch ends (measured: 0.52 s on an M3 Pro,
 *  ~7 ms with the watchdog); nothing here can hang the GPU.
 *
 *  Cases:
 *    1  normal bounds (hundreds of passes): every output exact
 *    2  "garbage" bounds (1 << 24 passes): the loops stop at the default cap,
 *       so acc holds exactly 1 << 20 LCG steps and the walk counts 1 << 20
 *
 *  Exit code = number of failed checks (case 2 fails with loop-watchdog = 0
 *  or another cap; it still completes).
 */
#define COBJMACROS
#define INITGUID
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loopwd_dxil.h"

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

enum { NT = 4 };                                /* threads: one group of four */
static const UINT LCG_MUL = 1664525u, LCG_INC = 1013904223u;   /* full period mod 2^32 */
static const UINT DEFAULT_CAP = 1u << 20;
static const UINT HUGE_BOUND = 1u << 24;

struct input { UINT n, seed, target, pad; };

static pfn_serialize_v     p_serialize;
static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;
static ID3D12Resource     *inbuf, *outbuf, *rb;
static ID3D12RootSignature *rs;
static ID3D12PipelineState *pso;

static UINT lcg_steps(UINT x, UINT k) { while (k--) x = x * LCG_MUL + LCG_INC; return x; }

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
    DWORD w = WaitForSingleObject(ev, 30000);
    CloseHandle(ev);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    if (w != WAIT_OBJECT_0) { printf("        fence wait timed out\n"); return FALSE; }
    return TRUE;
}

/* b0: two root constants (mul, inc); t0: root SRV; u0: root UAV. */
static ID3D12RootSignature *make_rs(void) {
    D3D12_ROOT_PARAMETER1 p[3]; ZeroMemory(p, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.Num32BitValues = 2;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v; ZeroMemory(&v, sizeof v);
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    v.Desc_1_1.NumParameters = 3; v.Desc_1_1.pParameters = p;
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature *r = NULL;
    HRESULT hr = p_serialize(&v, &blob, &err);
    if (err) ID3D10Blob_Release(err);
    if (SUCCEEDED(hr) && blob)
        hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                              &IID_ID3D12RootSignature, (void **)&r);
    if (blob) ID3D10Blob_Release(blob);
    CHECK(SUCCEEDED(hr) && r, "root signature (2 constants, root SRV, root UAV) created (%#lx)", hr);
    return r;
}

/* One dispatch of the kernel over `in`; the four outputs come back in out. */
static BOOL dispatch(const struct input in[NT], UINT out[NT][4], double *ms) {
    void *p = NULL;
    const UINT consts[2] = { LCG_MUL, LCG_INC };
    LARGE_INTEGER f, t0, t1;
    ID3D12Resource_Map(inbuf, 0, NULL, &p);
    if (!p) return FALSE;
    memcpy(p, in, sizeof(struct input) * NT);
    ID3D12Resource_Unmap(inbuf, 0, NULL);

    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, 2, consts, 0);
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(list, 1, ID3D12Resource_GetGPUVirtualAddress(inbuf));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 2, ID3D12Resource_GetGPUVirtualAddress(outbuf));
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    barrier(outbuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, rb, 0, outbuf, 0, 16 * NT);
    barrier(outbuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if (!submit_and_wait()) return FALSE;
    QueryPerformanceCounter(&t1);
    *ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;

    p = NULL;
    ID3D12Resource_Map(rb, 0, NULL, &p);
    if (!p) return FALSE;
    memcpy(out, p, 16 * NT);
    ID3D12Resource_Unmap(rb, 0, NULL);
    return TRUE;
}

static UINT seed_of(int t) { return 0x01234567u * (UINT)(t + 1); }

static void case_normal(void) {
    struct input in[NT];
    UINT out[NT][4], k[NT];
    double ms = 0;
    int t, bad = 0;
    printf("\n[1] normal bounds\n");
    for (t = 0; t < NT; t++) {
        k[t] = 500 + 11 * t;
        in[t].n = 1000 + 37 * t; in[t].seed = seed_of(t); in[t].target = lcg_steps(in[t].seed, k[t]); in[t].pad = 0;
    }
    BOOL ok = dispatch(in, out, &ms);
    CHECK(ok, "dispatch completed (%.1f ms)", ms);
    if (!ok) return;
    for (t = 0; t < NT; t++) {
        UINT acc = lcg_steps(in[t].seed, in[t].n);
        if (out[t][0] != in[t].n || out[t][1] != acc || out[t][2] != k[t] || out[t][3] != in[t].target) {
            if (!bad++)
                printf("        thread %d: got (i %u, acc %#x, steps %u, x %#x), expected (%u, %#x, %u, %#x)\n", t,
                       out[t][0], out[t][1], out[t][2], out[t][3], in[t].n, acc, k[t], in[t].target);
        }
    }
    CHECK(!bad, "counted loop and LCG walk exact on all %d threads (%d wrong)", NT, bad);
}

static void case_huge(void) {
    struct input in[NT];
    UINT out[NT][4];
    double ms = 0;
    int t, bad_acc = 0, bad_walk = 0;
    printf("\n[2] garbage bounds: %u passes asked for, cap %u expected\n", HUGE_BOUND, DEFAULT_CAP);
    for (t = 0; t < NT; t++) {
        in[t].n = HUGE_BOUND; in[t].seed = seed_of(t); in[t].target = lcg_steps(in[t].seed, HUGE_BOUND); in[t].pad = 0;
    }
    BOOL ok = dispatch(in, out, &ms);
    CHECK(ok, "dispatch completed (%.1f ms)", ms);
    if (!ok) return;
    for (t = 0; t < NT; t++) {
        UINT capped = lcg_steps(in[t].seed, DEFAULT_CAP);
        if (out[t][1] != capped && !bad_acc++)
            printf("        thread %d: counted loop acc %#x, expected %#x (%u passes)%s\n", t, out[t][1], capped, DEFAULT_CAP,
                   out[t][1] == lcg_steps(in[t].seed, HUGE_BOUND) ? " -- it ran all the passes: no watchdog" : "");
        /* Only the step count: the compiler knows x == target once the walk
         * ends and stores target, so a capped walk still reports it. */
        if (out[t][2] != DEFAULT_CAP && !bad_walk++)
            printf("        thread %d: walk stopped after %u steps, expected %u\n", t, out[t][2], DEFAULT_CAP);
    }
    CHECK(!bad_acc, "counted loop stopped at the cap on all %d threads (%d wrong)", NT, bad_acc);
    CHECK(!bad_walk, "LCG walk stopped at the cap on all %d threads (%d wrong)", NT, bad_walk);
}

int main(void) {
    HRESULT hr;
    printf("wine-lab d3d12: loop watchdog\n\n");

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

    inbuf = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    outbuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(inbuf && outbuf && rb, "input, output and readback buffers created");
    if (!inbuf || !outbuf || !rb) goto done;
    rs = make_rs();
    if (!rs) goto done;
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd; ZeroMemory(&cd, sizeof cd);
        cd.pRootSignature = rs;
        cd.CS.pShaderBytecode = lw_cs_dxil; cd.CS.BytecodeLength = sizeof lw_cs_dxil;
        hr = ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **)&pso);
        CHECK(SUCCEEDED(hr) && pso, "compute PSO created (%#lx)", hr);
    }
    if (!pso) goto done;

    case_normal();
    case_huge();

done:
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rb) ID3D12Resource_Release(rb);
    if (outbuf) ID3D12Resource_Release(outbuf);
    if (inbuf) ID3D12Resource_Release(inbuf);
    if (fence) ID3D12Fence_Release(fence);
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (dev) ID3D12Device_Release(dev);
    printf("\n%d checks, %d failures\n", checks, fails);
    fflush(stdout);
    return fails;
}
