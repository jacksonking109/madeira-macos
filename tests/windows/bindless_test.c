/*  Descriptor heaps, arrays and register spaces ("bindless phase 1") for the
 *  wine-lab D3D12 port.
 *
 *  The DXIL shaders are precompiled by Microsoft's dxc and embedded
 *  (bindless_dxil.h, from bindless.hlsl; the regeneration commands are at the
 *  top of bindless.hlsl). The DXBC shader of case 3 is compiled at run time by
 *  Wine's d3dcompiler_47 (ps_5_1), as dxbc_test.c does.
 *
 *  Every texture is 2x1 with a different colour in each texel, and every
 *  sample is taken at u = 1.25, outside the texture: a CLAMP sampler returns
 *  the right texel, a WRAP sampler the left one. The centre pixel therefore
 *  names both the texture and the sampler a shader reached.
 *
 *  Cases (each draw into a cleared 64x64 RGBA8 target, read back):
 *    1  SM 6.6 ResourceDescriptorHeap[] + SamplerDescriptorHeap[], indices from
 *       root constants, under a root signature with
 *       CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED
 *    2  SM 6.0 Texture2D texs[4] : register(t0, space1), indexed dynamically,
 *       beside a texture at t0 space 0 in the same descriptor table; once with
 *       a 1.1 root signature, once with a 1.2 one (D3D12_STATIC_SAMPLER_DESC1)
 *    3  SM 5.1 DXBC through airconv: t0 in spaces 0 and 1 and a static sampler
 *       in space 2, each next to a decoy at the same register in another space
 *    4  root signature blobs: 1.2 serializes as version 3, and the versioned
 *       deserializer gives back the 1.2 description and the root flags
 *    5  SM 5.1 DXBC constant buffers through airconv (three-dimensional
 *       operands): b1 space 0 from root constants with a dynamically indexed
 *       element, b0 space 1 from a root CBV and b2 space 2 from a descriptor
 *       table, each of the last two after a space-0 decoy at the same register;
 *       and a constant-buffer array (b0-b1 space 3), which must be refused at
 *       pipeline creation
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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"   /* only test_pso_defaults() is used */
#include "root_sig.h"
#pragma clang diagnostic pop
#include "bindless_dxil.h"

static int checks, fails;
#define CHECK(cond, ...) do {                                                  \
    checks++;                                                                  \
    if (!(cond)) { fails++; printf("  FAIL  " __VA_ARGS__); printf("\n"); }     \
    else         { printf("  ok    " __VA_ARGS__); printf("\n"); }             \
    fflush(stdout);                                                            \
} while (0)

/* Root signature 1.2 (Agility SDK 1.610). llvm-mingw's d3d12.h predates it, so
 * the two structures are spelled out: D3D12_STATIC_SAMPLER_DESC1 is the 1.0
 * sampler plus a D3D12_SAMPLER_FLAGS word, D3D12_ROOT_SIGNATURE_DESC2 is
 * D3D12_ROOT_SIGNATURE_DESC1 with those samplers. */
#define RS_VERSION_1_2 ((D3D_ROOT_SIGNATURE_VERSION)0x3)
typedef struct { D3D12_STATIC_SAMPLER_DESC d; UINT Flags; } static_sampler_desc1;
typedef struct {
    UINT NumParameters; const D3D12_ROOT_PARAMETER1 *pParameters;
    UINT NumStaticSamplers; const static_sampler_desc1 *pStaticSamplers;
    D3D12_ROOT_SIGNATURE_FLAGS Flags;
} root_signature_desc2;
typedef struct { D3D_ROOT_SIGNATURE_VERSION Version; root_signature_desc2 Desc_1_2; } versioned_desc_1_2;

typedef HRESULT (WINAPI *pfn_create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *pfn_serialize_v)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *, ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *pfn_deserializer_v)(const void *, SIZE_T, REFIID, void **);
typedef const char *(*pfn_marker)(void);

enum { RT_W = 64, RT_H = 64, NTEX = 8 };
static const UINT64 PIX_SIZE = (UINT64)RT_W * RT_H * 4;   /* row pitch 256: already aligned */

static pD3DCompile         p_compile;
static pfn_serialize_v     p_serialize;
static pfn_deserializer_v  p_deserializer;
static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;
static ID3D12Resource     *rt, *pix, *tex[NTEX];
static ID3D12DescriptorHeap *rtvh;
static D3D12_CPU_DESCRIPTOR_HANDLE rtv;

/* Texture k: left texel A(k), right texel B(k). All sixteen colours differ in
 * every channel the cases read. */
static void tex_a(int k, BYTE c[4]) { c[0] = (BYTE)(10 + 30 * k); c[1] = (BYTE)(220 - 20 * k); c[2] = 40;  c[3] = 255; }
static void tex_b(int k, BYTE c[4]) { c[0] = (BYTE)(245 - 25 * k); c[1] = (BYTE)(5 + 25 * k); c[2] = (BYTE)(100 + 15 * k); c[3] = 255; }

