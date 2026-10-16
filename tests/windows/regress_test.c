/*  Regression checks for the 2026-10-12/13 Rise of the Tomb Raider fixes, for the
 *  wine-lab D3D12 port. Each case is something the game hit; each check names
 *  what a regression would look like.
 *
 *    1  GetCopyableFootprints of a BUFFER: the total is the width (not rounded up to
 *       256) and the footprint sits at the base offset. ROTTR's d3dx12
 *       UpdateSubresources skipped every mesh upload whose upload buffer was "too
 *       small" by the rounding: the DX12 menu room was missing.
 *    2  A NULL shader resource view (texture and buffer) reads zeros. It used to be
 *       texture handle 0, which Metal does not define.
 *    3  Per-instance input with InstanceDataStepRate 0 gives EVERY instance element 0
 *       ("never advance"); step rate 1 gives instance N element N. ROTTR's grass read
 *       past a one-element UV buffer -> NaN normals -> black rectangles. DXBC path.
 *    4  ClearDepthStencilView with rects (ROTTR clears one shadow-atlas tile per
 *       cascade), against draws on the same depth view. The runtime draws the clear
 *       quads inside an open depth-only pass (ds-rect-inpass) or holds the clear for
 *       the next pass on its view (ds-rect-defer); the order of operations on the
 *       target must not change:
 *         a  in-pass: full clear, draw, rect clear, draw
 *         b  held, then a LESS_EQUAL draw: a dropped clear shows as no 0.25 texel
 *         c  held, then an ALWAYS draw: a clear landing AFTER the draw leaves 0.25
 *         d  held, then a FULL clear of the same view: the full clear must win
 *         e  held, then a barrier and a copy: the copy must see the clear
 *    5  Stub audit (2026-10-16): names and private data on the main objects, resource
 *       heap properties, PIX markers, SetPredication, OMSetDepthBounds and
 *       CheckDriverMatchingIdentifier no longer answer E_NOTIMPL from their stubs.
 *
 *  Shaders are compiled at run time by Wine's d3dcompiler_47 (SM 5.0 DXBC, the
 *  airconv path ROTTR uses). Exit code = number of failed checks.
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

enum { W = 64, H = 64, PITCH = 256, IMG = PITCH * H };

static pD3DCompile         p_compile;
static pfn_serialize_rs    p_serialize;
static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;
static ID3D12Resource     *rb;
static ID3D12DescriptorHeap *rtvh, *dsvh, *srvh;

/* ------------------------------------------------------------------ helpers */

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = type;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}
static ID3D12Resource *make_tex(DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    D3D12_CLEAR_VALUE cv; ZeroMemory(&cv, sizeof cv);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1; d.Flags = flags;
    cv.Format = fmt; cv.DepthStencil.Depth = 1.0f;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st,
                                         (flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) ? &cv : NULL,
                                         &IID_ID3D12Resource, (void **)&r);
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
static void copy_to_rb(ID3D12Resource *t, DXGI_FORMAT fp_fmt) {
    D3D12_TEXTURE_COPY_LOCATION d, s;
    ZeroMemory(&d, sizeof d); ZeroMemory(&s, sizeof s);
    d.pResource = rb; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint.Footprint.Format = fp_fmt; d.PlacedFootprint.Footprint.Width = W;
    d.PlacedFootprint.Footprint.Height = H; d.PlacedFootprint.Footprint.Depth = 1; d.PlacedFootprint.Footprint.RowPitch = PITCH;
    s.pResource = t; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &d, 0, 0, 0, &s, NULL);
}
static void poison_rb(void) {
    BYTE *p = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
    if (p) { memset(p, 0xCD, IMG); ID3D12Resource_Unmap(rb, 0, NULL); }
}
static UINT32 rb_texel(UINT x, UINT y) {
    BYTE *p = NULL; UINT32 v = 0;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
    if (p) { memcpy(&v, p + y * PITCH + x * 4, 4); ID3D12Resource_Unmap(rb, 0, NULL); }
    return v;
}
static ID3DBlob *compile(const char *name, const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = p_compile(src, strlen(src), name, NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err) {
        SIZE_T n = ID3D10Blob_GetBufferSize(err);
        if (n) printf("        %s %s/%s compiler output:\n%.*s\n", name, entry, target, (int)n, (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    CHECK(SUCCEEDED(hr) && code, "%s: HLSL %s compiled to %s (%#lx)", name, entry, target, hr);
    return SUCCEEDED(hr) ? code : NULL;
}
static ID3D12RootSignature *make_rs(const char *name, const D3D12_ROOT_SIGNATURE_DESC *rd) {
    ID3DBlob *blob = NULL, *err = NULL; ID3D12RootSignature *rs = NULL;
    HRESULT hr = p_serialize(rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err);
    if (err) ID3D10Blob_Release(err);
    if (SUCCEEDED(hr) && blob)
        hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                              &IID_ID3D12RootSignature, (void **)&rs);
    if (blob) ID3D10Blob_Release(blob);
    CHECK(SUCCEEDED(hr) && rs, "%s: root signature created (%#lx)", name, hr);
    return rs;
}
static ID3D12PipelineState *make_pso(const char *name, ID3D12RootSignature *rs, ID3DBlob *vs, ID3DBlob *ps,
                                     const D3D12_INPUT_ELEMENT_DESC *il, UINT nil, DXGI_FORMAT rtv, DXGI_FORMAT dsv,
                                     D3D12_COMPARISON_FUNC depth_func) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
    ID3D12PipelineState *pso = NULL;
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    if (ps) { pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps); }
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = nil;
    pd.NumRenderTargets = rtv != DXGI_FORMAT_UNKNOWN ? 1 : 0;
    pd.RTVFormats[0] = rtv;
    pd.DSVFormat = dsv;
    pd.SampleDesc.Count = 1;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    test_pso_defaults(&pd, dsv != DXGI_FORMAT_UNKNOWN);
    if (dsv != DXGI_FORMAT_UNKNOWN) pd.DepthStencilState.DepthFunc = depth_func;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    CHECK(SUCCEEDED(hr) && pso, "%s: pipeline created (%#lx)", name, hr);
    return SUCCEEDED(hr) ? pso : NULL;
}
static D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE t, UINT i) {
    D3D12_CPU_DESCRIPTOR_HANDLE c;
    h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &c);
    c.ptr += (SIZE_T)i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, t);
    return c;
}
static void set_vp(void) {
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 }; D3D12_RECT sc = { 0, 0, W, H };
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
}

