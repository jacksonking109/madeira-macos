/*  DXBC (shader model 5.x) test for the wine-lab D3D12 port.
 *
 *  Every other test here hands the runtime precompiled DXIL. Most D3D12 games
 *  that matter on this port (and every D3D11-era engine that grew a D3D12
 *  backend) still ship SM 5.x DXBC, which the runtime routes through DXMT's
 *  airconv rather than Apple's converter. This test exercises that path with
 *  shaders compiled at run time by Wine's own d3dcompiler_47 (vs_5_0 / ps_5_0),
 *  so there is no embedded blob that could be stale.
 *
 *  Cases (each a fresh draw into a cleared 64x64 RGBA8 target, read back, and
 *  the centre pixel compared):
 *    1  solid colour = root constants (b0) + root CBV (b1); neither source alone
 *       produces the expected value
 *    2  Texture2D sampled through an SRV descriptor table (non-zero heap slot)
 *       with a static sampler, tinted by a root CBV
 *    3  vertex buffer + input layout (POSITION float3 @0, COLOR float4 @12,
 *       stride 28); a corner outside the triangle must keep the clear colour
 *
 *  Until the runtime's DXBC backend is built in, pipeline creation is EXPECTED
 *  to fail ("DXBC shaders need the airconv backend"). That is reported as a
 *  failed check and the draw is skipped; nothing crashes.
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
typedef const char *(*pfn_marker)(void);

enum { RT_W = 64, RT_H = 64, TEX_W = 64, TEX_H = 64 };
static const UINT64 PIX_SIZE = (UINT64)RT_W * RT_H * 4;   /* row pitch 256: already aligned */

static pD3DCompile         p_compile;
static pfn_serialize_rs    p_serialize;
static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;
static ID3D12Resource     *rt, *pix;
static ID3D12DescriptorHeap *rtvh;
static D3D12_CPU_DESCRIPTOR_HANDLE rtv;
static int pso_failures;

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

/* Bind + clear the shared render target and set a full viewport and scissor
 * (a D3D12 scissor that is never set is zero-sized and clips everything). */
static void begin_pass(const FLOAT clear[4]) {
    D3D12_VIEWPORT vp; ZeroMemory(&vp, sizeof vp);
    vp.Width = (FLOAT)RT_W; vp.Height = (FLOAT)RT_H; vp.MaxDepth = 1.0f;
    D3D12_RECT sc = { 0, 0, RT_W, RT_H };
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
}

/* RT -> readback buffer, then submit and pull out the centre and a corner. */
static BOOL finish_and_read(BYTE centre[4], BYTE corner[4]) {
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
    memcpy(corner, p + (2 * RT_W + 2) * 4, 4);
    ID3D12Resource_Unmap(pix, 0, NULL);
    return TRUE;
}

static BOOL px_near(const BYTE got[4], int r, int g, int b, int a) {
    return abs(got[0] - r) <= 2 && abs(got[1] - g) <= 2 && abs(got[2] - b) <= 2 && abs(got[3] - a) <= 2;
}