/* ------------------------------------------------------------------ helpers */

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = heap;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL,
                                         &IID_ID3D12Resource, (void **)&r);
    return r;
}

static ID3D12Resource *make_tex2d(UINT w, UINT h, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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

/* Close, execute, wait on the fence, and reopen the list for the next case. */
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

/* Bind + clear the shared render target and set a full viewport and scissor. */
static void begin_pass(void) {
    const FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   /* transparent black: no texel is */
    D3D12_VIEWPORT vp; ZeroMemory(&vp, sizeof vp);
    vp.Width = (FLOAT)RT_W; vp.Height = (FLOAT)RT_H; vp.MaxDepth = 1.0f;
    D3D12_RECT sc = { 0, 0, RT_W, RT_H };
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
}

/* RT -> readback buffer, then submit and pull out the centre pixel. */
static BOOL finish_and_read(BYTE centre[4]) {
    D3D12_TEXTURE_COPY_LOCATION dl, sl; ZeroMemory(&dl, sizeof dl); ZeroMemory(&sl, sizeof sl);
    dl.pResource = pix; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dl.PlacedFootprint.Footprint.Width = RT_W; dl.PlacedFootprint.Footprint.Height = RT_H;
    dl.PlacedFootprint.Footprint.Depth = 1; dl.PlacedFootprint.Footprint.RowPitch = RT_W * 4;
    sl.pResource = rt; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    BYTE *p = NULL;
    ID3D12Resource_Map(pix, 0, NULL, (void **)&p);
    if (p) { memset(p, 0xCD, (size_t)PIX_SIZE); ID3D12Resource_Unmap(pix, 0, NULL); }

    barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dl, 0, 0, 0, &sl, NULL);
    barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!submit_and_wait()) return FALSE;

    p = NULL;
    ID3D12Resource_Map(pix, 0, NULL, (void **)&p);
    if (!p) { printf("        readback Map failed\n"); return FALSE; }
    memcpy(centre, p + ((RT_H / 2) * RT_W + RT_W / 2) * 4, 4);
    ID3D12Resource_Unmap(pix, 0, NULL);
    return TRUE;
}

static BOOL px_near(const BYTE got[4], const BYTE want[4]) {
    return abs(got[0] - want[0]) <= 2 && abs(got[1] - want[1]) <= 2 &&
           abs(got[2] - want[2]) <= 2 && abs(got[3] - want[3]) <= 2;
}

/* The NTEX 2x1 textures, uploaded once and left in PIXEL_SHADER_RESOURCE. */
static BOOL make_textures(void) {
    const UINT64 PLACE = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;   /* 512 */
    ID3D12Resource *up = make_buffer(D3D12_HEAP_TYPE_UPLOAD, PLACE * NTEX, D3D12_RESOURCE_STATE_GENERIC_READ);
    BYTE *m = NULL;
    int k;
    if (!up) return FALSE;
    ID3D12Resource_Map(up, 0, NULL, (void **)&m);
    if (!m) { ID3D12Resource_Release(up); return FALSE; }
    for (k = 0; k < NTEX; k++) { tex_a(k, m + PLACE * k); tex_b(k, m + PLACE * k + 4); }
    ID3D12Resource_Unmap(up, 0, NULL);
    for (k = 0; k < NTEX; k++) {
        tex[k] = make_tex2d(2, 1, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!tex[k]) break;
        D3D12_TEXTURE_COPY_LOCATION dl, sl; ZeroMemory(&dl, sizeof dl); ZeroMemory(&sl, sizeof sl);
        dl.pResource = tex[k]; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.pResource = up; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint.Offset = PLACE * k;
        sl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sl.PlacedFootprint.Footprint.Width = 2; sl.PlacedFootprint.Footprint.Height = 1;
        sl.PlacedFootprint.Footprint.Depth = 1; sl.PlacedFootprint.Footprint.RowPitch = 256;
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dl, 0, 0, 0, &sl, NULL);
        barrier(tex[k], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    BOOL ok = k == NTEX && submit_and_wait();
    ID3D12Resource_Release(up);
    return ok;
}

static ID3D12DescriptorHeap *make_heap(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n) {
    ID3D12DescriptorHeap *h = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
    hd.Type = type; hd.NumDescriptors = n; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&h);
    return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT i) {
    D3D12_CPU_DESCRIPTOR_HANDLE c;
    h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &c);
    c.ptr += (SIZE_T)i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, type);
    return c;
}
static D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT i) {
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    h->lpVtbl->GetGPUDescriptorHandleForHeapStart(h, &g);
    g.ptr += (UINT64)i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, type);
    return g;
}

