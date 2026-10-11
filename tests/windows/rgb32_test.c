/*  RGB32 typed buffer views for the wine-lab D3D12 port.
 *
 *  Metal has no 3-channel 32-bit texture-buffer format, so the runtime serves
 *  a typed SRV of R32G32B32_FLOAT / _UINT / _SINT from a hidden RGBA32 shadow
 *  of the viewed range (w = 1), refreshed on the GPU after a barrier makes the
 *  source readable, at the end of a list whose copies wrote it, at the first
 *  list after the view is made, and at every list start for an UPLOAD source
 *  (madeira_d3d12.c, struct mad_shadow). Before that, typed loads read zeros
 *  (TLOU1's mesh ray caster: "Invalid result data for ray 0").
 *
 *  The kernel (rgb32.hlsl, DXIL in rgb32_dxil.h) reads element t of one view,
 *  Buffer<float4> / <uint4> / <int4> from an unbounded descriptor array, and
 *  writes the four raw dwords out. Every value is compared bit for bit; w must
 *  be 1.0f (float) or 1 (integers).
 *
 *  Cases:
 *    1  DEFAULT buffer filled by CopyBufferRegion from UPLOAD, barrier, read
 *       (40000 elements: more than one 16384-row refresh chunk)
 *    2  a FirstElement offset (37)
 *    3  UINT and SINT views
 *    4  a second copy + barrier in the same list: the read after it sees the
 *       new data, the read before it the old (both the full and offset views)
 *    5  a view made after its buffer already holds data
 *    6  a copy in one list and the read in the next, with no barrier at all
 *       (implicit promotion from COMMON)
 *    7  a view over an UPLOAD buffer the CPU rewrites between submissions
 *    8  an UPLOAD ring of 3 per-frame slots rewritten by the CPU every frame
 *       with 3 frames in flight (no wait between submissions), read through
 *       one view per slot AND one view over the whole ring at the slot's
 *       offset (a shadow shared by all frames in flight)
 *    9  refresh cost: 80 small views and one whole-buffer view of one 12 MB
 *       buffer; 40 small copies, each made readable by a barrier and read,
 *       must copy exactly the bytes those copies wrote that views cover (the
 *       runtime's MadeiraD3D12ShadowStats), not every view each time; two
 *       UAV -> SRV barriers with no work between refresh the views once
 *   10  a SINT view of a range that already has a UINT view shares its shadow
 *       (no new memory) and reads the same bits; two shader-write barriers
 *       with a dispatch between, in one frame, refresh the big (12 MB) view
 *       once and the small views twice; the next frame refreshes it again
 *       (frames are ExecuteCommandLists calls here: the test never presents).
 *       With madeira.cfg rgb32-big-exact = 1 the big view is refreshed twice.
 *
 *  Exit code = number of failed checks. Every dispatch is finite.
 */
#define COBJMACROS
#define INITGUID
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rgb32_dxil.h"

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

enum { NF = 40000, NI = 4096, NN = 512, NP = 256, OFF = 37, NOFF = 1000, LATE_FIRST = 100, NLATE = 300 };
enum { NR = 3000, NSLOT = 3, NFRAMES = 24, NREAD = 4 };   /* case 8: elements per slot, slots = frames in flight */
enum { K_FLOAT, K_UINT, K_SINT };
/* heap slots */
enum { S_F, S_FOFF, S_U, S_I, S_P, S_N, S_LATE, S_RING, S_SLOT0, S_BIG0 = S_SLOT0 + NSLOT, NBIGV = 80, S_WHOLE = S_BIG0 + NBIGV, S_USI, S_COUNT };
enum { NG = 1 << 20, NCOPY = 40, CPYN = 1200 };   /* case 9: big buffer elements, copies, elements per copy */
static UINT big_first(UINT k) { return k * 12007u + 5u; }
static UINT big_num(UINT k) { return 100u + 13u * k; }
struct shadow_stats { UINT64 copied, viewed, skips, big_skips, live, live_bytes, big_exact, big_bytes; };
typedef void (*pfn_shadow_stats)(struct shadow_stats *);
static pfn_shadow_stats p_stats;
#define OUT_REGION ((UINT64)NF * 16)   /* one dispatch's output */
#define ONE_F 0x3f800000u

