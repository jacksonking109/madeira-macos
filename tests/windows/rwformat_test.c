/*  UAV read-after-write on formats Metal does not list as read_write tier 2, for
 *  the wine-lab D3D12 port. The Last of Us Part I uses:
 *    (a) a 256x16x2 3D texture, R8G8B8A8_SNORM, RWTexture3D<float4>: written,
 *        then read through the UAV in the same thread and in a later dispatch;
 *    (b) a 960x600 2D R11G11B10_FLOAT texture, RWTexture2D<float3>: v = t[p];
 *        t[p] = v + delta, in place, over several dispatches (UAV barriers
 *        between) and several ExecuteCommandLists ("frames").
 *  Controls on tier-2 formats with identical logic: (a) R8G8B8A8_UNORM,
 *  (b) R16G16B16A16_FLOAT. Every texel is checked against a CPU reference
 *  (texture copied to a READBACK buffer). Each case also runs an SRV variant:
 *  the same work, but the later dispatches read the texture through an SRV
 *  (Texture3D/Texture2D Load) and write the other of two textures (ping-pong).
 *
 *  Cases (each: path "uav" and path "srv", ITERS iterations):
 *    A_S / A_U     3D 256x16x2 (RGBA8 SNORM / UNORM). Iteration: dispatch wr
 *                  (store A, load it back in the same thread, store rot1 = yzwx),
 *                  readback R1 [checks same-thread RAW]; dispatch rot (UAV load,
 *                  store yzwx again), readback R2 [checks cross-dispatch RAW].
 *                  srv path: w (store A), then srv (SRV Load of T0 -> T1 as yzwx),
 *                  srv (T1 -> T0 as yzwx); T0 should be rot2.
 *    B_R11 / B_F16 960x600 2D. Iteration: 4 frames of 2 "add" dispatches (frame 0
 *                  first runs init), integer deltas, 8 adds; frames are separate
 *                  ExecuteCommandLists signalled without a CPU wait between
 *                  them. srv path: 8 ping-pong steps (SRV read, UAV write).
 *
 *  Prints per case: wrong texels (total, over how many iterations) and first
 *  examples. Exit code = number of cases (paths) with any wrong texel.
 */
#define COBJMACROS
#define INITGUID
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include "rwformat_dxil.h"

static int checks, fails;
#define CHECK(cond, ...) do {                                                  \
    checks++;                                                                  \
    if (!(cond)) { fails++; printf("  FAIL  " __VA_ARGS__); printf("\n"); }     \
    else         { printf("  ok    " __VA_ARGS__); printf("\n"); }             \
    fflush(stdout);                                                            \
} while (0)

typedef HRESULT (WINAPI *pfn_create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef const char *(*pfn_marker)(void);

#define ITERS 50
enum { AW = 256, AH = 16, AD = 2, BW = 960, BH = 600, FRAMES = 4, ADDS_PER_FRAME = 2 };

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc[FRAMES];
static ID3D12GraphicsCommandList *list[FRAMES];
static ID3D12Fence *fence;
static UINT64 fence_v;
static ID3D12DescriptorHeap *gpuh;
static UINT inc;
static int cases_bad;   /* case/paths with any wrong texel */

/* ------------------------------------------------------------------ helpers */

static ID3D12Resource *make_readback(UINT64 size) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                         &IID_ID3D12Resource, (void **)&r);
    return r;
}

static ID3D12Resource *make_tex(D3D12_RESOURCE_DIMENSION dim, UINT w, UINT h, UINT depth, DXGI_FORMAT fmt) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = dim; d.Width = w; d.Height = h;
    d.DepthOrArraySize = (UINT16)depth; d.MipLevels = 1; d.Format = fmt;
    d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                         NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}

static void transition(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x; ZeroMemory(&x, sizeof x);
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a; x.Transition.StateAfter = b;
    ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &x);
}
static void uav_barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r) {
    D3D12_RESOURCE_BARRIER x; ZeroMemory(&x, sizeof x);
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; x.UAV.pResource = r;
    ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &x);
}