static void put_srv(ID3D12DescriptorHeap *h, UINT slot, int k) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv; ZeroMemory(&sv, sizeof sv);
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(dev, tex[k], &sv,
                                          cpu_slot(h, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slot));
}

static void point_sampler(D3D12_STATIC_SAMPLER_DESC *ss, D3D12_TEXTURE_ADDRESS_MODE mode, UINT reg, UINT space) {
    ZeroMemory(ss, sizeof *ss);
    ss->Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss->AddressU = ss->AddressV = ss->AddressW = mode;
    ss->MaxAnisotropy = 1;
    ss->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ss->MaxLOD = D3D12_FLOAT32_MAX;
    ss->ShaderRegister = reg; ss->RegisterSpace = space;
    ss->ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
}

/* Serializes through D3D12SerializeVersionedRootSignature (any version) and
 * creates the root signature; the blob is handed back for case 4. */
static ID3D12RootSignature *make_rs(const char *name, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *v, ID3DBlob **blob_out) {
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature *rs = NULL;
    HRESULT hr = p_serialize(v, &blob, &err);
    if (err) {
        printf("        serializer: %.*s\n", (int)ID3D10Blob_GetBufferSize(err),
               (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    CHECK(SUCCEEDED(hr) && blob, "%s: root signature %s serialized (%#lx)", name,
          v->Version == RS_VERSION_1_2 ? "1.2" : v->Version == D3D_ROOT_SIGNATURE_VERSION_1_1 ? "1.1" : "1.0", hr);
    if (SUCCEEDED(hr) && blob) {
        hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob),
                                              ID3D10Blob_GetBufferSize(blob),
                                              &IID_ID3D12RootSignature, (void **)&rs);
        CHECK(SUCCEEDED(hr) && rs, "%s: root signature created (%#lx)", name, hr);
    }
    if (blob_out) *blob_out = blob;
    else if (blob) ID3D10Blob_Release(blob);
    return rs;
}

static ID3D12PipelineState *make_pso(const char *name, ID3D12RootSignature *rs,
                                     const void *vs, SIZE_T nvs, const void *ps, SIZE_T nps) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
    ID3D12PipelineState *pso = NULL;
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = vs; pd.VS.BytecodeLength = nvs;
    pd.PS.pShaderBytecode = ps; pd.PS.BytecodeLength = nps;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    test_pso_defaults(&pd, FALSE);
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    CHECK(SUCCEEDED(hr) && pso, "%s: graphics PSO created (%#lx)", name, hr);
    return SUCCEEDED(hr) ? pso : NULL;
}

/* One fullscreen draw; the root constants go to parameter 0. */
static BOOL draw_and_read(ID3D12RootSignature *rs, ID3D12PipelineState *pso, ID3D12DescriptorHeap **heaps,
                          UINT nheaps, const UINT *consts, UINT nconsts, UINT table_param,
                          D3D12_GPU_DESCRIPTOR_HANDLE table, BYTE got[4]) {
    begin_pass();
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, nheaps, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, nconsts, consts, 0);
    if (table.ptr) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, table_param, table);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    return finish_and_read(got);
}

#define RGBA(c) (c)[0], (c)[1], (c)[2], (c)[3]

/* ------------------------------------------------------------------- case 1 */

static ID3DBlob *g_heap_blob;   /* case 4 deserializes it */

static void case_heap_direct(void) {
    static const struct { UINT tex, smp; } draws[2] = { { 3, 1 }, { 6, 2 } };
    printf("\n[1] SM 6.6 ResourceDescriptorHeap[] / SamplerDescriptorHeap[] (root flags 0xc00)\n");

    D3D12_ROOT_PARAMETER1 rp; ZeroMemory(&rp, sizeof rp);
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.ShaderRegister = 0; rp.Constants.Num32BitValues = 2;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v; ZeroMemory(&v, sizeof v);
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    v.Desc_1_1.NumParameters = 1; v.Desc_1_1.pParameters = &rp;
    v.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                       D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ID3D12RootSignature *rs = make_rs("heap", &v, &g_heap_blob);
    ID3D12PipelineState *pso = rs ? make_pso("heap", rs, bl_vs_dxil, sizeof bl_vs_dxil,
                                             bl_ps_heap_dxil, sizeof bl_ps_heap_dxil) : NULL;

    /* Resource heap slot k = texture k. Sampler heap: WRAP, CLAMP, WRAP, CLAMP. */
    ID3D12DescriptorHeap *heaps[2] = { make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, NTEX),
                                       make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4) };
    CHECK(heaps[0] && heaps[1], "heap: resource and sampler heaps created");
    if (pso && heaps[0] && heaps[1]) {
        int k;
        for (k = 0; k < NTEX; k++) put_srv(heaps[0], k, k);
        for (k = 0; k < 4; k++) {
            D3D12_SAMPLER_DESC sd; ZeroMemory(&sd, sizeof sd);
            sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = (k & 1) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                                              : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.MaxAnisotropy = 1; sd.MaxLOD = D3D12_FLOAT32_MAX;
            ID3D12Device_CreateSampler(dev, &sd, cpu_slot(heaps[1], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, k));
        }
        for (k = 0; k < 2; k++) {
            UINT idx[2] = { draws[k].tex, draws[k].smp };
            D3D12_GPU_DESCRIPTOR_HANDLE none = { 0 };
            BYTE got[4] = {0}, want[4];
            if (draws[k].smp & 1) tex_b(draws[k].tex, want); else tex_a(draws[k].tex, want);
            BOOL ok = draw_and_read(rs, pso, heaps, 2, idx, 2, 0, none, got);
            CHECK(ok && px_near(got, want),
                  "heap: texture %u through sampler %u (%s): got (%u,%u,%u,%u), expected (%u,%u,%u,%u)",
                  draws[k].tex, draws[k].smp, (draws[k].smp & 1) ? "clamp" : "wrap", RGBA(got), RGBA(want));
        }
    }
    if (heaps[0]) ID3D12DescriptorHeap_Release(heaps[0]);
    if (heaps[1]) ID3D12DescriptorHeap_Release(heaps[1]);
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
}

