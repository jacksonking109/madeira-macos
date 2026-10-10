/*  Occlusion queries resolved into a GPU-only buffer, for the wine-lab D3D12 port.
 *
 *  The runtime counts occlusion queries in Metal visibility slots and hands the
 *  results to ResolveQueryData's destination on the CPU once the command buffer
 *  has completed, which a DEFAULT-heap buffer (no CPU mapping) never received.
 *  The Last of Us Part I resolves its occlusion queries into one and reads them
 *  in a compute shader (CS_ProcessOcclusionQueries) that zeroes the indirect
 *  draw arguments of every object whose result is 0, so every tested object was
 *  culled. A GPU-only destination is now written on the GPU, in program order.
 *
 *  One list: four queries around fullscreen-triangle draws into a 64x64 target,
 *  resolved into a DEFAULT buffer that is then copied (on the GPU, same list) to
 *  a readback buffer, and resolved into the readback buffer directly as well.
 *
 *  Cases:
 *    1  OCCLUSION around a draw covering the target: 4096 samples, both paths
 *    2  OCCLUSION around a draw whose scissor is empty: 0, both paths
 *    3  BINARY_OCCLUSION around a covering draw: nonzero from the GPU path
 *       (the sample count; the CPU path gives 1)
 *    4  OCCLUSION spanning two render passes (one draw into each of two
 *       targets): nonzero from the GPU path (answered visible), 8192 from the
 *       CPU path
 *
 *  The DXIL shaders are precompiled by Microsoft's dxc and embedded
 *  (occlusion_dxil.h, from occlusion.hlsl; regeneration commands there).
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
#include "occlusion_dxil.h"

static int checks, fails;
#define CHECK(cond, ...) do {                                                  \
    checks++;                                                                  \
    if (!(cond)) { fails++; printf("  FAIL  " __VA_ARGS__); printf("\n"); }     \
    else         { printf("  ok    " __VA_ARGS__); printf("\n"); }             \
    fflush(stdout);                                                            \
} while (0)

typedef HRESULT (WINAPI *pfn_create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef const char *(*pfn_marker)(void);

enum { W = 64, H = 64, NQ = 4, CPU_OFF = 256 };

static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = type;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}

static ID3D12Resource *make_target(void) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                         &IID_ID3D12Resource, (void **)&r);
    return r;
}

static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b; ZeroMemory(&b, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

/* Close, execute, wait on the fence. */
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
    if (w != WAIT_OBJECT_0) { printf("        fence wait timed out\n"); return FALSE; }
    return TRUE;
}