static pfn_serialize_v     p_serialize;
static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;
static ID3D12DescriptorHeap *heap;
static UINT                inc;
static ID3D12Resource     *stage, *fbuf, *ubuf, *ibuf, *nbuf, *pbuf, *ring, *outbuf, *rb, *gbuf, *gstage;
#define NREGIONS (2 * NSLOT)
static ID3D12RootSignature *rs;
static ID3D12PipelineState *pso;

/* staging layout (UPLOAD): data A, B (floats, NF each), U, I (NI), C (NN) */
#define ST_A 0
#define ST_B (ST_A + (UINT64)NF * 12)
#define ST_U (ST_B + (UINT64)NF * 12)
#define ST_I (ST_U + (UINT64)NI * 12)
#define ST_C (ST_I + (UINT64)NI * 12)
#define ST_SIZE (ST_C + (UINT64)NN * 12)

static float fval(UINT i, UINT c, float seed) { return (float)(i * 3 + c) * 0.5f + seed; }
static UINT fbits(UINT i, UINT c, float seed) { float f = fval(i, c, seed); UINT u; memcpy(&u, &f, 4); return u; }
static UINT uval(UINT i, UINT c) { return 0x80000000u | (i * 7 + c * 1000003u); }
static INT ival(UINT i, UINT c) { return -(INT)(i * 3 + c) - 1000000; }

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE heap_type, UINT64 size, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = heap_type;
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

/* b0: 3 constants (n, sel, kind); three unbounded SRV tables (space0..2) over
 * the same heap; u0: root UAV. */
static ID3D12RootSignature *make_rs(void) {
    D3D12_DESCRIPTOR_RANGE1 r[3]; D3D12_ROOT_PARAMETER1 p[5];
    UINT k;
    ZeroMemory(r, sizeof r); ZeroMemory(p, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.Num32BitValues = 4;
    for (k = 0; k < 3; k++) {
        r[k].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r[k].NumDescriptors = UINT_MAX;
        r[k].BaseShaderRegister = 0; r[k].RegisterSpace = k;
        r[k].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        p[1 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1 + k].DescriptorTable.NumDescriptorRanges = 1; p[1 + k].DescriptorTable.pDescriptorRanges = &r[k];
    }
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v; ZeroMemory(&v, sizeof v);
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    v.Desc_1_1.NumParameters = 5; v.Desc_1_1.pParameters = p;
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature *o = NULL;
    HRESULT hr = p_serialize(&v, &blob, &err);
    if (err) ID3D10Blob_Release(err);
    if (SUCCEEDED(hr) && blob)
        hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                              &IID_ID3D12RootSignature, (void **)&o);
    if (blob) ID3D10Blob_Release(blob);
    CHECK(SUCCEEDED(hr) && o, "root signature (constants, 3 unbounded SRV tables, root UAV) created (%#lx)", hr);
    return o;
}

static void make_view(UINT slot, ID3D12Resource *r, DXGI_FORMAT f, UINT first, UINT num) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d; ZeroMemory(&d, sizeof d);
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &h);   /* aggregate return: the vtbl form */
    d.Format = f; d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.FirstElement = first; d.Buffer.NumElements = num;
    h.ptr += (SIZE_T)slot * inc;
    ID3D12Device_CreateShaderResourceView(dev, r, &d, h);
}

/* Records one dispatch reading `n` elements of heap slot `sel` into output region `region`. */
static void read_view_at(UINT kind, UINT sel, UINT base, UINT n, UINT region) {
    const UINT c[4] = { n, sel, kind, base };
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &g);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, 4, c, 0);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, g);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 2, g);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 3, g);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 4, ID3D12Resource_GetGPUVirtualAddress(outbuf) + region * OUT_REGION);
    ID3D12GraphicsCommandList_Dispatch(list, (n + 63) / 64, 1, 1);
}
static void read_view(UINT kind, UINT sel, UINT n, UINT region) { read_view_at(kind, sel, 0, n, region); }