/* ------------------------------------------------------------------- case 2 */

static ID3DBlob *g_array12_blob;   /* case 4 deserializes it */

/* Root constants b1; one table: t0 space 0 (one descriptor), then t0..t3 in
 * space 1; a CLAMP static sampler at s0. */
static void case_array(BOOL v12) {
    const char *name = v12 ? "array 1.2" : "array 1.1";
    printf("\n[2%s] SM 6.0 Texture2D texs[4] : register(t0, space1), root signature %s\n",
           v12 ? "b" : "a", v12 ? "1.2" : "1.1");

    D3D12_DESCRIPTOR_RANGE1 ranges[2]; ZeroMemory(ranges, sizeof ranges);
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1; ranges[0].BaseShaderRegister = 0; ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 4; ranges[1].BaseShaderRegister = 0; ranges[1].RegisterSpace = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER1 rp[2]; ZeroMemory(rp, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 1; rp[0].Constants.Num32BitValues = 1;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 2; rp[1].DescriptorTable.pDescriptorRanges = ranges;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    ID3D12RootSignature *rs;
    if (v12) {
        static_sampler_desc1 ss;
        point_sampler(&ss.d, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 0);
        ss.Flags = 0;
        versioned_desc_1_2 v; ZeroMemory(&v, sizeof v);
        v.Version = RS_VERSION_1_2;
        v.Desc_1_2.NumParameters = 2; v.Desc_1_2.pParameters = rp;
        v.Desc_1_2.NumStaticSamplers = 1; v.Desc_1_2.pStaticSamplers = &ss;
        rs = make_rs(name, (const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *)&v, &g_array12_blob);
    } else {
        D3D12_STATIC_SAMPLER_DESC ss;
        point_sampler(&ss, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 0);
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC v; ZeroMemory(&v, sizeof v);
        v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        v.Desc_1_1.NumParameters = 2; v.Desc_1_1.pParameters = rp;
        v.Desc_1_1.NumStaticSamplers = 1; v.Desc_1_1.pStaticSamplers = &ss;
        rs = make_rs(name, &v, NULL);
    }
    ID3D12PipelineState *pso = rs ? make_pso(name, rs, bl_vs_dxil, sizeof bl_vs_dxil,
                                             bl_ps_array_dxil, sizeof bl_ps_array_dxil) : NULL;

    /* The table starts at slot 8, not 0: [8] = texture 7 (t0 space 0),
     * [9..12] = textures 1..4 (texs[0..3]). */
    ID3D12DescriptorHeap *h = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16);
    CHECK(h != NULL, "%s: resource heap created", name);
    if (pso && h) {
        UINT k;
        put_srv(h, 8, 7);
        for (k = 0; k < 4; k++) put_srv(h, 9 + k, 1 + k);
        for (k = 0; k < 4; k += 2) {
            BYTE got[4] = {0}, want[4], plain[4];
            tex_b(1 + k, want); tex_b(7, plain);
            want[2] = plain[2]; want[3] = 255;   /* float4(texs[i].rg, plain.b, 1) */
            BOOL ok = draw_and_read(rs, pso, &h, 1, &k, 1, 1,
                                    gpu_slot(h, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8), got);
            CHECK(ok && px_near(got, want),
                  "%s: texs[%u] (space 1) + t0 (space 0): got (%u,%u,%u,%u), expected (%u,%u,%u,%u)",
                  name, k, RGBA(got), RGBA(want));
        }
    }
    if (h) ID3D12DescriptorHeap_Release(h);
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
}

/* ------------------------------------------------------------------- case 3 */