int main(void) {
    HRESULT hr;
    ID3D12Resource *rt[2] = { NULL, NULL }, *gpu_buf = NULL, *rb = NULL;
    ID3D12DescriptorHeap *rtvh = NULL;
    ID3D12QueryHeap *qh = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12PipelineState *pso = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2];
    UINT64 *res = NULL;
    printf("wine-lab d3d12: occlusion queries resolved into a GPU-only buffer\n\n");

    HMODULE d12 = LoadLibraryA("d3d12.dll");
    pfn_create_device create = d12 ? (pfn_create_device)GetProcAddress(d12, "D3D12CreateDevice") : NULL;
    CHECK(create != NULL, "d3d12.dll loaded and D3D12CreateDevice resolved");
    if (!create) goto done;
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

    hr = ID3D12Device_CreateRootSignature(dev, 0, oc_vs_dxil, sizeof oc_vs_dxil, &IID_ID3D12RootSignature, (void **)&rs);
    if (rs) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
        pd.pRootSignature = rs;
        pd.VS.pShaderBytecode = oc_vs_dxil; pd.VS.BytecodeLength = sizeof oc_vs_dxil;
        pd.PS.pShaderBytecode = oc_ps_dxil; pd.PS.BytecodeLength = sizeof oc_ps_dxil;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        test_pso_defaults(&pd, FALSE);
        hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    }
    CHECK(rs && pso, "root signature (embedded) and graphics pipeline created (%#lx)", hr);

    {
        D3D12_QUERY_HEAP_DESC qhd; ZeroMemory(&qhd, sizeof qhd);
        qhd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION; qhd.Count = NQ;
        ID3D12Device_CreateQueryHeap(dev, &qhd, &IID_ID3D12QueryHeap, (void **)&qh);
        D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
    }
    rt[0] = make_target(); rt[1] = make_target();
    gpu_buf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_COPY_DEST);
    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, 512, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(qh && rtvh && rt[0] && rt[1] && gpu_buf && rb,
          "occlusion query heap, two 64x64 targets, a DEFAULT and a READBACK buffer created");
    if (!rs || !pso || !qh || !rtvh || !rt[0] || !rt[1] || !gpu_buf || !rb) goto done;

    {
        UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv[0]);
        rtv[1] = rtv[0]; rtv[1].ptr += inc;
        ID3D12Device_CreateRenderTargetView(dev, rt[0], NULL, rtv[0]);
        ID3D12Device_CreateRenderTargetView(dev, rt[1], NULL, rtv[1]);
    }
    {   /* poison the readback buffer: an undelivered result must not read as a lucky 0 */
        BYTE *p = NULL;
        ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
        if (p) { memset(p, 0xCD, 512); ID3D12Resource_Unmap(rb, 0, NULL); }
    }

    {
        D3D12_VIEWPORT vp; ZeroMemory(&vp, sizeof vp);
        vp.Width = (FLOAT)W; vp.Height = (FLOAT)H; vp.MaxDepth = 1.0f;
        D3D12_RECT full = { 0, 0, W, H }, none = { 0, 0, 0, 0 };
        const FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv[0], clear, 0, NULL);
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv[1], clear, 0, NULL);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv[0], FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        /* [1] covering draw */
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &full);
        ID3D12GraphicsCommandList_BeginQuery(list, qh, D3D12_QUERY_TYPE_OCCLUSION, 0);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        ID3D12GraphicsCommandList_EndQuery(list, qh, D3D12_QUERY_TYPE_OCCLUSION, 0);
        /* [2] empty scissor: no sample passes */
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &none);
        ID3D12GraphicsCommandList_BeginQuery(list, qh, D3D12_QUERY_TYPE_OCCLUSION, 1);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        ID3D12GraphicsCommandList_EndQuery(list, qh, D3D12_QUERY_TYPE_OCCLUSION, 1);
        /* [3] binary, covering */
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &full);
        ID3D12GraphicsCommandList_BeginQuery(list, qh, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 2);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        ID3D12GraphicsCommandList_EndQuery(list, qh, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 2);
        /* [4] one draw in each of two render passes */
        ID3D12GraphicsCommandList_BeginQuery(list, qh, D3D12_QUERY_TYPE_OCCLUSION, 3);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv[1], FALSE, NULL);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        ID3D12GraphicsCommandList_EndQuery(list, qh, D3D12_QUERY_TYPE_OCCLUSION, 3);

        /* GPU-only destination, read on the GPU (a copy) in the same list */
        ID3D12GraphicsCommandList_ResolveQueryData(list, qh, D3D12_QUERY_TYPE_OCCLUSION, 0, 2, gpu_buf, 0);
        ID3D12GraphicsCommandList_ResolveQueryData(list, qh, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 2, 1, gpu_buf, 16);
        ID3D12GraphicsCommandList_ResolveQueryData(list, qh, D3D12_QUERY_TYPE_OCCLUSION, 3, 1, gpu_buf, 24);
        transition(gpu_buf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, rb, 0, gpu_buf, 0, NQ * 8);
        /* CPU-visible destination */
        ID3D12GraphicsCommandList_ResolveQueryData(list, qh, D3D12_QUERY_TYPE_OCCLUSION, 0, 2, rb, CPU_OFF);
        ID3D12GraphicsCommandList_ResolveQueryData(list, qh, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 2, 1, rb, CPU_OFF + 16);
        ID3D12GraphicsCommandList_ResolveQueryData(list, qh, D3D12_QUERY_TYPE_OCCLUSION, 3, 1, rb, CPU_OFF + 24);
    }
    CHECK(submit_and_wait(), "list with four queries and their resolves submitted");

    ID3D12Resource_Map(rb, 0, NULL, (void **)&res);
    CHECK(res != NULL, "readback buffer mapped");
    if (!res) goto done;
    {
        const UINT64 *g = res, *c = res + CPU_OFF / 8;
        printf("        GPU-only path: %llu %llu %llu %llu; CPU path: %llu %llu %llu %llu\n",
               (unsigned long long)g[0], (unsigned long long)g[1], (unsigned long long)g[2], (unsigned long long)g[3],
               (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2], (unsigned long long)c[3]);
        printf("\n[1] covering draw\n");
        CHECK(g[0] == W * H, "GPU-only buffer: %llu samples (expected %u)", (unsigned long long)g[0], W * H);
        CHECK(c[0] == W * H, "readback buffer: %llu samples (expected %u)", (unsigned long long)c[0], W * H);
        printf("\n[2] empty scissor\n");
        CHECK(g[1] == 0, "GPU-only buffer: %llu samples (expected 0)", (unsigned long long)g[1]);
        CHECK(c[1] == 0, "readback buffer: %llu samples (expected 0)", (unsigned long long)c[1]);
        printf("\n[3] binary occlusion, covering draw\n");
        CHECK(g[2] != 0 && g[2] != 0xCDCDCDCDCDCDCDCDull, "GPU-only buffer: %llu (nonzero)", (unsigned long long)g[2]);
        CHECK(c[2] == 1, "readback buffer: %llu (expected 1)", (unsigned long long)c[2]);
        printf("\n[4] query spanning two render passes\n");
        CHECK(g[3] != 0 && g[3] != 0xCDCDCDCDCDCDCDCDull, "GPU-only buffer: %llu (nonzero: answered visible)", (unsigned long long)g[3]);
        CHECK(c[3] == 2 * W * H, "readback buffer: %llu samples (expected %u)", (unsigned long long)c[3], 2 * W * H);
    }
    ID3D12Resource_Unmap(rb, 0, NULL);

done:
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
    if (qh) ID3D12QueryHeap_Release(qh);
    if (rb) ID3D12Resource_Release(rb);
    if (gpu_buf) ID3D12Resource_Release(gpu_buf);
    if (rt[1]) ID3D12Resource_Release(rt[1]);
    if (rt[0]) ID3D12Resource_Release(rt[0]);
    if (rtvh) ID3D12DescriptorHeap_Release(rtvh);
    if (fence) ID3D12Fence_Release(fence);
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (dev) ID3D12Device_Release(dev);
    printf("\n%d checks, %d failures\n", checks, fails);
    fflush(stdout);
    return fails;
}