/* ------------------------------------------------- 1: buffer footprints */

static void case_footprints(void) {
    printf("\n[1] GetCopyableFootprints of a buffer: total = width, footprint at the base offset\n");
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = 100008; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    UINT64 bases[2] = { 0, 256 }; UINT b;
    for (b = 0; b < 2; b++) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows = 0; UINT64 rowsize = 0, total = 0;
        ZeroMemory(&fp, sizeof fp);
        ID3D12Device_GetCopyableFootprints(dev, &d, 0, 1, bases[b], &fp, &rows, &rowsize, &total);
        CHECK(total == 100008, "base %llu: total bytes %llu (want 100008, not the 256-rounded 100096)", (unsigned long long)bases[b], (unsigned long long)total);
        CHECK(fp.Offset == bases[b], "base %llu: footprint offset %llu (want the base offset)", (unsigned long long)bases[b], (unsigned long long)fp.Offset);
        CHECK(fp.Footprint.Width == 100008 && fp.Footprint.Height == 1 && fp.Footprint.Depth == 1 && rows == 1 && rowsize == 100008,
              "base %llu: footprint %ux%ux%u, rows %u, row size %llu", (unsigned long long)bases[b], fp.Footprint.Width,
              fp.Footprint.Height, fp.Footprint.Depth, rows, (unsigned long long)rowsize);
    }
}

/* ------------------------------------------------------ 2: null SRVs */

static const char hlsl_null[] =
    "Texture2D<float4> t0 : register(t0);\n"
    "Buffer<float4>    t1 : register(t1);\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n"
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    "    return t0.Load(int3(0, 0, 0)) + t1.Load(0) + float4(0.25, 0.5, 0.75, 1.0);\n"
    "}\n";