static const char hlsl_spaces[] =
    "Texture2D    ta   : register(t0);\n"
    "Texture2D    tb   : register(t0, space1);\n"
    "SamplerState s    : register(s0, space2);\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n"
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    "    float4 a = ta.SampleLevel(s, float2(1.25, 0.5), 0);\n"
    "    float4 b = tb.SampleLevel(s, float2(1.25, 0.5), 0);\n"
    "    return float4(a.r, b.g, 0.5, 1.0);\n"
    "}\n";

static ID3DBlob *compile(const char *name, const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = p_compile(src, strlen(src), name, NULL, NULL, entry, target, 0, 0, &code, &err);
    if (err) {
        SIZE_T n = ID3D10Blob_GetBufferSize(err);
        if (n) printf("        %s/%s compiler output:\n%.*s\n", entry, target, (int)n,
                      (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    const BYTE *bc = code ? (const BYTE *)ID3D10Blob_GetBufferPointer(code) : NULL;
    SIZE_T bcn = code ? ID3D10Blob_GetBufferSize(code) : 0;
    BOOL dxbc = bc && bcn > 32 && !memcmp(bc, "DXBC", 4);
    CHECK(SUCCEEDED(hr) && dxbc, "%s: HLSL %s compiled to %s DXBC (%#lx, %lu bytes)",
          name, entry, target, hr, (unsigned long)bcn);
    if (FAILED(hr) || !dxbc) { if (code) ID3D10Blob_Release(code); return NULL; }
    return code;
}

/* Every binding the shader reads has a decoy at the same register in another
 * space, listed FIRST, so a lookup that ignores the space finds the decoy: a
 * WRAP static sampler s0 space 0 before the CLAMP one at s0 space 2, and in the
 * table t0 space 0 = texture 5 before t0 space 1 = texture 6. */
static void case_dxbc_spaces(void) {
    printf("\n[3] SM 5.1 DXBC: registers in spaces 1 and 2 beside space-0 decoys\n");
    ID3DBlob *vs = compile("spaces", hlsl_spaces, "vs_main", "vs_5_1");
    ID3DBlob *ps = compile("spaces", hlsl_spaces, "ps_main", "ps_5_1");

    D3D12_DESCRIPTOR_RANGE1 ranges[2]; ZeroMemory(ranges, sizeof ranges);
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1;
    ranges[0].RegisterSpace = 0; ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[1].NumDescriptors = 1;
    ranges[1].RegisterSpace = 1; ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER1 rp; ZeroMemory(&rp, sizeof rp);
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable.NumDescriptorRanges = 2; rp.DescriptorTable.pDescriptorRanges = ranges;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss[2];
    point_sampler(&ss[0], D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, 0);    /* decoy */
    point_sampler(&ss[1], D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 2);
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v; ZeroMemory(&v, sizeof v);
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    v.Desc_1_1.NumParameters = 1; v.Desc_1_1.pParameters = &rp;
    v.Desc_1_1.NumStaticSamplers = 2; v.Desc_1_1.pStaticSamplers = ss;
    ID3D12RootSignature *rs = make_rs("spaces", &v, NULL);
    ID3D12PipelineState *pso = (rs && vs && ps)
        ? make_pso("spaces", rs, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs),
                   ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps)) : NULL;

    ID3D12DescriptorHeap *h = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8);
    CHECK(h != NULL, "spaces: resource heap created");
    if (pso && h) {
        BYTE got[4] = {0}, want[4], b6[4];
        put_srv(h, 4, 5);
        put_srv(h, 5, 6);
        tex_b(5, want); tex_b(6, b6);
        want[1] = b6[1]; want[2] = 128; want[3] = 255;   /* float4(ta.r, tb.g, 0.5, 1) */
        begin_pass();
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &h);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0,
                gpu_slot(h, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4));
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        BOOL ok = finish_and_read(got);
        CHECK(ok && px_near(got, want),
              "spaces: t0 in space 0 and 1, s0 in space 2: got (%u,%u,%u,%u), expected (%u,%u,%u,%u)",
              RGBA(got), RGBA(want));
    }
    if (h) ID3D12DescriptorHeap_Release(h);
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
}

/* ------------------------------------------------------------------- case 4 */

static UINT32 rts0_version(ID3DBlob *b) {
    const BYTE *p = (const BYTE *)ID3D10Blob_GetBufferPointer(b);
    SIZE_T n = ID3D10Blob_GetBufferSize(b), i;
    UINT32 v = 0;
    for (i = 32; i + 12 <= n; i += 4)
        if (!memcmp(p + i, "RTS0", 4)) { memcpy(&v, p + i + 8, 4); break; }
    return v;
}