/* Compiles one entry point to SM 5.0 DXBC; prints the error blob on failure. */
static ID3DBlob *compile(const char *name, const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = p_compile(src, strlen(src), name, NULL, NULL, entry, target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err) {
        const char *msg = (const char *)ID3D10Blob_GetBufferPointer(err);
        SIZE_T n = ID3D10Blob_GetBufferSize(err);
        if (n) printf("        %s %s/%s compiler output:\n%.*s\n", name, entry, target, (int)n, msg);
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

static ID3D12RootSignature *make_rs(const char *name, const D3D12_ROOT_SIGNATURE_DESC *rd) {
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature *rs = NULL;
    HRESULT hr = p_serialize(rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err);
    if (err) {
        printf("        serializer: %.*s\n", (int)ID3D10Blob_GetBufferSize(err),
               (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    if (SUCCEEDED(hr) && blob)
        hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob),
                                              ID3D10Blob_GetBufferSize(blob),
                                              &IID_ID3D12RootSignature, (void **)&rs);
    if (blob) ID3D10Blob_Release(blob);
    CHECK(SUCCEEDED(hr) && rs, "%s: root signature serialized and created (%#lx)", name, hr);
    return rs;
}

static ID3D12PipelineState *make_pso(const char *name, ID3D12RootSignature *rs, ID3DBlob *vs, ID3DBlob *ps,
                                     const D3D12_INPUT_ELEMENT_DESC *il, UINT nil) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
    ID3D12PipelineState *pso = NULL;
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = nil;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    test_pso_defaults(&pd, FALSE);   /* real blend/raster defaults; a zeroed desc writes nothing */
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    CHECK(SUCCEEDED(hr) && pso, "%s: graphics PSO created from SM 5.0 DXBC (%#lx)", name, hr);
    if (FAILED(hr) || !pso) {
        pso_failures++;
        printf("        (DXBC PSO refused: draw skipped. Expected until the runtime's airconv DXBC backend is built in.)\n");
        return NULL;
    }
    return pso;
}

/* Fullscreen triangle from SV_VertexID: no vertex buffer, no input layout. */
#define FULLSCREEN_VS_BODY                                                        \
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"                            \
    "    float4 pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"

/* ------------------------------------------------------------------- case 1 */

static const char hlsl_solid[] =
    "cbuffer RootConsts : register(b0) { float4 rc; };\n"
    "cbuffer RootCBV    : register(b1) { float4 cb; };\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    FULLSCREEN_VS_BODY
    "    return pos;\n"
    "}\n"
    "float4 ps_main(float4 p : SV_Position) : SV_Target { return rc + cb; }\n";

static void case_solid(void) {
    printf("\n[1] solid colour: root constants (b0) + root CBV (b1)\n");
    ID3DBlob *vs = compile("solid", hlsl_solid, "vs_main", "vs_5_0");
    ID3DBlob *ps = compile("solid", hlsl_solid, "ps_main", "ps_5_0");

    D3D12_ROOT_PARAMETER rp[2]; ZeroMemory(rp, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0; rp[0].Constants.Num32BitValues = 4;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].Descriptor.ShaderRegister = 1;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd; ZeroMemory(&rd, sizeof rd);
    rd.NumParameters = 2; rd.pParameters = rp;
    ID3D12RootSignature *rs = make_rs("solid", &rd);

    ID3D12PipelineState *pso = (rs && vs && ps) ? make_pso("solid", rs, vs, ps, NULL, 0) : NULL;

    /* CBV lives 256 bytes into its buffer so a binding that drops the offset
     * reads the sentinel instead. */
    ID3D12Resource *cbuf = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 512, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (pso && cbuf) {
        const float rc[4] = { 0.25f, 0.0f, 0.5f, 0.5f };
        const float cb[4] = { 0.5f, 0.25f, 0.0f, 0.5f };
        float *m = NULL;
        ID3D12Resource_Map(cbuf, 0, NULL, (void **)&m);
        if (m) {
            for (int i = 0; i < 64; i++) m[i] = 0.9f;   /* sentinel */
            memcpy(m + 64, cb, sizeof cb);
            ID3D12Resource_Unmap(cbuf, 0, NULL);
        }
        const FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        begin_pass(clear);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, rc, 0);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 1,
                ID3D12Resource_GetGPUVirtualAddress(cbuf) + 256);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        BYTE c[4] = {0}, k[4] = {0};
        BOOL ok = finish_and_read(c, k);
        CHECK(ok, "solid: draw submitted and read back");
        /* 0.75,0.25,0.5,1.0: root constants alone give (64,0,128,128), CBV alone (128,64,0,128) */
        CHECK(ok && px_near(c, 191, 64, 128, 255),
              "solid: centre = root constants + CBV: got (%u,%u,%u,%u), expected (191,64,128,255)",
              c[0], c[1], c[2], c[3]);
    }
    if (cbuf) ID3D12Resource_Release(cbuf);
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
}

/* ------------------------------------------------------------------- case 2 */

static const char hlsl_tex[] =
    "cbuffer Tint : register(b0) { float4 tint; };\n"
    "Texture2D    tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs_main(uint id : SV_VertexID) {\n"
    FULLSCREEN_VS_BODY
    "    V o; o.pos = pos; o.uv = uv; return o;\n"
    "}\n"
    "float4 ps_main(V i) : SV_Target { return tex.Sample(smp, i.uv) * tint; }\n";