/* Copies `nregions` output regions to the readback buffer, submits and waits. */
static BOOL finish(UINT nregions) {
    barrier(outbuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, rb, 0, outbuf, 0, nregions * OUT_REGION);
    barrier(outbuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return submit_and_wait();
}

/* Expected element i of a view: kind, data set (float seed), first element. */
struct expect { UINT kind; float seed; UINT first; };
static void expected(const struct expect *x, UINT t, UINT v[4]) {
    UINT i = x->first + t, c;
    for (c = 0; c < 3; c++)
        v[c] = x->kind == K_FLOAT ? fbits(i, c, x->seed) : x->kind == K_UINT ? uval(i, c) : (UINT)ival(i, c);
    v[3] = x->kind == K_FLOAT ? ONE_F : 1u;
}
static void verify(const char *what, UINT region, UINT n, const struct expect *x) {
    const UINT *o = NULL; UINT t, bad = 0, zero_w = 0;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&o);
    if (!o) { CHECK(0, "%s: readback mapped", what); return; }
    o += (SIZE_T)region * (OUT_REGION / 4);
    for (t = 0; t < n; t++) {
        UINT v[4];
        expected(x, t, v);
        if (memcmp(&o[4 * t], v, 16)) {
            if (!o[4 * t + 3]) zero_w++;
            if (!bad++)
                printf("        element %u: got (%#x %#x %#x %#x), expected (%#x %#x %#x %#x)\n", t,
                       o[4 * t], o[4 * t + 1], o[4 * t + 2], o[4 * t + 3], v[0], v[1], v[2], v[3]);
        }
    }
    ID3D12Resource_Unmap(rb, 0, NULL);
    CHECK(!bad, "%s: %u elements exact with w = 1 (%u wrong%s)", what, n, bad, zero_w ? ", w = 0 in some: raw view" : "");
}

static void fill_stage(void) {
    BYTE *p = NULL; UINT i, c;
    ID3D12Resource_Map(stage, 0, NULL, (void **)&p);
    for (i = 0; i < NF; i++) for (c = 0; c < 3; c++) {
        ((float *)(p + ST_A))[3 * i + c] = fval(i, c, 0.25f);
        ((float *)(p + ST_B))[3 * i + c] = fval(i, c, -7.0f);
    }
    for (i = 0; i < NI; i++) for (c = 0; c < 3; c++) {
        ((UINT *)(p + ST_U))[3 * i + c] = uval(i, c);
        ((INT *)(p + ST_I))[3 * i + c] = ival(i, c);
    }
    for (i = 0; i < NN; i++) for (c = 0; c < 3; c++) ((float *)(p + ST_C))[3 * i + c] = fval(i, c, 1000.0f);
    ID3D12Resource_Unmap(stage, 0, NULL);
}
static void fill_upload(float seed) {
    float *p = NULL; UINT i, c;
    ID3D12Resource_Map(pbuf, 0, NULL, (void **)&p);
    for (i = 0; i < NP; i++) for (c = 0; c < 3; c++) p[3 * i + c] = fval(i, c, seed);
    ID3D12Resource_Unmap(pbuf, 0, NULL);
}

/* Case 8. Frame f writes slot f % NSLOT of the ring (seed f), records NREAD
 * reads of it through the slot's own view and through the whole-ring view at
 * the slot's offset, and is submitted without waiting; frame f + NSLOT waits
 * for frame f's fence before checking its output and reusing its slot. */