static void case_null_srv(ID3D12Resource *rt) {
    printf("\n[2] NULL shader resource views (texture and buffer) read zeros\n");
    ID3DBlob *vs = compile("null-srv", hlsl_null, "vs_main", "vs_5_0"), *ps = compile("null-srv", hlsl_null, "ps_main", "ps_5_0");
    D3D12_DESCRIPTOR_RANGE rg; ZeroMemory(&rg, sizeof rg);
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 2; rg.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp; ZeroMemory(&rp, sizeof rp);
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp.DescriptorTable.NumDescriptorRanges = 1;
    rp.DescriptorTable.pDescriptorRanges = &rg; rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd; ZeroMemory(&rd, sizeof rd); rd.NumParameters = 1; rd.pParameters = &rp;
    ID3D12RootSignature *rs = make_rs("null-srv", &rd);
    ID3D12PipelineState *pso = (rs && vs && ps) ? make_pso("null-srv", rs, vs, ps, NULL, 0, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, 0) : NULL;
    if (!pso) return;

    D3D12_SHADER_RESOURCE_VIEW_DESC sd; ZeroMemory(&sd, sizeof sd);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(dev, NULL, &sd, heap_cpu(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0));
    ZeroMemory(&sd, sizeof sd);
    sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Buffer.NumElements = 4;
    ID3D12Device_CreateShaderResourceView(dev, NULL, &sd, heap_cpu(srvh, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1));

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_cpu(rtvh, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 0);
    float clr[4] = { 0, 0, 0, 0 };
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clr, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &srvh);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    srvh->lpVtbl->GetGPUDescriptorHandleForHeapStart(srvh, &g);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, g);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    set_vp();
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_to_rb(rt, DXGI_FORMAT_R8G8B8A8_UNORM);
    barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    poison_rb();
    if (!submit_and_wait()) { CHECK(0, "null-srv: submission finished"); return; }
    UINT32 px = rb_texel(W / 2, H / 2);
    UINT r = px & 0xff, gch = (px >> 8) & 0xff, b = (px >> 16) & 0xff, a = px >> 24;
    CHECK(r >= 63 && r <= 65 && gch >= 127 && gch <= 129 && b >= 190 && b <= 192 && a == 255,
          "null texture + null buffer reads add nothing: pixel (%u,%u,%u,%u), want (64,128,191,255)", r, gch, b, a);
}

/* ---------------------------------------------- 3: instance step rate 0 */

static const char hlsl_inst[] =
    "struct VSIn { float2 pos : POSITION; float4 col : COLOR; uint iid : SV_InstanceID; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VSOut vs_main(VSIn i) {\n"
    "    VSOut o;\n"
    "    o.pos = float4(i.pos.x * 0.5 + (i.iid ? 0.5 : -0.5), i.pos.y, 0.0, 1.0);\n"
    "    o.col = i.col;\n"
    "    return o;\n"
    "}\n"
    "float4 ps_main(VSOut i) : SV_Target { return i.col; }\n";

static void case_step_rate(ID3D12Resource *rt) {
    printf("\n[3] per-instance input: step rate 0 = every instance reads element 0 (DXBC)\n");
    ID3DBlob *vs = compile("instance", hlsl_inst, "vs_main", "vs_5_0"), *ps = compile("instance", hlsl_inst, "ps_main", "ps_5_0");
    D3D12_ROOT_SIGNATURE_DESC rd; ZeroMemory(&rd, sizeof rd);
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3D12RootSignature *rs = make_rs("instance", &rd);
    D3D12_INPUT_ELEMENT_DESC il0[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 0 },
    }, il1[2];
    memcpy(il1, il0, sizeof il1); il1[1].InstanceDataStepRate = 1;
    ID3D12PipelineState *p0 = (rs && vs && ps) ? make_pso("step-rate-0", rs, vs, ps, il0, 2, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, 0) : NULL;
    ID3D12PipelineState *p1 = (rs && vs && ps) ? make_pso("step-rate-1", rs, vs, ps, il1, 2, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, 0) : NULL;
    if (!p0 || !p1) return;

    /* slot 0: a quad over the whole target (two triangles); slot 1: [green, red] */
    static const float quad[12] = { -1, -1, -1, 1, 1, 1, -1, -1, 1, 1, 1, -1 };
    static const float cols[8] = { 0, 1, 0, 1, 1, 0, 0, 1 };
    ID3D12Resource *vb = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
    BYTE *p = NULL;
    if (!vb) { CHECK(0, "instance: vertex buffer created"); return; }
    ID3D12Resource_Map(vb, 0, NULL, (void **)&p);
    memcpy(p, quad, sizeof quad); memcpy(p + 128, cols, sizeof cols);
    ID3D12Resource_Unmap(vb, 0, NULL);
    D3D12_VERTEX_BUFFER_VIEW v[2];
    v[0].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb); v[0].SizeInBytes = sizeof quad; v[0].StrideInBytes = 8;
    v[1].BufferLocation = v[0].BufferLocation + 128; v[1].SizeInBytes = 16; v[1].StrideInBytes = 16;   /* ONE element, like ROTTR's grass */

    int k;
    for (k = 0; k < 2; k++) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_cpu(rtvh, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 0);
        float clr[4] = { 0, 0, 1, 1 };
        if (k == 1) v[1].SizeInBytes = 32;   /* step rate 1 reads element 1: give it both */
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clr, 0, NULL);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
        ID3D12GraphicsCommandList_SetPipelineState(list, k ? p1 : p0);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 2, v);
        set_vp();
        ID3D12GraphicsCommandList_DrawInstanced(list, 6, 2, 0, 0);
        barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        copy_to_rb(rt, DXGI_FORMAT_R8G8B8A8_UNORM);
        barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        poison_rb();
        if (!submit_and_wait()) { CHECK(0, "instance: submission finished"); return; }
        UINT32 l = rb_texel(W / 4, H / 2), r = rb_texel(3 * W / 4, H / 2);
        if (k == 0)
            CHECK(l == 0xff00ff00u && r == 0xff00ff00u, "step rate 0: both instances green (left %08x, right %08x; red on the right = "
                  "instance 1 read past the one-element buffer)", l, r);
        else
            CHECK(l == 0xff00ff00u && r == 0xff0000ffu, "step rate 1: instance 0 green, instance 1 red (left %08x, right %08x)", l, r);
    }
    ID3D12Resource_Release(vb);
}