static void case_blobs(void) {
    ID3D12VersionedRootSignatureDeserializer *d = NULL;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *un = NULL, *at = NULL;
    HRESULT hr;
    printf("\n[4] root signature blobs: 1.2 container and the versioned deserializer\n");
    if (!g_heap_blob || !g_array12_blob || !p_deserializer) {
        CHECK(0, "blobs: case 1 and 2b blobs and D3D12CreateVersionedRootSignatureDeserializer available");
        return;
    }
    CHECK(rts0_version(g_array12_blob) == 3, "blobs: a 1.2 description serializes as RTS0 version 3 (got %u)",
          rts0_version(g_array12_blob));

    hr = p_deserializer(ID3D10Blob_GetBufferPointer(g_array12_blob), ID3D10Blob_GetBufferSize(g_array12_blob),
                        &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&d);
    CHECK(SUCCEEDED(hr) && d, "blobs: 1.2 blob deserialized (%#lx)", hr);
    if (d) {
        un = ID3D12VersionedRootSignatureDeserializer_GetUnconvertedRootSignatureDesc(d);
        const versioned_desc_1_2 *u12 = (const versioned_desc_1_2 *)un;
        CHECK(un && un->Version == RS_VERSION_1_2 && u12->Desc_1_2.NumParameters == 2 &&
              u12->Desc_1_2.NumStaticSamplers == 1 && u12->Desc_1_2.pStaticSamplers &&
              u12->Desc_1_2.pStaticSamplers[0].d.AddressU == D3D12_TEXTURE_ADDRESS_MODE_CLAMP &&
              u12->Desc_1_2.pStaticSamplers[0].Flags == 0,
              "blobs: unconverted description is 1.2 with its D3D12_STATIC_SAMPLER_DESC1");
        hr = ID3D12VersionedRootSignatureDeserializer_GetRootSignatureDescAtVersion(d, D3D_ROOT_SIGNATURE_VERSION_1_1, &at);
        CHECK(SUCCEEDED(hr) && at && at->Version == D3D_ROOT_SIGNATURE_VERSION_1_1 &&
              at->Desc_1_1.NumStaticSamplers == 1 &&
              at->Desc_1_1.pStaticSamplers[0].Filter == D3D12_FILTER_MIN_MAG_MIP_POINT &&
              at->Desc_1_1.pParameters[1].DescriptorTable.pDescriptorRanges[1].RegisterSpace == 1,
              "blobs: 1.2 blob converts to 1.1 (%#lx)", hr);
        ID3D12VersionedRootSignatureDeserializer_Release(d);
        d = NULL;
    }

    hr = p_deserializer(ID3D10Blob_GetBufferPointer(g_heap_blob), ID3D10Blob_GetBufferSize(g_heap_blob),
                        &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&d);
    if (d) {
        un = ID3D12VersionedRootSignatureDeserializer_GetUnconvertedRootSignatureDesc(d);
        CHECK(un && un->Version == D3D_ROOT_SIGNATURE_VERSION_1_1 && un->Desc_1_1.Flags == 0xc00,
              "blobs: case 1's root flags survive the round trip (%#x)", un ? (unsigned)un->Desc_1_1.Flags : 0u);
        ID3D12VersionedRootSignatureDeserializer_Release(d);
    } else CHECK(0, "blobs: case 1 blob deserialized (%#lx)", hr);
}

/* ------------------------------------------------------------------- case 5 */

/* SM 5.1 constant-buffer operands are three-dimensional: [range id][register]
 * [element]. The compiler numbers the ranges in declaration order (CB0 = b1,
 * CB1 = b0 space 1, CB2 = b2 space 2), so two range ids are not their register.
 * The elements read are not all 0: b_val is element 1, a_vals[a_sel] a
 * dynamically indexed one. */
static const char hlsl_cbuffers[] =
    "cbuffer A : register(b1)         { float4 a_vals[2]; uint a_sel; };\n"
    "cbuffer B : register(b0, space1) { float4 b_pad; float4 b_val; };\n"
    "cbuffer C : register(b2, space2) { float4 c_val; };\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n"
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    "    float4 a = a_vals[a_sel];\n"
    "    return float4(a.x, b_val.y, c_val.z, a.w);\n"
    "}\n";

/* A constant-buffer array: the runtime maps singleton ranges only, so one must
 * be refused at pipeline creation, not compiled against its first buffer. Wine's
 * compiler lowers ConstantBuffer<S> arr[2] to ONE buffer of two elements, so the
 * array is made by widening this shader's declaration (widen_cb_decl). */
static const char hlsl_cbuffer_array[] =
    "cbuffer A : register(b0, space3) { float4 v; };\n"
    "float4 ps_main(float4 p : SV_Position) : SV_Target { return v; }\n";

/* dcl_constantBuffer CBn[lb:lb] in the given space -> CBn[lb:lb+1]. The SM 5.1
 * declaration is 7 tokens: opcode, operand, range id, lower bound, upper bound,
 * size, space. */