static float frame_seed(UINT f) { return 10.0f + 3.25f * (float)f; }
static void frames_in_flight(void) {
    ID3D12CommandAllocator *fa[NSLOT] = { 0 }; ID3D12GraphicsCommandList *fl[NSLOT] = { 0 };
    ID3D12GraphicsCommandList *saved = list;
    UINT64 done_at[NSLOT] = { 0 };
    UINT f, k, bad_slot = 0, bad_ring = 0, checked = 0;
    float *rp = NULL;
    printf("\n[8] UPLOAD ring, %u slots rewritten every frame, %u frames, %u in flight\n", NSLOT, NFRAMES, NSLOT);
    for (k = 0; k < NSLOT; k++) {
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&fa[k]);
        if (fa[k]) ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, fa[k], NULL,
                                                  &IID_ID3D12GraphicsCommandList, (void **)&fl[k]);
        if (fl[k]) ID3D12GraphicsCommandList_Close(fl[k]);
    }
    ID3D12Resource_Map(ring, 0, NULL, (void **)&rp);
    if (!rp || !fl[0] || !fl[1] || !fl[2]) { CHECK(0, "case 8 set up"); goto out; }
    for (f = 0; f < NFRAMES + NSLOT; f++) {
        UINT slot = f % NSLOT, i, c, r;
        if (f >= NSLOT) {   /* frame f - NSLOT is done: check it, then reuse its slot */
            UINT pf = f - NSLOT; const UINT *o = NULL;
            if (ID3D12Fence_GetCompletedValue(fence) < done_at[slot]) {
                HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
                ID3D12Fence_SetEventOnCompletion(fence, done_at[slot], ev);
                if (WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0) { CloseHandle(ev); CHECK(0, "frame %u fence", pf); goto out; }
                CloseHandle(ev);
            }
            ID3D12Resource_Map(rb, 0, NULL, (void **)&o);
            if (!o) { CHECK(0, "readback mapped"); goto out; }
            for (r = 0; r < 2; r++) {
                const UINT *q = o + (SIZE_T)(2 * slot + r) * (OUT_REGION / 4);
                UINT bad = 0;
                for (i = 0; i < NR; i++) {
                    UINT v[4];
                    for (c = 0; c < 3; c++) { float x = fval(i, c, frame_seed(pf)); memcpy(&v[c], &x, 4); }
                    v[3] = ONE_F;
                    if (memcmp(&q[4 * i], v, 16)) {
                        if (!bad++ && !(r ? bad_ring : bad_slot))
                            printf("        frame %u %s view, element %u: got (%#x %#x %#x %#x), expected (%#x %#x %#x %#x)\n",
                                   pf, r ? "whole-ring" : "slot", i, q[4 * i], q[4 * i + 1], q[4 * i + 2], q[4 * i + 3], v[0], v[1], v[2], v[3]);
                    }
                }
                if (bad) { if (r) bad_ring++; else bad_slot++; }
            }
            ID3D12Resource_Unmap(rb, 0, NULL);
            checked++;
        }
        if (f >= NFRAMES) continue;
        for (i = 0; i < NR; i++) for (c = 0; c < 3; c++) rp[3 * ((SIZE_T)slot * NR + i) + c] = fval(i, c, frame_seed(f));
        list = fl[slot];
        ID3D12CommandAllocator_Reset(fa[slot]);
        ID3D12GraphicsCommandList_Reset(list, fa[slot], NULL);
        for (r = 0; r < NREAD; r++) {   /* the last read of each lands in the output */
            read_view_at(K_FLOAT, S_SLOT0 + slot, 0, NR, 2 * slot);
            read_view_at(K_FLOAT, S_RING, slot * NR, NR, 2 * slot + 1);
        }
        barrier(outbuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, rb, 2 * slot * OUT_REGION, outbuf, 2 * slot * OUT_REGION, 2 * OUT_REGION);
        barrier(outbuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12GraphicsCommandList_Close(list);
        {
            ID3D12CommandList *l1[1] = { (ID3D12CommandList *)list };
            ID3D12CommandQueue_ExecuteCommandLists(queue, 1, l1);
            ID3D12CommandQueue_Signal(queue, fence, ++fence_v);
            done_at[slot] = fence_v;
        }
    }
    CHECK(checked == NFRAMES, "all %u frames completed and checked", NFRAMES);
    CHECK(!bad_slot, "per-slot views: every frame read its own data, w = 1 (%u frames wrong)", bad_slot);
    CHECK(!bad_ring, "whole-ring view at the slot's offset: every frame read its own data, w = 1 (%u frames wrong)", bad_ring);
out:
    if (rp) ID3D12Resource_Unmap(ring, 0, NULL);
    list = saved;
    for (k = 0; k < NSLOT; k++) { if (fl[k]) ID3D12GraphicsCommandList_Release(fl[k]); if (fa[k]) ID3D12CommandAllocator_Release(fa[k]); }
}