static void case_texture(void) {
    printf("\n[2] texture sample: SRV descriptor table + static sampler, tinted by root CBV\n");
    ID3DBlob *vs = compile("texture", hlsl_tex, "vs_main", "vs_5_0");
    ID3DBlob *ps = compile("texture", hlsl_tex, "ps_main", "ps_5_0");

    D3D12_DESCRIPTOR_RANGE range; ZeroMemory(&range, sizeof range);
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1; range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER rp[2]; ZeroMemory(rp, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss; ZeroMemory(&ss, sizeof ss);
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxAnisotropy = 1;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ss.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd; ZeroMemory(&rd, sizeof rd);
    rd.NumParameters = 2; rd.pParameters = rp;
    rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
    ID3D12RootSignature *rs = make_rs("texture", &rd);

    ID3D12PipelineState *pso = (rs && vs && ps) ? make_pso("texture", rs, vs, ps, NULL, 0) : NULL;

    const UINT64 TEX_BYTES = (UINT64)TEX_W * TEX_H * 4;   /* row pitch 256 */
    ID3D12Resource *tex = make_tex2d(TEX_W, TEX_H, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *up = make_buffer(D3D12_HEAP_TYPE_UPLOAD, TEX_BYTES, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *cbuf = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12DescriptorHeap *srvh = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srvh);
    CHECK(tex && up && cbuf && srvh, "texture: texture, upload buffer, CB and SRV heap created");

    if (pso && tex && up && cbuf && srvh) {
        const BYTE TR = 200, TG = 100, TB = 50;
        BYTE *t = NULL;
        ID3D12Resource_Map(up, 0, NULL, (void **)&t);
        if (t) {
            for (UINT64 i = 0; i < TEX_BYTES; i += 4) { t[i] = TR; t[i+1] = TG; t[i+2] = TB; t[i+3] = 255; }
            ID3D12Resource_Unmap(up, 0, NULL);
        }
        float *m = NULL;
        ID3D12Resource_Map(cbuf, 0, NULL, (void **)&m);
        if (m) { m[0] = 0.5f; m[1] = 1.0f; m[2] = 1.0f; m[3] = 1.0f; ID3D12Resource_Unmap(cbuf, 0, NULL); }

        /* SRV at heap slot 1, not 0, so a table base offset that is ignored shows. */
        UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE sh; D3D12_GPU_DESCRIPTOR_HANDLE sg;
        srvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(srvh, &sh);
        srvh->lpVtbl->GetGPUDescriptorHandleForHeapStart(srvh, &sg);
        sh.ptr += inc; sg.ptr += inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv; ZeroMemory(&sv, sizeof sv);
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        ID3D12Device_CreateShaderResourceView(dev, tex, &sv, sh);

        D3D12_TEXTURE_COPY_LOCATION dl, sl; ZeroMemory(&dl, sizeof dl); ZeroMemory(&sl, sizeof sl);
        dl.pResource = tex; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.pResource = up; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sl.PlacedFootprint.Footprint.Width = TEX_W; sl.PlacedFootprint.Footprint.Height = TEX_H;
        sl.PlacedFootprint.Footprint.Depth = 1; sl.PlacedFootprint.Footprint.RowPitch = TEX_W * 4;
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dl, 0, 0, 0, &sl, NULL);
        barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        const FLOAT clear[4] = { 0.0f, 0.0f, 1.0f, 1.0f };   /* blue: not a possible result */
        begin_pass(clear);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &srvh);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, ID3D12Resource_GetGPUVirtualAddress(cbuf));
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, sg);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        BYTE c[4] = {0}, k[4] = {0};
        BOOL ok = finish_and_read(c, k);
        CHECK(ok, "texture: upload + draw submitted and read back");
        CHECK(ok && px_near(c, 100, 100, 50, 255),
              "texture: centre = texel (200,100,50) * tint (0.5,1,1): got (%u,%u,%u,%u), expected (100,100,50,255)",
              c[0], c[1], c[2], c[3]);
        CHECK(ok && !(c[0] == TR && c[1] == TG && c[2] == TB), "texture: tint CB was applied (not the raw texel)");
    }
    if (srvh) ID3D12DescriptorHeap_Release(srvh);
    if (cbuf) ID3D12Resource_Release(cbuf);
    if (up) ID3D12Resource_Release(up);
    if (tex) ID3D12Resource_Release(tex);
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
}

/* ------------------------------------------------------------------- case 3 */

static const char hlsl_vb[] =
    "struct VI { float3 pos : POSITION; float4 col : COLOR; };\n"
    "struct VO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VO vs_main(VI i) { VO o; o.pos = float4(i.pos, 1.0); o.col = i.col; return o; }\n"
    "float4 ps_main(VO i) : SV_Target { return i.col; }\n";

struct vtx { float pos[3]; float col[4]; };   /* stride 28 */