static BOOL widen_cb_decl(ID3DBlob *b, UINT space) {
    UINT32 *t = (UINT32 *)ID3D10Blob_GetBufferPointer(b);
    SIZE_T n = ID3D10Blob_GetBufferSize(b) / 4, i;
    for (i = 0; i + 7 <= n; i++)
        if ((t[i] & 0x7ff) == 0x59 /* DCL_CONSTANT_BUFFER */ && ((t[i] >> 24) & 0x7f) == 7 &&
            t[i + 6] == space && t[i + 4] == t[i + 3]) {
            t[i + 4] = t[i + 3] + 1;
            return TRUE;
        }
    return FALSE;
}

/* b1 space 0 from root constants; b0 space 1 from a root CBV, after a root CBV
 * decoy at b0 space 0; b2 space 2 from a descriptor table, after a decoy at b2
 * space 0 in the same table. A lookup that ignores the space finds the decoys. */
static void case_dxbc_cbuffers(void) {
    static const float a_vals[2][4] = { { 0.1f, 0.0f, 0.0f, 0.7f }, { 0.3f, 0.0f, 0.0f, 0.9f } };
    static const float cb_data[4][4] = {
        { 0.0f, 0.2f, 0.0f, 0.0f },   /* [0] b0 space 0, decoy (element 1) */
        { 0.0f, 0.6f, 0.0f, 0.0f },   /* [1] b0 space 1, b_val (element 1) */
        { 0.0f, 0.0f, 0.4f, 0.0f },   /* [2] b2 space 0, decoy */
        { 0.0f, 0.0f, 0.8f, 0.0f },   /* [3] b2 space 2, c_val */
    };
    printf("\n[5] SM 5.1 DXBC constant buffers: root constants, root CBV and table CBV, spaces 0-2\n");
    ID3DBlob *vs = compile("cbuffers", hlsl_cbuffers, "vs_main", "vs_5_1");
    ID3DBlob *ps = compile("cbuffers", hlsl_cbuffers, "ps_main", "ps_5_1");

    D3D12_DESCRIPTOR_RANGE1 ranges[2]; ZeroMemory(ranges, sizeof ranges);
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV; ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 2; ranges[0].RegisterSpace = 0; ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV; ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 2; ranges[1].RegisterSpace = 2; ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER1 rp[4]; ZeroMemory(rp, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;              /* decoy */
    rp[0].Descriptor.ShaderRegister = 0; rp[0].Descriptor.RegisterSpace = 0;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 1; rp[1].Constants.RegisterSpace = 0; rp[1].Constants.Num32BitValues = 9;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[2].Descriptor.ShaderRegister = 0; rp[2].Descriptor.RegisterSpace = 1;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[3].DescriptorTable.NumDescriptorRanges = 2; rp[3].DescriptorTable.pDescriptorRanges = ranges;
    for (int i = 0; i < 4; i++) rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v; ZeroMemory(&v, sizeof v);
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    v.Desc_1_1.NumParameters = 4; v.Desc_1_1.pParameters = rp;
    ID3D12RootSignature *rs = make_rs("cbuffers", &v, NULL);
    ID3D12PipelineState *pso = (rs && vs && ps)
        ? make_pso("cbuffers", rs, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs),
                   ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps)) : NULL;

    /* Four 256-byte constant buffers. The b0 pair holds its value at element 1
     * (b_val) and zeros at element 0, so reading the wrong element shows. */
    ID3D12Resource *cbuf = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 4 * 256, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12DescriptorHeap *h = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4);
    CHECK(cbuf && h, "cbuffers: constant buffer and resource heap created");
    if (pso && cbuf && h) {
        BYTE *m = NULL;
        UINT64 va = ID3D12Resource_GetGPUVirtualAddress(cbuf);
        int k;
        ID3D12Resource_Map(cbuf, 0, NULL, (void **)&m);
        if (m) {
            memset(m, 0, 4 * 256);
            for (k = 0; k < 4; k++) memcpy(m + 256 * k + (k < 2 ? 16 : 0), cb_data[k], 16);
            ID3D12Resource_Unmap(cbuf, 0, NULL);
        }
        for (k = 0; k < 2; k++) {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cv;
            cv.BufferLocation = va + 256 * (2 + k); cv.SizeInBytes = 256;
            ID3D12Device_CreateConstantBufferView(dev, &cv, cpu_slot(h, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 + k));
        }
        for (UINT sel = 0; sel < 2; sel++) {
            UINT consts[9];
            BYTE got[4] = {0}, want[4];
            memcpy(consts, a_vals, sizeof a_vals);
            consts[8] = sel;
            want[0] = (BYTE)(a_vals[sel][0] * 255.0f + 0.5f); want[1] = (BYTE)(0.6f * 255.0f + 0.5f);
            want[2] = (BYTE)(0.8f * 255.0f + 0.5f);           want[3] = (BYTE)(a_vals[sel][3] * 255.0f + 0.5f);
            begin_pass();
            ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
            ID3D12GraphicsCommandList_SetPipelineState(list, pso);
            ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &h);
            ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, va);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 1, 9, consts, 0);
            ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 2, va + 256);
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 3,
                    gpu_slot(h, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2));
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
            BOOL ok = finish_and_read(got);
            CHECK(ok && px_near(got, want),
                  "cbuffers: a_vals[%u] (b1, root constants), b_val (b0 space 1, root CBV), "
                  "c_val (b2 space 2, table): got (%u,%u,%u,%u), expected (%u,%u,%u,%u)",
                  sel, RGBA(got), RGBA(want));
        }
    }

    /* A constant-buffer array is refused with a named reason, never collapsed. */
    ID3DBlob *pa = compile("cbuffer array", hlsl_cbuffer_array, "ps_main", "ps_5_1");
    if (pa) CHECK(widen_cb_decl(pa, 3), "cbuffer array: declaration widened to b0-b1 space 3");
    if (pa && vs) {
        D3D12_DESCRIPTOR_RANGE1 ar; ZeroMemory(&ar, sizeof ar);
        ar.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV; ar.NumDescriptors = 2;
        ar.BaseShaderRegister = 0; ar.RegisterSpace = 3; ar.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER1 ap; ZeroMemory(&ap, sizeof ap);
        ap.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        ap.DescriptorTable.NumDescriptorRanges = 1; ap.DescriptorTable.pDescriptorRanges = &ar;
        ap.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC av; ZeroMemory(&av, sizeof av);
        av.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        av.Desc_1_1.NumParameters = 1; av.Desc_1_1.pParameters = &ap;
        ID3D12RootSignature *ars = make_rs("cbuffer array", &av, NULL);
        if (ars) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
            ID3D12PipelineState *apso = NULL;
            pd.pRootSignature = ars;
            pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
            pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(pa); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(pa);
            pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pd.SampleDesc.Count = 1; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            test_pso_defaults(&pd, FALSE);
            HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&apso);
            CHECK(FAILED(hr) && !apso, "cbuffer array: b0-b1 space 3 refused at pipeline creation (%#lx)", hr);
            if (apso) ID3D12PipelineState_Release(apso);
            ID3D12RootSignature_Release(ars);
        }
    }

    if (pa) ID3D10Blob_Release(pa);
    if (h) ID3D12DescriptorHeap_Release(h);
    if (cbuf) ID3D12Resource_Release(cbuf);
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
}