static void stats(UINT64 *c, UINT64 *v, UINT64 *k) {
    struct shadow_stats st; memset(&st, 0, sizeof st);
    if (p_stats) p_stats(&st);
    *c = st.copied; *v = st.viewed; *k = st.skips;
}
static struct shadow_stats stats2(void) { struct shadow_stats st; memset(&st, 0, sizeof st); if (p_stats) p_stats(&st); return st; }
static UINT64 g_all_views, g_whole_bytes;   /* case 9 -> 10 */
static void refresh_cost(void) {
    struct expect G1 = { K_FLOAT, 5.0f, 0 };
    UINT64 c0, v0, k0, c1, v1, k1, want_c = 0, want_v = 0, all = 0;
    float *p = NULL; UINT i, c, k, last = 2 * (NCOPY - 1);
    printf("\n[9] refresh cost: %u small views + 1 whole view of one %u MB buffer\n", NBIGV, NG * 12 >> 20);
    if (!p_stats) { CHECK(0, "MadeiraD3D12ShadowStats exported (another runtime?)"); return; }
    ID3D12Resource_Map(gstage, 0, NULL, (void **)&p);
    if (!p) { CHECK(0, "big staging buffer mapped"); return; }
    for (i = 0; i < NG; i++) for (c = 0; c < 3; c++) { p[3 * i + c] = fval(i, c, 5.0f); p[3 * ((SIZE_T)NG + i) + c] = fval(i, c, -3.5f); }
    ID3D12Resource_Unmap(gstage, 0, NULL);
    for (k = 0; k < NBIGV; k++) { make_view(S_BIG0 + k, gbuf, DXGI_FORMAT_R32G32B32_FLOAT, big_first(k), big_num(k)); all += big_num(k) * 12ull; }
    make_view(S_WHOLE, gbuf, DXGI_FORMAT_R32G32B32_FLOAT, 0, NG); all += NG * 12ull;
    g_all_views = all; g_whole_bytes = NG * 12ull;

    /* whole contents (seed 5), read */
    ID3D12GraphicsCommandList_CopyBufferRegion(list, gbuf, 0, gstage, 0, (UINT64)NG * 12);
    barrier(gbuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    read_view_at(K_FLOAT, S_WHOLE, 0, 4096, 0);
    if (finish(1)) verify("whole view after the first copy", 0, 4096, &G1); else CHECK(0, "case 9a submitted");

    /* NCOPY small copies (seed -3.5) at even views' starts, each made readable and read */
    stats(&c0, &v0, &k0);
    for (i = 0; i < NCOPY; i++) {
        UINT v = 2 * i, o = big_first(v);
        barrier(gbuf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, gbuf, (UINT64)o * 12, gstage, ((UINT64)NG + o) * 12, (UINT64)CPYN * 12);
        barrier(gbuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        read_view_at(K_FLOAT, S_BIG0 + v, 0, big_num(v), 0);
        want_c += big_num(v) * 12ull + CPYN * 12ull;   /* the view, and the copied range of the whole view */
        want_v += big_num(v) * 12ull + NG * 12ull;
    }
    if (!finish(1)) { CHECK(0, "case 9b submitted"); return; }
    stats(&c1, &v1, &k1);
    printf("        copied %llu bytes for views of %llu bytes (refreshing those views whole: %llu; every view at every barrier: %llu)\n",
           (unsigned long long)(c1 - c0), (unsigned long long)(v1 - v0), (unsigned long long)want_v, (unsigned long long)(all * NCOPY));
    CHECK(c1 - c0 == want_c, "40 small copies refreshed exactly what they wrote: %llu bytes copied, %llu expected", (unsigned long long)(c1 - c0), (unsigned long long)want_c);
    {
        struct expect Gv = { K_FLOAT, -3.5f, big_first(last) }, Gn = { K_FLOAT, 5.0f, big_first(last) + CPYN };
        read_view_at(K_FLOAT, S_BIG0 + last, 0, big_num(last), 0);
        read_view_at(K_FLOAT, S_WHOLE, big_first(last), CPYN, 1);
        read_view_at(K_FLOAT, S_WHOLE, big_first(last) + CPYN, 500, 2);
        if (finish(3)) {
            verify("last small view: new data", 0, big_num(last), &Gv);
            verify("whole view over the last copy: new data", 1, CPYN, &Gv);
            verify("whole view just past it: old data", 2, 500, &Gn);
        } else CHECK(0, "case 9c submitted");
    }

    /* two UAV -> SRV barriers with no work between: one whole refresh */
    stats(&c0, &v0, &k0);
    barrier(gbuf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(gbuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(gbuf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(gbuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    read_view_at(K_FLOAT, S_BIG0, 0, big_num(0), 0);
    if (!finish(1)) { CHECK(0, "case 9d submitted"); return; }
    stats(&c1, &v1, &k1);
    CHECK(c1 - c0 == all && k1 - k0 == 1, "UAV -> SRV twice with no work between: every view refreshed once (%llu of %llu bytes, %llu skipped)",
          (unsigned long long)(c1 - c0), (unsigned long long)all, (unsigned long long)(k1 - k0));
}

static void uav_cycle(void) {
    barrier(gbuf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(gbuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    read_view_at(K_FLOAT, S_BIG0, 0, big_num(0), 0);
}
static void dedupe_and_big(void) {
    struct expect Ubits = { K_UINT, 0, 0 };
    struct shadow_stats a, b;
    UINT64 small, big_n;
    printf("\n[10] UINT/SINT shadow sharing; big views refreshed once a frame after shader writes\n");
    if (!p_stats || !g_all_views) { CHECK(0, "MadeiraD3D12ShadowStats exported and case 9 ran"); return; }
    a = stats2();
    make_view(S_USI, ubuf, DXGI_FORMAT_R32G32B32_SINT, 0, NI);
    b = stats2();
    CHECK(b.live == a.live && b.live_bytes == a.live_bytes, "a SINT view of a UINT view's range made no new shadow (%llu -> %llu live, %llu -> %llu bytes)",
          (unsigned long long)a.live, (unsigned long long)b.live, (unsigned long long)a.live_bytes, (unsigned long long)b.live_bytes);
    read_view(K_SINT, S_USI, NI, 0);
    if (finish(1)) verify("SINT view over the UINT data: same bits", 0, NI, &Ubits); else CHECK(0, "case 10a submitted");

    small = g_all_views - g_whole_bytes;
    big_n = g_whole_bytes >= b.big_bytes ? 1 : 0;
    printf("        big views from %llu bytes (%s); the whole view is %s\n", (unsigned long long)b.big_bytes,
           b.big_exact ? "rgb32-big-exact = 1" : "once a frame", big_n ? "big" : "NOT big: the check expects every view refreshed every time");
    a = stats2();
    uav_cycle(); uav_cycle();   /* one frame: shader writes, read, shader writes, read */
    if (!finish(1)) { CHECK(0, "case 10b submitted"); return; }
    b = stats2();
    {
        UINT64 want = 2 * small + ((b.big_exact || !big_n) ? 2 : 1) * g_whole_bytes;
        UINT64 want_skips = (b.big_exact || !big_n) ? 0 : 1;
        CHECK(b.copied - a.copied == want && b.big_skips - a.big_skips == want_skips,
              "two shader-write barriers in one frame: %llu bytes copied (%llu expected), big view held back %llu times (%llu expected)",
              (unsigned long long)(b.copied - a.copied), (unsigned long long)want,
              (unsigned long long)(b.big_skips - a.big_skips), (unsigned long long)want_skips);
    }
    a = stats2();
    uav_cycle();   /* the next frame */
    if (!finish(1)) { CHECK(0, "case 10c submitted"); return; }
    b = stats2();
    CHECK(b.copied - a.copied == g_all_views && b.big_skips == a.big_skips,
          "next frame: the big view refreshed again (%llu of %llu bytes copied)", (unsigned long long)(b.copied - a.copied), (unsigned long long)g_all_views);
    {
        struct expect Gw = { K_FLOAT, 5.0f, 13000 };   /* between case 9's copies at views 0 and 2 */
        read_view_at(K_FLOAT, S_WHOLE, 13000, 4096, 0);
        if (finish(1)) verify("whole view still right", 0, 4096, &Gw); else CHECK(0, "case 10d submitted");
    }
}

static void cases(void) {
    struct expect A = { K_FLOAT, 0.25f, 0 }, Aoff = { K_FLOAT, 0.25f, OFF }, B = { K_FLOAT, -7.0f, 0 }, Boff = { K_FLOAT, -7.0f, OFF };
    struct expect U = { K_UINT, 0, 0 }, I = { K_SINT, 0, 0 }, C = { K_FLOAT, 1000.0f, 0 }, Blate = { K_FLOAT, -7.0f, LATE_FIRST };
    struct expect P1 = { K_FLOAT, 3.0f, 0 }, P2 = { K_FLOAT, 99.5f, 0 };

    printf("\n[1] R32G32B32_FLOAT over a DEFAULT buffer: CopyBufferRegion, barrier, read (%u elements)\n", NF);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, fbuf, 0, stage, ST_A, (UINT64)NF * 12);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, ubuf, 0, stage, ST_U, (UINT64)NI * 12);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, ibuf, 0, stage, ST_I, (UINT64)NI * 12);
    {
        D3D12_RESOURCE_BARRIER b[3]; ID3D12Resource *r[3] = { fbuf, ubuf, ibuf }; UINT k;
        ZeroMemory(b, sizeof b);
        for (k = 0; k < 3; k++) {
            b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[k].Transition.pResource = r[k];
            b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[k].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b[k].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        ID3D12GraphicsCommandList_ResourceBarrier(list, 3, b);
    }
    read_view(K_FLOAT, S_F, NF, 0);
    if (finish(1)) verify("float view", 0, NF, &A); else CHECK(0, "case 1 submitted");

    printf("\n[2] FirstElement %u\n", OFF);
    read_view(K_FLOAT, S_FOFF, NOFF, 0);
    if (finish(1)) verify("float view at FirstElement 37", 0, NOFF, &Aoff); else CHECK(0, "case 2 submitted");

    printf("\n[3] R32G32B32_UINT and R32G32B32_SINT\n");
    read_view(K_UINT, S_U, NI, 0);
    read_view(K_SINT, S_I, NI, 1);
    if (finish(2)) { verify("uint view", 0, NI, &U); verify("sint view", 1, NI, &I); } else CHECK(0, "case 3 submitted");

    printf("\n[4] a second copy + barrier in the same list\n");
    read_view(K_FLOAT, S_F, NF, 0);
    barrier(fbuf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, fbuf, 0, stage, ST_B, (UINT64)NF * 12);
    barrier(fbuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    read_view(K_FLOAT, S_F, NF, 1);
    read_view(K_FLOAT, S_FOFF, NOFF, 2);
    if (finish(3)) {
        verify("read before the second copy: old data", 0, NF, &A);
        verify("read after it: new data", 1, NF, &B);
        verify("offset view after it: new data", 2, NOFF, &Boff);
    } else CHECK(0, "case 4 submitted");

    printf("\n[5] a view made after its buffer holds data\n");
    make_view(S_LATE, fbuf, DXGI_FORMAT_R32G32B32_FLOAT, LATE_FIRST, NLATE);
    read_view(K_FLOAT, S_LATE, NLATE, 0);
    if (finish(1)) verify("late view", 0, NLATE, &Blate); else CHECK(0, "case 5 submitted");

    printf("\n[6] copy in one list, read in the next, no barrier (implicit promotion from COMMON)\n");
    ID3D12GraphicsCommandList_CopyBufferRegion(list, nbuf, 0, stage, ST_C, (UINT64)NN * 12);
    if (!submit_and_wait()) CHECK(0, "case 6 copy submitted");
    read_view(K_FLOAT, S_N, NN, 0);
    if (finish(1)) verify("view of a buffer copied without a barrier", 0, NN, &C); else CHECK(0, "case 6 submitted");

    printf("\n[7] a view over an UPLOAD buffer the CPU rewrites\n");
    fill_upload(3.0f);
    read_view(K_FLOAT, S_P, NP, 0);
    if (finish(1)) verify("upload view, first contents", 0, NP, &P1); else CHECK(0, "case 7a submitted");
    fill_upload(99.5f);
    read_view(K_FLOAT, S_P, NP, 0);
    if (finish(1)) verify("upload view, rewritten", 0, NP, &P2); else CHECK(0, "case 7b submitted");

    frames_in_flight();
    refresh_cost();
    dedupe_and_big();
}

int main(void) {
    HRESULT hr;
    printf("wine-lab d3d12: RGB32 typed buffer views\n\n");

    HMODULE d12 = LoadLibraryA("d3d12.dll");
    pfn_create_device create = d12 ? (pfn_create_device)GetProcAddress(d12, "D3D12CreateDevice") : NULL;
    p_serialize = d12 ? (pfn_serialize_v)GetProcAddress(d12, "D3D12SerializeVersionedRootSignature") : NULL;
    CHECK(create && p_serialize, "d3d12.dll loaded, device creation and root signature serializer resolved");
    if (!create || !p_serialize) goto done;
    pfn_marker marker = (pfn_marker)GetProcAddress(d12, "MadeiraD3D12GetBuildMarker");
    if (marker) printf("        implementation: %s\n", marker());
    p_stats = (pfn_shadow_stats)GetProcAddress(d12, "MadeiraD3D12ShadowStats");

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
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 128;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap);
        inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    CHECK(queue && alloc && list && fence && heap, "queue, allocator, list, fence and descriptor heap created");
    if (!queue || !alloc || !list || !fence || !heap) goto done;

    stage = make_buffer(D3D12_HEAP_TYPE_UPLOAD, ST_SIZE, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    fbuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)NF * 12, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ubuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)NI * 12, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ibuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)NI * 12, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    nbuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)NN * 12, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    pbuf = make_buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)NP * 12, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    ring = make_buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)NSLOT * NR * 12, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    gbuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)NG * 12, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    gstage = make_buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)NG * 24, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    outbuf = make_buffer(D3D12_HEAP_TYPE_DEFAULT, NREGIONS * OUT_REGION, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, NREGIONS * OUT_REGION, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(stage && fbuf && ubuf && ibuf && nbuf && pbuf && ring && gbuf && gstage && outbuf && rb, "source, staging, output and readback buffers created");
    if (!stage || !fbuf || !ubuf || !ibuf || !nbuf || !pbuf || !ring || !gbuf || !gstage || !outbuf || !rb) goto done;
    rs = make_rs();
    if (!rs) goto done;
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd; ZeroMemory(&cd, sizeof cd);
        cd.pRootSignature = rs;
        cd.CS.pShaderBytecode = rgb_cs_dxil; cd.CS.BytecodeLength = sizeof rgb_cs_dxil;
        hr = ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **)&pso);
        CHECK(SUCCEEDED(hr) && pso, "compute PSO created (%#lx)", hr);
    }
    if (!pso) goto done;

    fill_stage();
    /* every view but the late one exists before any data does */
    make_view(S_F, fbuf, DXGI_FORMAT_R32G32B32_FLOAT, 0, NF);
    make_view(S_FOFF, fbuf, DXGI_FORMAT_R32G32B32_FLOAT, OFF, NOFF);
    make_view(S_U, ubuf, DXGI_FORMAT_R32G32B32_UINT, 0, NI);
    make_view(S_I, ibuf, DXGI_FORMAT_R32G32B32_SINT, 0, NI);
    make_view(S_P, pbuf, DXGI_FORMAT_R32G32B32_FLOAT, 0, NP);
    make_view(S_N, nbuf, DXGI_FORMAT_R32G32B32_FLOAT, 0, NN);
    make_view(S_RING, ring, DXGI_FORMAT_R32G32B32_FLOAT, 0, NSLOT * NR);
    { UINT k; for (k = 0; k < NSLOT; k++) make_view(S_SLOT0 + k, ring, DXGI_FORMAT_R32G32B32_FLOAT, k * NR, NR); }

    cases();

done:
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rb) ID3D12Resource_Release(rb);
    if (outbuf) ID3D12Resource_Release(outbuf);
    if (ring) ID3D12Resource_Release(ring);
    if (gstage) ID3D12Resource_Release(gstage);
    if (gbuf) ID3D12Resource_Release(gbuf);
    if (pbuf) ID3D12Resource_Release(pbuf);
    if (nbuf) ID3D12Resource_Release(nbuf);
    if (ibuf) ID3D12Resource_Release(ibuf);
    if (ubuf) ID3D12Resource_Release(ubuf);
    if (fbuf) ID3D12Resource_Release(fbuf);
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