static void case_vertex_buffer(void) {
    printf("\n[3] input assembler: vertex buffer with POSITION + COLOR layout\n");
    ID3DBlob *vs = compile("vertexbuf", hlsl_vb, "vs_main", "vs_5_0");
    ID3DBlob *ps = compile("vertexbuf", hlsl_vb, "ps_main", "ps_5_0");

    D3D12_ROOT_SIGNATURE_DESC rd; ZeroMemory(&rd, sizeof rd);
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3D12RootSignature *rs = make_rs("vertexbuf", &rd);

    D3D12_INPUT_ELEMENT_DESC il[2]; ZeroMemory(il, sizeof il);
    il[0].SemanticName = "POSITION"; il[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    il[0].AlignedByteOffset = 0; il[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    il[1].SemanticName = "COLOR"; il[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    il[1].AlignedByteOffset = 12; il[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    ID3D12PipelineState *pso = (rs && vs && ps) ? make_pso("vertexbuf", rs, vs, ps, il, 2) : NULL;

    /* A triangle around the centre that leaves the corners uncovered. Uniform
     * colour, so the centre is exact rather than an interpolation. */
    const struct vtx tri[3] = {
        { { -0.5f, -0.5f, 0.0f }, { 0.2f, 0.6f, 0.4f, 1.0f } },
        { {  0.0f,  0.5f, 0.0f }, { 0.2f, 0.6f, 0.4f, 1.0f } },
        { {  0.5f, -0.5f, 0.0f }, { 0.2f, 0.6f, 0.4f, 1.0f } },
    };
    ID3D12Resource *vb = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
    CHECK(vb != NULL, "vertexbuf: vertex buffer created");

    if (pso && vb) {
        BYTE *m = NULL;
        ID3D12Resource_Map(vb, 0, NULL, (void **)&m);
        if (m) { memcpy(m, tri, sizeof tri); ID3D12Resource_Unmap(vb, 0, NULL); }
        D3D12_VERTEX_BUFFER_VIEW vbv;
        vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb);
        vbv.SizeInBytes = sizeof tri;
        vbv.StrideInBytes = sizeof(struct vtx);

        const FLOAT clear[4] = { 1.0f, 0.0f, 1.0f, 1.0f };   /* magenta */
        begin_pass(clear);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        BYTE c[4] = {0}, k[4] = {0};
        BOOL ok = finish_and_read(c, k);
        CHECK(ok, "vertexbuf: draw submitted and read back");
        CHECK(ok && px_near(c, 51, 153, 102, 255),
              "vertexbuf: centre = vertex COLOR: got (%u,%u,%u,%u), expected (51,153,102,255)",
              c[0], c[1], c[2], c[3]);
        CHECK(ok && px_near(k, 255, 0, 255, 255),
              "vertexbuf: corner outside the triangle kept the clear colour: got (%u,%u,%u,%u)",
              k[0], k[1], k[2], k[3]);
    }
    if (vb) ID3D12Resource_Release(vb);
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
}

/* --------------------------------------------------------------------- main */

int main(void) {
    HRESULT hr;
    printf("wine-lab d3d12: DXBC (SM 5.0) shader test\n\n");

    HMODULE dcc = LoadLibraryA("d3dcompiler_47.dll");
    p_compile = dcc ? (pD3DCompile)GetProcAddress(dcc, "D3DCompile") : NULL;
    CHECK(p_compile != NULL, "d3dcompiler_47.dll loaded and D3DCompile resolved (err %lu)",
          p_compile ? 0UL : GetLastError());

    HMODULE d12 = LoadLibraryA("d3d12.dll");
    pfn_create_device create = d12 ? (pfn_create_device)GetProcAddress(d12, "D3D12CreateDevice") : NULL;
    p_serialize = d12 ? (pfn_serialize_rs)GetProcAddress(d12, "D3D12SerializeRootSignature") : NULL;
    CHECK(create && p_serialize, "d3d12.dll loaded, D3D12CreateDevice + D3D12SerializeRootSignature resolved");
    if (!p_compile || !create || !p_serialize) goto done;
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

    case_solid();
    case_texture();
    case_vertex_buffer();

    ID3D12DescriptorHeap_Release(rtvh);
    ID3D12Resource_Release(pix);
    ID3D12Resource_Release(rt);
    ID3D12Fence_Release(fence);
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(alloc);
    ID3D12CommandQueue_Release(queue);
    ID3D12Device_Release(dev);

done:
    if (pso_failures)
        printf("\nnote: %d DXBC pipeline(s) refused -- the runtime has no DXBC (airconv) backend yet\n", pso_failures);
    printf("\n%d checks, %d failures\n", checks, fails);
    fflush(stdout);
    return fails;
}