/* ------------------------------------------------ 4: rect depth clears */

static const char hlsl_depth[] =
    "cbuffer Z : register(b0) { float z; };\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), z, 1.0);\n"
    "}\n";

static ID3D12RootSignature *drs;
static ID3D12PipelineState *d_le, *d_always;
static ID3D12Resource *dtex;

static D3D12_CPU_DESCRIPTOR_HANDLE dsv(void) { return heap_cpu(dsvh, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 0); }
static void d_bind(void) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = dsv();
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 0, NULL, FALSE, &h);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, drs);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    set_vp();
}
static void d_draw(ID3D12PipelineState *pso, float z) {
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 1, &z, 0);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
}
static void d_clear_full(float z) { ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv(), D3D12_CLEAR_FLAG_DEPTH, z, 0, 0, NULL); }
static void d_clear_left(float z) {
    D3D12_RECT r = { 0, 0, W / 2, H };
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv(), D3D12_CLEAR_FLAG_DEPTH, z, 0, 1, &r);
}
/* reads the depth texture back; the caller already transitioned it to COPY_SOURCE */
static void d_check(const char *what, float want_l, float want_r) {
    UINT x, y, bad = 0; float first = 0;
    copy_to_rb(dtex, DXGI_FORMAT_R32_TYPELESS);
    barrier(dtex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    poison_rb();
    if (!submit_and_wait()) { CHECK(0, "%s: submission finished", what); return; }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            UINT32 v = rb_texel(x, y); float f; memcpy(&f, &v, 4);
            if (f != (x < W / 2 ? want_l : want_r)) { if (!bad) first = f; bad++; }
        }
    CHECK(bad == 0, "%s: left half %g, right half %g (%u/%u texels wrong, first %g)", what, want_l, want_r, bad, W * H, first);
}
/* every case starts from a known, already-applied state: the whole target at z */
static void d_reset(float z) {
    d_bind(); d_clear_full(1.0f); d_draw(d_always, z);
    barrier(dtex, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(dtex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    submit_and_wait();
}

static void case_rect_clears(void) {
    printf("\n[4] ClearDepthStencilView with rects vs draws on the same depth view\n");
    ID3DBlob *vs = compile("depth", hlsl_depth, "vs_main", "vs_5_0");
    D3D12_ROOT_PARAMETER rp; ZeroMemory(&rp, sizeof rp);
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp.Constants.Num32BitValues = 1;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rd; ZeroMemory(&rd, sizeof rd); rd.NumParameters = 1; rd.pParameters = &rp;
    drs = make_rs("depth", &rd);
    d_le = (drs && vs) ? make_pso("depth LESS_EQUAL (depth only)", drs, vs, NULL, NULL, 0, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT, D3D12_COMPARISON_FUNC_LESS_EQUAL) : NULL;
    d_always = (drs && vs) ? make_pso("depth ALWAYS (depth only)", drs, vs, NULL, NULL, 0, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT, D3D12_COMPARISON_FUNC_ALWAYS) : NULL;
    dtex = make_tex(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    CHECK(dtex != NULL, "depth: 64x64 D32_FLOAT target created");
    if (!d_le || !d_always || !dtex) return;
    ID3D12Device_CreateDepthStencilView(dev, dtex, NULL, dsv());

    /* a: in-pass. Expected left 0.25 (the clear, kept by LESS_EQUAL 0.3), right 0.3.
     * Dropped clear: left 0.3. Clear before the first draw: left 0.3 too. */
    d_bind(); d_clear_full(1.0f); d_draw(d_always, 0.6f); d_clear_left(0.25f); d_draw(d_le, 0.3f);
    barrier(dtex, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d_check("[4a] in-pass rect clear between two draws", 0.25f, 0.3f);

    /* b: held (no pass open), then a LESS_EQUAL draw. Expected left 0.25, right 0.3; dropped: left 0.3. */
    d_reset(0.6f);
    d_clear_left(0.25f); d_bind(); d_draw(d_le, 0.3f);
    barrier(dtex, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d_check("[4b] held rect clear, then a LESS_EQUAL draw", 0.25f, 0.3f);

    /* c: held, then an ALWAYS draw. Expected 0.3 everywhere; a clear landing after the draw leaves left 0.25. */
    d_reset(0.6f);
    d_clear_left(0.25f); d_bind(); d_draw(d_always, 0.3f);
    barrier(dtex, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d_check("[4c] held rect clear, then an ALWAYS draw (order)", 0.3f, 0.3f);

    /* d: held, then a FULL clear of the same view, then LESS_EQUAL 0.3: the full clear wins -> 0.3 everywhere.
     * If the full clear landed first, the rect would survive: left 0.25. */
    d_reset(0.6f);
    d_clear_left(0.25f); d_clear_full(1.0f); d_bind(); d_draw(d_le, 0.3f);
    barrier(dtex, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d_check("[4d] held rect clear, then a full clear (the full clear wins)", 0.3f, 0.3f);

    /* e: held, then a barrier and the copy: the copy must see the clear. Expected left 0.25, right 0.6. */
    d_reset(0.6f);
    d_clear_left(0.25f);
    barrier(dtex, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d_check("[4e] held rect clear, then a barrier and a copy", 0.25f, 0.6f);
}

/* 5. Stub audit (2026-10-16): methods that answered E_NOTIMPL from their generated stubs.
 *  a. names and private data on the main objects (SetName is WKPDID_D3DDebugObjectNameW)
 *  b. GetHeapProperties reports the heap type a resource was created in
 *  c. PIX markers, SetPredication and OMSetDepthBounds are accepted, and the work around
 *     them still runs (a predicated clear still lands)
 *  d. CheckDriverMatchingIdentifier says "unrecognized", so cached blobs are rebuilt */
static const GUID name_guid = { 0x4cca5fd8, 0x921f, 0x42c8, { 0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41 } };
static const GUID test_guid = { 0x1234abcd, 0x5678, 0x4321, { 1, 2, 3, 4, 5, 6, 7, 8 } };
static void name_check(const char *what, ID3D12Object *o) {
    static const WCHAR nm[] = L"regress-name";
    WCHAR got[32] = { 0 }; UINT n = sizeof got;
    HRESULT h1 = ID3D12Object_SetName(o, nm);
    HRESULT h2 = ID3D12Object_GetPrivateData(o, &name_guid, &n, got);
    CHECK(h1 == S_OK && h2 == S_OK && n == sizeof nm && !memcmp(got, nm, sizeof nm),
          "[5a] %s: SetName stores the name, GetPrivateData reads it back (%#lx, %#lx, %u bytes)", what, h1, h2, n);
}
static void case_stub_audit(ID3D12Resource *rt) {
    printf("\n[5] stub audit: names, private data, heap properties, markers, predication\n");
    name_check("device", (ID3D12Object *)dev);
    name_check("queue", (ID3D12Object *)queue);
    name_check("allocator", (ID3D12Object *)alloc);
    name_check("command list", (ID3D12Object *)list);
    name_check("fence", (ID3D12Object *)fence);
    name_check("resource", (ID3D12Object *)rt);
    name_check("descriptor heap", (ID3D12Object *)rtvh);
    {
        UINT64 v = 0x1122334455667788ull, got = 0; UINT n = sizeof got;
        HRESULT h1 = ID3D12CommandQueue_SetPrivateData(queue, &test_guid, sizeof v, &v);
        HRESULT h2 = ID3D12CommandQueue_GetPrivateData(queue, &test_guid, &n, &got);
        CHECK(h1 == S_OK && h2 == S_OK && got == v, "[5a] queue private data round-trips (%#lx, %#lx)", h1, h2);
        ID3D12CommandQueue_SetPrivateData(queue, &test_guid, 0, NULL);
        n = sizeof got;
        h2 = ID3D12CommandQueue_GetPrivateData(queue, &test_guid, &n, &got);
        CHECK(h2 == DXGI_ERROR_NOT_FOUND, "[5a] deleted private data reads as DXGI_ERROR_NOT_FOUND (%#lx)", h2);
    }
    {
        D3D12_HEAP_PROPERTIES hp; D3D12_HEAP_FLAGS hf = (D3D12_HEAP_FLAGS)-1;
        HRESULT h = ID3D12Resource_GetHeapProperties(rb, &hp, &hf);
        CHECK(h == S_OK && hp.Type == D3D12_HEAP_TYPE_READBACK, "[5b] readback buffer: heap type READBACK (%#lx, type %d)", h, (int)hp.Type);
        h = ID3D12Resource_GetHeapProperties(rt, &hp, &hf);
        CHECK(h == S_OK && hp.Type == D3D12_HEAP_TYPE_DEFAULT, "[5b] render target: heap type DEFAULT (%#lx, type %d)", h, (int)hp.Type);
    }
    {
        static const float green[4] = { 0, 1, 0, 1 };
        ID3D12GraphicsCommandList1 *l1 = NULL;
        ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList1, (void **)&l1);
        ID3D12CommandQueue_BeginEvent(queue, 0, "q", 2);
        ID3D12GraphicsCommandList_BeginEvent(list, 0, "frame", 6);
        ID3D12GraphicsCommandList_SetMarker(list, 0, "m", 2);
        ID3D12GraphicsCommandList_SetPredication(list, rb, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        if (l1) ID3D12GraphicsCommandList1_OMSetDepthBounds(l1, 0.25f, 0.75f);
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, heap_cpu(rtvh, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 0), green, 0, NULL);
        ID3D12GraphicsCommandList_SetPredication(list, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        ID3D12GraphicsCommandList_EndEvent(list);
        barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        copy_to_rb(rt, DXGI_FORMAT_R8G8B8A8_UNORM);
        barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        poison_rb();
        BOOL ran = submit_and_wait();
        ID3D12CommandQueue_SetMarker(queue, 0, "q", 2);
        ID3D12CommandQueue_EndEvent(queue);
        UINT32 t = rb_texel(5, 5);
        CHECK(ran && l1 && t == 0xFF00FF00u, "[5c] markers, predication and depth bounds accepted; the predicated clear still ran (texel %08x)", t);
        if (l1) ID3D12GraphicsCommandList1_Release(l1);
    }
    {
        ID3D12Device5 *d5 = NULL;
        ID3D12Device_QueryInterface(dev, &IID_ID3D12Device5, (void **)&d5);
        D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER id; ZeroMemory(&id, sizeof id);
        D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS st = d5 ? ID3D12Device5_CheckDriverMatchingIdentifier(d5, D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE, &id)
                                                        : D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE;
        CHECK(d5 && st == D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED, "[5d] CheckDriverMatchingIdentifier: unrecognized (%d)", (int)st);
        if (d5) ID3D12Device5_Release(d5);
    }
}

int main(void) {
    HRESULT hr;
    printf("wine-lab d3d12: regression checks for the 2026-10-12/13 ROTTR fixes and the 2026-10-16 stub audit\n\n");
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
    {
        D3D12_COMMAND_QUEUE_DESC qd; ZeroMemory(&qd, sizeof qd);
        D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (alloc) ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list);
        ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsvh);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srvh);
    }
    CHECK(queue && alloc && list && fence && rtvh && dsvh && srvh, "queue, list, fence and descriptor heaps created");
    if (!queue || !alloc || !list || !fence || !rtvh || !dsvh || !srvh) goto done;
    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, IMG, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *rt = make_tex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(rb && rt, "readback buffer and 64x64 RGBA8 render target created");
    if (!rb || !rt) goto done;
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, heap_cpu(rtvh, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 0));

    case_footprints();
    case_null_srv(rt);
    case_step_rate(rt);
    case_rect_clears();
    case_stub_audit(rt);
done:
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails;
}