static void wait_fence(UINT64 v) {
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, v, ev);
    if (WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0) { printf("        fence wait timed out\n"); fflush(stdout); ExitProcess(99); }
    CloseHandle(ev);
}
/* Close, execute, signal; returns the fence value. */
static UINT64 submit(ID3D12GraphicsCommandList *l) {
    ID3D12CommandList *c[1] = { (ID3D12CommandList *)l };
    HRESULT hr = ID3D12GraphicsCommandList_Close(l);
    if (FAILED(hr)) printf("        list Close failed (%#lx)\n", hr);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, c);
    ID3D12CommandQueue_Signal(queue, fence, ++fence_v);
    return fence_v;
}
static void reset_list(int f) {
    ID3D12CommandAllocator_Reset(alloc[f]);
    ID3D12GraphicsCommandList_Reset(list[f], alloc[f], NULL);
}

static D3D12_CPU_DESCRIPTOR_HANDLE slot_cpu(UINT s) {
    D3D12_CPU_DESCRIPTOR_HANDLE c; gpuh->lpVtbl->GetCPUDescriptorHandleForHeapStart(gpuh, &c); c.ptr += (SIZE_T)s * inc; return c;
}
static D3D12_GPU_DESCRIPTOR_HANDLE slot_gpu(UINT s) {
    D3D12_GPU_DESCRIPTOR_HANDLE g; gpuh->lpVtbl->GetGPUDescriptorHandleForHeapStart(gpuh, &g); g.ptr += (UINT64)s * inc; return g;
}
/* heap: 0 UAV(T0) 1 UAV(T1) 2 SRV(T0) 3 SRV(T1) */
static void put_views(ID3D12Resource *t0, ID3D12Resource *t1, DXGI_FORMAT fmt, BOOL is3d) {
    ID3D12Resource *t[2] = { t0, t1 };
    int i;
    for (i = 0; i < 2; i++) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u; D3D12_SHADER_RESOURCE_VIEW_DESC s;
        ZeroMemory(&u, sizeof u); ZeroMemory(&s, sizeof s);
        u.Format = fmt; s.Format = fmt;
        if (is3d) { u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; u.Texture3D.WSize = AD;
                    s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; s.Texture3D.MipLevels = 1; }
        else      { u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; s.Texture2D.MipLevels = 1; }
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ID3D12Device_CreateUnorderedAccessView(dev, t[i], NULL, &u, slot_cpu(i));
        ID3D12Device_CreateShaderResourceView(dev, t[i], &s, slot_cpu(2 + i));
    }
}

static ID3D12RootSignature *rs;
static ID3D12PipelineState *make_pso(const unsigned char *b, size_t n) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
    ID3D12PipelineState *p = NULL;
    HRESULT hr;
    if (!rs) ID3D12Device_CreateRootSignature(dev, 0, b, n, &IID_ID3D12RootSignature, (void **)&rs);
    pd.pRootSignature = rs; pd.CS.pShaderBytecode = b; pd.CS.BytecodeLength = n;
    hr = ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&p);
    if (FAILED(hr)) printf("        CreateComputePipelineState failed (%#lx)\n", hr);
    return p;
}

/* Binds heap, root signature, pso, constants and the two tables (UAV slot, SRV slot). */
static void bind_cs(ID3D12GraphicsCommandList *l, ID3D12PipelineState *pso, UINT seed, UINT d0, UINT d1, UINT d2,
                 UINT uav_slot, UINT srv_slot) {
    UINT c[4] = { seed, d0, d1, d2 };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 1, &gpuh);
    ID3D12GraphicsCommandList_SetComputeRootSignature(l, rs);
    ID3D12GraphicsCommandList_SetPipelineState(l, pso);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(l, 0, 4, c, 0);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(l, 1, slot_gpu(uav_slot));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(l, 2, slot_gpu(srv_slot));
}