/* --------------------------------------------------------------------- main */

int main(void) {
    HRESULT hr;
    int k;
    printf("wine-lab d3d12: descriptor heaps, arrays and register spaces\n\n");

    HMODULE dcc = LoadLibraryA("d3dcompiler_47.dll");
    p_compile = dcc ? (pD3DCompile)GetProcAddress(dcc, "D3DCompile") : NULL;
    CHECK(p_compile != NULL, "d3dcompiler_47.dll loaded and D3DCompile resolved");

    HMODULE d12 = LoadLibraryA("d3d12.dll");
    pfn_create_device create = d12 ? (pfn_create_device)GetProcAddress(d12, "D3D12CreateDevice") : NULL;
    p_serialize = d12 ? (pfn_serialize_v)GetProcAddress(d12, "D3D12SerializeVersionedRootSignature") : NULL;
    p_deserializer = d12 ? (pfn_deserializer_v)GetProcAddress(d12, "D3D12CreateVersionedRootSignatureDeserializer") : NULL;
    CHECK(create && p_serialize && p_deserializer,
          "d3d12.dll loaded, device creation and versioned root signature entry points resolved");
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

    rt = make_tex2d(RT_W, RT_H, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    pix = make_buffer(D3D12_HEAP_TYPE_READBACK, PIX_SIZE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
    CHECK(rt && pix && rtvh, "64x64 R8G8B8A8 render target, readback buffer and RTV heap created");
    if (!rt || !pix || !rtvh) goto done;
    rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    CHECK(make_textures(), "%d 2x1 textures uploaded", NTEX);

    case_heap_direct();
    case_array(FALSE);
    case_array(TRUE);
    if (p_compile) case_dxbc_spaces();
    case_blobs();
    if (p_compile) case_dxbc_cbuffers();

    if (g_heap_blob) ID3D10Blob_Release(g_heap_blob);
    if (g_array12_blob) ID3D10Blob_Release(g_array12_blob);
    for (k = 0; k < NTEX; k++) if (tex[k]) ID3D12Resource_Release(tex[k]);
    ID3D12DescriptorHeap_Release(rtvh);
    ID3D12Resource_Release(pix);
    ID3D12Resource_Release(rt);
    ID3D12Fence_Release(fence);
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(alloc);
    ID3D12CommandQueue_Release(queue);
    ID3D12Device_Release(dev);

done:
    printf("\n%d checks, %d failures\n", checks, fails);
    fflush(stdout);
    return fails;
}