/* texture (UAV state) -> readback buffer at `off`, back to UAV state */
static void copy_out(ID3D12GraphicsCommandList *l, ID3D12Resource *tex, ID3D12Resource *rb, UINT64 off, DXGI_FORMAT fmt,
                     UINT w, UINT h, UINT d, UINT pitch) {
    D3D12_TEXTURE_COPY_LOCATION dl, sl; ZeroMemory(&dl, sizeof dl); ZeroMemory(&sl, sizeof sl);
    dl.pResource = rb; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint.Offset = off;
    dl.PlacedFootprint.Footprint.Format = fmt; dl.PlacedFootprint.Footprint.Width = w;
    dl.PlacedFootprint.Footprint.Height = h; dl.PlacedFootprint.Footprint.Depth = d;
    dl.PlacedFootprint.Footprint.RowPitch = pitch;
    sl.pResource = tex; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    transition(l, tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyTextureRegion(l, &dl, 0, 0, 0, &sl, NULL);
    transition(l, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

static void fill_rb(ID3D12Resource *rb, UINT64 size) {
    BYTE *p = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
    if (p) { memset(p, 0xCD, (size_t)size); ID3D12Resource_Unmap(rb, 0, NULL); }
}

static UINT hsh(UINT a, UINT b) {
    UINT h = a * 0x9e3779b1u ^ b * 0x85ebca6bu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

static float half_to_float(UINT32 h) {
    UINT32 sign = (h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
    float f;
    if (e == 0) { f = (float)m / 16777216.0f; return sign ? -f : f; }
    if (e == 31) x = sign | 0x7f800000 | (m << 13);
    else x = sign | ((e - 15 + 127) << 23) | (m << 13);
    memcpy(&f, &x, 4);
    return f;
}
static float uf(UINT e, UINT m, UINT mbits) {   /* unsigned small float */
    if (e == 0) return ldexpf((float)m / (float)(1u << mbits), -14);
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15);
}

/* result accumulation */
typedef struct { const char *name; long wrong, texels; int bad_iters, iters; char ex[4][160]; int nex; } result_t;
static void note(result_t *r, const char *fmt, ...) {
    va_list ap;
    if (r->nex >= 4) return;
    va_start(ap, fmt); vsnprintf(r->ex[r->nex++], sizeof r->ex[0], fmt, ap); va_end(ap);
}
static void report(result_t *r) {
    int i;
    CHECK(r->wrong == 0, "%-28s %ld wrong of %ld texels checked, in %d of %d iterations", r->name, r->wrong, r->texels, r->bad_iters, r->iters);
    if (r->wrong) cases_bad++;
    for (i = 0; i < r->nex; i++) printf("          e.g. %s\n", r->ex[i]);
    fflush(stdout);
}

/* ------------------------------------------------------------------ case A */

static const UINT A_PITCH = AW * 4;                       /* 1024 */
static const UINT64 A_SLICE = (UINT64)(AW * 4) * AH;      /* 16384 per depth slice */
static const UINT64 A_BYTES = (UINT64)(AW * 4) * AH * AD; /* 32768 */

static void a_ref(BOOL snorm, UINT seed, UINT x, UINT y, UINT z, int a[4]) {
    UINT lin = x + 256u * (y + 16u * z), c;
    for (c = 0; c < 4; c++) {
        UINT h = hsh(seed, lin * 4u + c);
        a[c] = snorm ? (int)(h % 255u) - 127 : (int)(h & 255u);
    }
}

/* compares readback region at `rbp` with rot^`rot`(A); `res` counts */
static void a_check(result_t *r, const BYTE *rbp, BOOL snorm, UINT seed, int rot, const char *tag, int *iter_bad) {
    UINT x, y, z, c;
    for (z = 0; z < AD; z++) for (y = 0; y < AH; y++) for (x = 0; x < AW; x++) {
        int a[4], want[4], got[4], bad = 0;
        const BYTE *p = rbp + z * A_SLICE + (UINT64)y * A_PITCH + x * 4;
        a_ref(snorm, seed, x, y, z, a);
        for (c = 0; c < 4; c++) {
            want[c] = a[(c + rot) & 3];
            got[c] = snorm ? (int)(signed char)p[c] : (int)p[c];
            if (got[c] != want[c]) bad = 1;
        }
        r->texels++;
        if (bad) {
            r->wrong++; (*iter_bad)++;
            note(r, "iter %u %s texel (%u,%u,%u): got (%d,%d,%d,%d) want (%d,%d,%d,%d)  [A=(%d,%d,%d,%d)]", seed, tag, x, y, z,
                 got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3], a[0], a[1], a[2], a[3]);
        }
    }
}

static void run_a(const char *vname, DXGI_FORMAT fmt, BOOL snorm,
                  const unsigned char *wr, size_t nwr, const unsigned char *rot, size_t nrot,
                  const unsigned char *w, size_t nw, const unsigned char *srv, size_t nsrv) {
    ID3D12Resource *t0 = make_tex(D3D12_RESOURCE_DIMENSION_TEXTURE3D, AW, AH, AD, fmt);
    ID3D12Resource *t1 = make_tex(D3D12_RESOURCE_DIMENSION_TEXTURE3D, AW, AH, AD, fmt);
    ID3D12Resource *rb = make_readback(2 * A_BYTES);
    ID3D12PipelineState *p_wr = make_pso(wr, nwr), *p_rot = make_pso(rot, nrot), *p_w = make_pso(w, nw), *p_srv = make_pso(srv, nsrv);
    char n1[64], n2[64], n3[64];
    result_t r1 = {0}, r2 = {0}, r3 = {0};
    UINT it;
    snprintf(n1, sizeof n1, "%s uav same-thread RAW", vname); r1.name = n1;
    snprintf(n2, sizeof n2, "%s uav later-dispatch", vname); r2.name = n2;
    snprintf(n3, sizeof n3, "%s srv path (ping-pong)", vname); r3.name = n3;
    CHECK(t0 && t1 && rb && p_wr && p_rot && p_w && p_srv, "%s: textures, readback and 4 pipelines created", vname);
    if (!(t0 && t1 && rb && p_wr && p_rot && p_w && p_srv)) return;
    put_views(t0, t1, fmt, TRUE);

    for (it = 1; it <= ITERS; it++) {
        ID3D12GraphicsCommandList *l = list[0];
        BYTE *p = NULL;
        int ib1 = 0, ib2 = 0, ib3 = 0;
        /* ---- uav path */
        reset_list(0);
        fill_rb(rb, 2 * A_BYTES);
        bind_cs(l, p_wr, it, 0, 0, 0, 0, 2);
        ID3D12GraphicsCommandList_Dispatch(l, AW / 8, AH / 8, AD);
        uav_barrier(l, t0);
        copy_out(l, t0, rb, 0, fmt, AW, AH, AD, A_PITCH);
        uav_barrier(l, t0);
        bind_cs(l, p_rot, it, 0, 0, 0, 0, 2);
        ID3D12GraphicsCommandList_Dispatch(l, AW / 8, AH / 8, AD);
        uav_barrier(l, t0);
        copy_out(l, t0, rb, A_BYTES, fmt, AW, AH, AD, A_PITCH);
        wait_fence(submit(l));
        ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
        if (!p) { printf("        readback Map failed\n"); return; }
        a_check(&r1, p, snorm, it, 1, "wr", &ib1);
        a_check(&r2, p + A_BYTES, snorm, it, 2, "rot", &ib2);
        ID3D12Resource_Unmap(rb, 0, NULL);
        r1.iters++; r2.iters++; r1.bad_iters += ib1 != 0; r2.bad_iters += ib2 != 0;

        /* ---- srv path: w(T0) ; srv T0->T1 ; srv T1->T0 ; T0 == rot2 */
        reset_list(0);
        fill_rb(rb, A_BYTES);
        bind_cs(l, p_w, it, 0, 0, 0, 0, 2);
        ID3D12GraphicsCommandList_Dispatch(l, AW / 8, AH / 8, AD);
        uav_barrier(l, t0);
        transition(l, t0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        bind_cs(l, p_srv, it, 0, 0, 0, 1, 2);
        ID3D12GraphicsCommandList_Dispatch(l, AW / 8, AH / 8, AD);
        uav_barrier(l, t1);
        transition(l, t0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(l, t1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        bind_cs(l, p_srv, it, 0, 0, 0, 0, 3);
        ID3D12GraphicsCommandList_Dispatch(l, AW / 8, AH / 8, AD);
        uav_barrier(l, t0);
        transition(l, t1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        copy_out(l, t0, rb, 0, fmt, AW, AH, AD, A_PITCH);
        wait_fence(submit(l));
        p = NULL;
        ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
        if (!p) { printf("        readback Map failed\n"); return; }
        a_check(&r3, p, snorm, it, 2, "srv", &ib3);
        ID3D12Resource_Unmap(rb, 0, NULL);
        r3.iters++; r3.bad_iters += ib3 != 0;
    }
    report(&r1); report(&r2); report(&r3);
}

/* ------------------------------------------------------------------ case B */

static void b_expect(BOOL r11, UINT seed, UINT x, UINT y, float want[4]) {
    UINT lin = x + (UINT)BW * y, c, k;
    for (c = 0; c < 4; c++) {
        float v = (float)(hsh(seed, lin * 4u + c) & 7u);
        if (c < 3) for (k = 0; k < FRAMES * ADDS_PER_FRAME; k++) v += (float)((k + c) % 3 + 1);
        want[c] = v;
    }
    (void)r11;
}
/* delta of add number k (0..7) for channel c */
static UINT b_delta(UINT k, UINT c) { return (k + c) % 3 + 1; }

static void b_check(result_t *r, const BYTE *p, BOOL r11, UINT seed, const char *tag, int *iter_bad) {
    UINT x, y, c;
    const UINT pitch = r11 ? BW * 4 : BW * 8;
    for (y = 0; y < BH; y++) for (x = 0; x < BW; x++) {
        float want[4], got[4] = {0};
        int bad = 0, nc = r11 ? 3 : 4;
        b_expect(r11, seed, x, y, want);
        if (r11) {
            UINT32 v; memcpy(&v, p + (size_t)y * pitch + (size_t)x * 4, 4);
            got[0] = uf((v >> 6) & 31u, v & 63u, 6);
            got[1] = uf((v >> 17) & 31u, (v >> 11) & 63u, 6);
            got[2] = uf((v >> 27) & 31u, (v >> 22) & 31u, 5);
        } else {
            UINT16 h[4]; memcpy(h, p + (size_t)y * pitch + (size_t)x * 8, 8);
            for (c = 0; c < 4; c++) got[c] = half_to_float(h[c]);
        }
        for (c = 0; c < (UINT)nc; c++) if (!(got[c] == want[c])) bad = 1;
        r->texels++;
        if (bad) {
            r->wrong++; (*iter_bad)++;
            note(r, "iter %u %s texel (%u,%u): got (%g,%g,%g%s) want (%g,%g,%g%s)", seed, tag, x, y,
                 got[0], got[1], got[2], r11 ? "" : ",", want[0], want[1], want[2], r11 ? "" : ",");
            if (!r11) { size_t n = strlen(r->ex[r->nex - 1]); (void)n; }
        }
    }
}

static void run_b(const char *vname, DXGI_FORMAT fmt, BOOL r11,
                  const unsigned char *init, size_t ninit, const unsigned char *add, size_t nadd,
                  const unsigned char *sadd, size_t nsadd) {
    ID3D12Resource *t0 = make_tex(D3D12_RESOURCE_DIMENSION_TEXTURE2D, BW, BH, 1, fmt);
    ID3D12Resource *t1 = make_tex(D3D12_RESOURCE_DIMENSION_TEXTURE2D, BW, BH, 1, fmt);
    const UINT pitch = r11 ? BW * 4 : BW * 8;
    const UINT64 bytes = (UINT64)pitch * BH;
    ID3D12Resource *rb = make_readback(bytes);
    ID3D12PipelineState *p_init = make_pso(init, ninit), *p_add = make_pso(add, nadd), *p_sadd = make_pso(sadd, nsadd);
    char n1[64], n2[64];
    result_t r1 = {0}, r2 = {0};
    UINT it, f, j, k;
    snprintf(n1, sizeof n1, "%s uav in-place RMW", vname); r1.name = n1;
    snprintf(n2, sizeof n2, "%s srv path (ping-pong)", vname); r2.name = n2;
    CHECK(t0 && t1 && rb && p_init && p_add && p_sadd, "%s: textures, readback and 3 pipelines created", vname);
    if (!(t0 && t1 && rb && p_init && p_add && p_sadd)) return;
    put_views(t0, t1, fmt, FALSE);

    for (it = 1; it <= ITERS; it++) {
        BYTE *p = NULL;
        int ib1 = 0, ib2 = 0;
        UINT64 last = 0;
        /* ---- uav path: 4 frames, separate ExecuteCommandLists, no CPU wait between */
        for (f = 0; f < FRAMES; f++) reset_list(f);   /* all allocators idle: previous iteration waited */
        fill_rb(rb, bytes);
        for (f = 0; f < FRAMES; f++) {
            ID3D12GraphicsCommandList *l = list[f];
            if (f == 0) {
                bind_cs(l, p_init, it, 0, 0, 0, 0, 2);
                ID3D12GraphicsCommandList_Dispatch(l, BW / 8, BH / 8, 1);
                uav_barrier(l, t0);
            }
            for (j = 0; j < ADDS_PER_FRAME; j++) {
                k = f * ADDS_PER_FRAME + j;
                bind_cs(l, p_add, it, b_delta(k, 0), b_delta(k, 1), b_delta(k, 2), 0, 2);
                ID3D12GraphicsCommandList_Dispatch(l, BW / 8, BH / 8, 1);
                uav_barrier(l, t0);
            }
            if (f == FRAMES - 1) copy_out(l, t0, rb, 0, fmt, BW, BH, 1, pitch);
            last = submit(l);
        }
        wait_fence(last);
        ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
        if (!p) { printf("        readback Map failed\n"); return; }
        b_check(&r1, p, r11, it, "uav", &ib1);
        ID3D12Resource_Unmap(rb, 0, NULL);
        r1.iters++; r1.bad_iters += ib1 != 0;

        /* ---- srv path: init T0, then 8 steps src->dst alternating (T0->T1, T1->T0, ...) */
        for (f = 0; f < FRAMES; f++) reset_list(f);
        fill_rb(rb, bytes);
        for (f = 0; f < FRAMES; f++) {
            ID3D12GraphicsCommandList *l = list[f];
            if (f == 0) {
                bind_cs(l, p_init, it, 0, 0, 0, 0, 2);
                ID3D12GraphicsCommandList_Dispatch(l, BW / 8, BH / 8, 1);
                uav_barrier(l, t0);
            }
            for (j = 0; j < ADDS_PER_FRAME; j++) {
                k = f * ADDS_PER_FRAME + j;
                ID3D12Resource *src = (k & 1) ? t1 : t0, *dst = (k & 1) ? t0 : t1;
                transition(l, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                bind_cs(l, p_sadd, it, b_delta(k, 0), b_delta(k, 1), b_delta(k, 2), (k & 1) ? 0 : 1, (k & 1) ? 3 : 2);
                ID3D12GraphicsCommandList_Dispatch(l, BW / 8, BH / 8, 1);
                uav_barrier(l, dst);
                transition(l, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            if (f == FRAMES - 1) copy_out(l, t0, rb, 0, fmt, BW, BH, 1, pitch);   /* 8 steps: result back in T0 */
            last = submit(l);
        }
        wait_fence(last);
        p = NULL;
        ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
        if (!p) { printf("        readback Map failed\n"); return; }
        b_check(&r2, p, r11, it, "srv", &ib2);
        ID3D12Resource_Unmap(rb, 0, NULL);
        r2.iters++; r2.bad_iters += ib2 != 0;
    }
    report(&r1); report(&r2);
}

/* ------------------------------------------------------------------ main */

#define BLOB(n) n, sizeof n

static void print_caps(void) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS o; ZeroMemory(&o, sizeof o);
    struct { const char *n; DXGI_FORMAT f; } fm[] = {
        { "R8G8B8A8_SNORM", DXGI_FORMAT_R8G8B8A8_SNORM }, { "R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM },
        { "R11G11B10_FLOAT", DXGI_FORMAT_R11G11B10_FLOAT }, { "R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT } };
    int i;
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o)))
        printf("        caps: TypedUAVLoadAdditionalFormats=%d\n", (int)o.TypedUAVLoadAdditionalFormats);
    for (i = 0; i < 4; i++) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT s; ZeroMemory(&s, sizeof s); s.Format = fm[i].f;
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof s)))
            printf("        caps: %-20s UAV_TYPED_STORE=%d UAV_TYPED_LOAD=%d\n", fm[i].n,
                   !!(s.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE), !!(s.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD));
    }
}

int main(void) {
    HRESULT hr;
    int k;
    printf("wine-lab d3d12: UAV read-after-write on SNORM 3D and R11G11B10 2D, vs tier-2 controls (%d iterations)\n\n", ITERS);

    HMODULE d12 = LoadLibraryA("d3d12.dll");
    pfn_create_device create = d12 ? (pfn_create_device)GetProcAddress(d12, "D3D12CreateDevice") : NULL;
    CHECK(create != NULL, "d3d12.dll loaded and D3D12CreateDevice resolved");
    if (!create) goto done;
    pfn_marker marker = (pfn_marker)GetProcAddress(d12, "MadeiraD3D12GetBuildMarker");
    if (marker) printf("        implementation: %s\n", marker());

    hr = create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    CHECK(SUCCEEDED(hr) && dev, "device created (%#lx)", hr);
    if (!dev) goto done;
    {
        D3D12_COMMAND_QUEUE_DESC qd; ZeroMemory(&qd, sizeof qd);
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
    }
    for (k = 0; k < FRAMES; k++) {
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc[k]);
        if (alloc[k]) {
            ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[k], NULL, &IID_ID3D12GraphicsCommandList, (void **)&list[k]);
            if (list[k]) ID3D12GraphicsCommandList_Close(list[k]);
        }
    }
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&gpuh);
    }
    inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CHECK(queue && alloc[0] && list[0] && list[FRAMES - 1] && fence && gpuh, "queue, %d lists, fence and shader-visible heap created", FRAMES);
    if (!queue || !list[FRAMES - 1] || !fence || !gpuh) goto done;
    print_caps();
    printf("\n");

    run_a("A_S R8G8B8A8_SNORM 3D", DXGI_FORMAT_R8G8B8A8_SNORM, TRUE,
          BLOB(rwf_A_S_wr_dxil), BLOB(rwf_A_S_rot_dxil), BLOB(rwf_A_S_w_dxil), BLOB(rwf_A_S_srv_dxil));
    run_a("A_U R8G8B8A8_UNORM 3D", DXGI_FORMAT_R8G8B8A8_UNORM, FALSE,
          BLOB(rwf_A_U_wr_dxil), BLOB(rwf_A_U_rot_dxil), BLOB(rwf_A_U_w_dxil), BLOB(rwf_A_U_srv_dxil));
    run_b("B_R11 R11G11B10 2D", DXGI_FORMAT_R11G11B10_FLOAT, TRUE,
          BLOB(rwf_B_R11_init_dxil), BLOB(rwf_B_R11_add_dxil), BLOB(rwf_B_R11_srv_add_dxil));
    run_b("B_F16 R16G16B16A16F 2D", DXGI_FORMAT_R16G16B16A16_FLOAT, FALSE,
          BLOB(rwf_B_F16_init_dxil), BLOB(rwf_B_F16_add_dxil), BLOB(rwf_B_F16_srv_add_dxil));

done:
    printf("\n%d checks, %d failed; %d case-paths with wrong texels\n", checks, fails, cases_bad);
    fflush(stdout);
    return fails;
}
