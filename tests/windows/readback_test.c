/*  Compute results read back through a READBACK buffer as soon as a fence says
 *  so, for the wine-lab D3D12 port.
 *
 *  The Last of Us Part I casts rays on the GPU (Cs_CreateRayMeshInfoBuffer,
 *  Cs_GenerateRayHitPolygonInfo, Cs_CheckRayPolygonHit, Cs_SortingHitPosition-
 *  AlongRay), copies the hits to a readback buffer and reads them on the CPU
 *  once a fence passes; on entering the intro scene it asserted "Invalid result
 *  data for ray 0" (MeshRayCaster::HandleResultsInGameFrame). Two ways this
 *  runtime could cause that are checked here: a fence that reaches its value
 *  before the dispatch and the copy into the readback buffer have finished (or
 *  before what they wrote is visible to the CPU), and a 64-bit InterlockedMin
 *  (the closest-hit pattern, SM 6.6) that is not atomic as a whole.
 *
 *  A dispatch fills a 1 MB DEFAULT buffer with values that depend on an
 *  iteration seed (each thread spins first, so the dispatch takes GPU time), a
 *  copy moves it into a READBACK buffer, a Signal follows, and the CPU checks
 *  every dword the moment the fence reports the value. The readback range is
 *  poisoned with 0 (never a fill value) before each use, so a read that comes
 *  too early sees 0s rather than plausible data.
 *
 *  Cases (ITERS iterations each, every dword checked every time):
 *    1  one list (dispatch, barrier, copy), Signal, the CPU spins on
 *       GetCompletedValue
 *    2  batched: two small unrelated lists, then the dispatch list and the
 *       copy list, each in its own ExecuteCommandLists (they share one open
 *       batch, see madeira.cfg batch-lists), Signal, SetEventOnCompletion
 *    3  the copy on a COPY queue: direct queue dispatch + Signal(a), copy queue
 *       Wait(a), copy, Signal(b); the CPU polls b
 *    4  dispatch and copy on a COMPUTE queue
 *    5  frames in flight into a 16 MB readback buffer (CPU-visible buffers of
 *       8 MB and more live on file-backed storage, madeira.cfg upload-swap):
 *       frame i writes slot i % 3 at a nonzero offset and signals i; before
 *       submitting frame i + 1 the CPU waits for frame i - 1 and checks its slot
 *    6  64-bit InterlockedMin without the old value, RWByteAddressBuffer
 *       (InterlockedMin64), RWStructuredBuffer<uint64_t> and RWBuffer<uint64_t>
 *       (a 2 MB R32G32_UINT view cleared to all ones by
 *       ClearUnorderedAccessViewUint, as the game clears one, slots 4096
 *       elements apart): 262144 threads, 4096 per slot, key =
 *       (asuint(t) << 32) | thread; every slot must hold the exact minimum the
 *       CPU computes, every other element the clear value; and groupshared
 *       uint64_t, only while D3D12_OPTIONS9 claims AtomicInt64OnGroupShared:
 *       Apple's compiler could not build that kernel, every dispatch of it was
 *       skipped, and the claim is now off by default
 *
 *  The DXIL shaders are precompiled by Microsoft's dxc and embedded
 *  (readback_dxil.h, from readback.hlsl; regeneration commands there).
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
#include "readback_dxil.h"

static int checks, fails;
#define CHECK(cond, ...) do {                                                  \
    checks++;                                                                  \
    if (!(cond)) { fails++; printf("  FAIL  " __VA_ARGS__); printf("\n"); }     \
    else         { printf("  ok    " __VA_ARGS__); printf("\n"); }             \
    fflush(stdout);                                                            \
} while (0)

typedef HRESULT (WINAPI *pfn_create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef const char *(*pfn_marker)(void);

enum { N = 256 * 1024, BYTES = N * 4, SPIN = 1024, ITERS = 150 };
enum { RING = 3, RING_SLOT = 4 << 20, RING_OFF = 64 * 1024, BIG = 16 << 20 };
enum { SLOTS = 64, CANDIDATES = SLOTS * 4096, MIN_ITERS = 12, TKEYS = 256 * 1024, TSTRIDE = TKEYS / SLOTS, GROUPS = CANDIDATES / 256 };

static ID3D12Device       *dev;
static ID3D12CommandQueue *q_direct, *q_copy, *q_compute;
static ID3D12CommandAllocator *a_direct, *a_copy, *a_compute, *a_side[3];
static ID3D12GraphicsCommandList *l_direct, *l_copy, *l_compute, *l_side[3];
static ID3D12Fence        *fence, *fence_b;
static UINT64              fence_v, fence_bv;
static ID3D12RootSignature *rs, *rs_typed;
static ID3D12PipelineState *pso_fill, *pso_min_raw, *pso_min_struct, *pso_min_typed, *pso_min_gs;
static ID3D12DescriptorHeap *cpuh, *gpuh;
static ID3D12Resource     *def, *side, *rb, *rb_big, *keys, *keys_init, *rb_keys, *tkeys, *gkeys;

/* ------------------------------------------------------------------ helpers */

static UINT32 fill_value(UINT32 s, UINT32 i) {   /* readback.hlsl */
    UINT32 h = s * 0x9e3779b1u ^ i * 0x85ebca6bu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
    return h | 1u;
}
static UINT64 candidate_key(UINT32 s, UINT32 i) {   /* readback.hlsl */
    UINT32 h = s * 0x9e3779b1u ^ i * 0x85ebca6bu, tb;
    float t;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
    t = (float)(h >> 8) * (1.0f / 16777216.0f);
    memcpy(&tb, &t, 4);
    return ((UINT64)tb << 32) | i;
}

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = type;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}

static void barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b; ZeroMemory(&b, sizeof b);
    if (!r) {   /* a UAV barrier on every resource */
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &b);
        return;
    }
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &b);
}

static void dispatch(ID3D12GraphicsCommandList *l, ID3D12PipelineState *pso, ID3D12Resource *dst,
                     UINT seed, UINT count, UINT spin) {
    UINT c[4] = { seed, count, spin, 0 };
    ID3D12GraphicsCommandList_SetComputeRootSignature(l, rs);
    ID3D12GraphicsCommandList_SetPipelineState(l, pso);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(l, 0, 4, c, 0);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(l, 1, ID3D12Resource_GetGPUVirtualAddress(dst));
    ID3D12GraphicsCommandList_Dispatch(l, (count + 63) / 64, 1, 1);
}

static void execute(ID3D12CommandQueue *q, ID3D12GraphicsCommandList *l) {
    ID3D12CommandList *cl[1] = { (ID3D12CommandList *)l };
    ID3D12GraphicsCommandList_Close(l);
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, cl);
}

static void reopen(ID3D12CommandAllocator *a, ID3D12GraphicsCommandList *l) {
    ID3D12CommandAllocator_Reset(a);
    ID3D12GraphicsCommandList_Reset(l, a, NULL);
}

/* Spins on GetCompletedValue, as a game's frame loop does. FALSE after 10 s. */
static BOOL spin_until(ID3D12Fence *f, UINT64 v) {
    DWORD t0 = GetTickCount();
    while (ID3D12Fence_GetCompletedValue(f) < v)
        if (GetTickCount() - t0 > 10000) { printf("        fence never reached %llu\n", (unsigned long long)v); return FALSE; }
    return TRUE;
}
static BOOL event_until(ID3D12Fence *f, UINT64 v) {
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    DWORD w;
    ID3D12Fence_SetEventOnCompletion(f, v, ev);
    w = WaitForSingleObject(ev, 10000);
    CloseHandle(ev);
    if (w != WAIT_OBJECT_0) printf("        fence event for %llu timed out\n", (unsigned long long)v);
    return w == WAIT_OBJECT_0;
}

static void poison(ID3D12Resource *r, UINT64 off, UINT64 len) {
    BYTE *p = NULL;
    ID3D12Resource_Map(r, 0, NULL, (void **)&p);
    if (p) memset(p + off, 0, (size_t)len);
    ID3D12Resource_Unmap(r, 0, NULL);
}

/* Checks the fill of iteration `seed` at `off` in `r`; returns the dwords that
 * differ and says, the first few times, what they held instead. */
static int verify_fill(ID3D12Resource *r, UINT64 off, UINT seed, const char *what, int *reports) {
    const UINT32 *p = NULL;
    D3D12_RANGE range = { (SIZE_T)off, (SIZE_T)(off + BYTES) };
    int bad = 0, zero = 0, stale = 0, first = -1, i;
    ID3D12Resource_Map(r, 0, &range, (void **)&p);
    if (!p) { printf("        %s: Map failed\n", what); return N; }
    p = (const UINT32 *)((const BYTE *)p + off);
    for (i = 0; i < N; i++)
        if (p[i] != fill_value(seed, i)) {
            if (first < 0) first = i;
            bad++;
            if (!p[i]) zero++;
            else if (seed && p[i] == fill_value(seed - 1, i)) stale++;
        }
    if (bad && (*reports)++ < 3)
        printf("        %s, iteration %u: %d of %d dwords wrong (%d still the poison 0, %d the previous iteration's); "
               "first at %d: %#x, expected %#x\n", what, seed, bad, N, zero, stale, first, p[first], fill_value(seed, first));
    ID3D12Resource_Unmap(r, 0, NULL);
    return bad;
}

/* ------------------------------------------------------------------ cases */

static void case_one_list(void) {
    int bad_iters = 0, reports = 0;
    UINT it;
    for (it = 1; it <= ITERS; it++) {
        poison(rb, 0, BYTES);
        dispatch(l_direct, pso_fill, def, it, N, SPIN);
        barrier(l_direct, def, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(l_direct, rb, 0, def, 0, BYTES);
        barrier(l_direct, def, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        execute(q_direct, l_direct);
        ID3D12CommandQueue_Signal(q_direct, fence, ++fence_v);
        if (!spin_until(fence, fence_v)) { bad_iters = ITERS; break; }
        if (verify_fill(rb, 0, it, "one list", &reports)) bad_iters++;
        reopen(a_direct, l_direct);
    }
    CHECK(bad_iters == 0, "one list, Signal, GetCompletedValue: %d of %d iterations read wrong data", bad_iters, ITERS);
}

static void case_batched(void) {
    int bad_iters = 0, reports = 0, k;
    UINT it;
    for (it = 1; it <= ITERS; it++) {
        poison(rb, 0, BYTES);
        for (k = 0; k < 2; k++) {   /* unrelated small work ahead of it in the same batch */
            dispatch(l_side[k], pso_fill, side, 1000 + k, 64, 1);
            execute(q_direct, l_side[k]);
        }
        dispatch(l_direct, pso_fill, def, it, N, SPIN);
        barrier(l_direct, def, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        execute(q_direct, l_direct);
        ID3D12GraphicsCommandList_CopyBufferRegion(l_side[2], rb, 0, def, 0, BYTES);
        barrier(l_side[2], def, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        execute(q_direct, l_side[2]);
        ID3D12CommandQueue_Signal(q_direct, fence, ++fence_v);
        if (!event_until(fence, fence_v)) { bad_iters = ITERS; break; }
        if (verify_fill(rb, 0, it, "batched", &reports)) bad_iters++;
        reopen(a_direct, l_direct);
        for (k = 0; k < 3; k++) reopen(a_side[k], l_side[k]);
    }
    CHECK(bad_iters == 0, "four ExecuteCommandLists in one batch, Signal, SetEventOnCompletion: %d of %d iterations read wrong data",
          bad_iters, ITERS);
}

static void case_copy_queue(void) {
    int bad_iters = 0, reports = 0;
    UINT it;
    for (it = 1; it <= ITERS; it++) {
        poison(rb, 0, BYTES);
        dispatch(l_direct, pso_fill, def, it, N, SPIN);
        barrier(l_direct, def, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        execute(q_direct, l_direct);
        ID3D12CommandQueue_Signal(q_direct, fence, ++fence_v);
        ID3D12CommandQueue_Wait(q_copy, fence, fence_v);
        ID3D12GraphicsCommandList_CopyBufferRegion(l_copy, rb, 0, def, 0, BYTES);
        execute(q_copy, l_copy);
        ID3D12CommandQueue_Signal(q_copy, fence_b, ++fence_bv);
        if (!spin_until(fence_b, fence_bv)) { bad_iters = ITERS; break; }
        if (verify_fill(rb, 0, it, "copy queue", &reports)) bad_iters++;
        reopen(a_direct, l_direct); reopen(a_copy, l_copy);
        barrier(l_direct, def, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    CHECK(bad_iters == 0, "dispatch on the direct queue, copy on a copy queue behind Wait: %d of %d iterations read wrong data",
          bad_iters, ITERS);
}

static void case_compute_queue(void) {
    int bad_iters = 0, reports = 0;
    UINT it;
    for (it = 1; it <= ITERS; it++) {
        poison(rb, 0, BYTES);
        dispatch(l_compute, pso_fill, def, it, N, SPIN);
        barrier(l_compute, def, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(l_compute, rb, 0, def, 0, BYTES);
        barrier(l_compute, def, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        execute(q_compute, l_compute);
        ID3D12CommandQueue_Signal(q_compute, fence, ++fence_v);
        if (!spin_until(fence, fence_v)) { bad_iters = ITERS; break; }
        if (verify_fill(rb, 0, it, "compute queue", &reports)) bad_iters++;
        reopen(a_compute, l_compute);
    }
    CHECK(bad_iters == 0, "dispatch and copy on a compute queue: %d of %d iterations read wrong data", bad_iters, ITERS);
}

/* wine-lab port: a fence handed from one queue to another with nothing executed between --
 * direct: dispatch + copy, Signal(a); copy queue: Wait(a), Signal(b) at once. b must not
 * complete before a's work has (the runtime used to signal b with the copy queue's own last
 * batch, long finished). */
static void case_forward(void) {
    int bad_iters = 0, reports = 0;
    UINT it;
    for (it = 1; it <= ITERS; it++) {
        poison(rb, 0, BYTES);
        dispatch(l_direct, pso_fill, def, it, N, SPIN);
        barrier(l_direct, def, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(l_direct, rb, 0, def, 0, BYTES);
        barrier(l_direct, def, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        execute(q_direct, l_direct);
        ID3D12CommandQueue_Signal(q_direct, fence, ++fence_v);
        ID3D12CommandQueue_Wait(q_copy, fence, fence_v);
        ID3D12CommandQueue_Signal(q_copy, fence_b, ++fence_bv);
        if (!spin_until(fence_b, fence_bv)) { bad_iters = ITERS; break; }
        if (verify_fill(rb, 0, it, "forwarded fence", &reports)) bad_iters++;
        reopen(a_direct, l_direct);
    }
    CHECK(bad_iters == 0, "Wait then Signal on another queue (a forwarded fence): %d of %d iterations read wrong data", bad_iters, ITERS);
}

static void case_ring(void) {
    int bad_iters = 0, reports = 0, checked = 0;
    UINT64 sig[ITERS + 2];
    UINT it;
    for (it = 1; it <= ITERS + 1; it++) {
        if (it <= ITERS) {
            UINT64 off = (UINT64)(it % RING) * RING_SLOT + RING_OFF;
            poison(rb_big, off, BYTES);   /* frame it - 3 used it and was checked */
            dispatch(l_side[it % RING], pso_fill, def, it, N, SPIN);
            barrier(l_side[it % RING], def, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ID3D12GraphicsCommandList_CopyBufferRegion(l_side[it % RING], rb_big, off, def, 0, BYTES);
            barrier(l_side[it % RING], def, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            execute(q_direct, l_side[it % RING]);
            ID3D12CommandQueue_Signal(q_direct, fence, sig[it] = ++fence_v);
        }
        if (it >= 2) {   /* frame it - 1, while frame it may still run */
            UINT f = it - 1;
            if (!(f & 1) ? !spin_until(fence, sig[f]) : !event_until(fence, sig[f])) { bad_iters = ITERS; break; }
            if (verify_fill(rb_big, (UINT64)(f % RING) * RING_SLOT + RING_OFF, f, "frames in flight", &reports)) bad_iters++;
            checked++;
            reopen(a_side[f % RING], l_side[f % RING]);
        }
    }
    CHECK(bad_iters == 0 && checked == ITERS, "frames in flight, 16 MB readback buffer, %u slots at +64 KB: %d of %d iterations read wrong data",
          (unsigned)RING, bad_iters, checked);
}

/* typed: the slots are an R32G32_UINT view of `tkeys`, 2 MB like the game's,
 * TSTRIDE elements apart, cleared to all ones by ClearUnorderedAccessViewUint
 * as the game does (not copied from keys_init); every element is checked. */
static void case_min64(ID3D12PipelineState *pso, const char *what, BOOL typed) {
    static UINT64 want[SLOTS];
    const UINT n = typed ? TKEYS : SLOTS, stride = typed ? TSTRIDE : 1;
    int bad_iters = 0, reports = 0;
    UINT it, i;
    for (it = 1; it <= MIN_ITERS; it++) {
        const UINT64 *p = NULL;
        int bad = 0, first = -1;
        ID3D12Resource *dst = typed ? tkeys : keys;
        for (i = 0; i < SLOTS; i++) want[i] = ~(UINT64)0;
        for (i = 0; i < CANDIDATES; i++) { UINT64 k = candidate_key(it, i); if (k < want[i % SLOTS]) want[i % SLOTS] = k; }
        if (typed) {
            static const UINT ones[4] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu };
            UINT c[4] = { it, CANDIDATES, SLOTS, TSTRIDE };
            D3D12_CPU_DESCRIPTOR_HANDLE cpu; D3D12_GPU_DESCRIPTOR_HANDLE gpu;
            cpuh->lpVtbl->GetCPUDescriptorHandleForHeapStart(cpuh, &cpu);
            gpuh->lpVtbl->GetGPUDescriptorHandleForHeapStart(gpuh, &gpu);
            ID3D12GraphicsCommandList_SetDescriptorHeaps(l_direct, 1, &gpuh);
            ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(l_direct, gpu, cpu, tkeys, ones, 0, NULL);
            barrier(l_direct, NULL, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12GraphicsCommandList_SetComputeRootSignature(l_direct, rs_typed);
            ID3D12GraphicsCommandList_SetPipelineState(l_direct, pso);
            ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(l_direct, 0, 4, c, 0);
            ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(l_direct, 1, gpu);
            ID3D12GraphicsCommandList_Dispatch(l_direct, CANDIDATES / 64, 1, 1);
        } else {
            barrier(l_direct, keys, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            ID3D12GraphicsCommandList_CopyBufferRegion(l_direct, keys, 0, keys_init, 0, SLOTS * 8);
            barrier(l_direct, keys, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch(l_direct, pso, keys, it, CANDIDATES, SLOTS);
        }
        barrier(l_direct, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(l_direct, rb_keys, 0, dst, 0, (UINT64)n * 8);
        barrier(l_direct, dst, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        execute(q_direct, l_direct);
        ID3D12CommandQueue_Signal(q_direct, fence, ++fence_v);
        if (!event_until(fence, fence_v)) { bad_iters = MIN_ITERS; break; }
        ID3D12Resource_Map(rb_keys, 0, NULL, (void **)&p);
        if (!p) { bad_iters = MIN_ITERS; break; }
        for (i = 0; i < n; i++) {
            UINT64 w = i % stride ? ~(UINT64)0 : want[i / stride];
            if (p[i] != w) { if (first < 0) first = (int)i; bad++; }
        }
        if (bad && reports++ < 3) {
            UINT64 g = p[first], w = first % stride ? ~(UINT64)0 : want[first / stride];
            printf("        %s, iteration %u: %d of %u elements wrong; element %d: %#llx (t bits %#x, thread %u), expected %#llx "
                   "(t bits %#x, thread %u)%s\n", what, it, bad, n, first, (unsigned long long)g, (UINT32)(g >> 32), (UINT32)g,
                   (unsigned long long)w, (UINT32)(w >> 32), (UINT32)w,
                   first % stride ? " -- outside the slots: the clear" :
                   !g || g == ~(UINT64)0 ? " -- never written" :
                   (UINT32)g < CANDIDATES && candidate_key(it, (UINT32)g) != g ? " -- t of one thread, index of another" : "");
        }
        if (bad) bad_iters++;
        ID3D12Resource_Unmap(rb_keys, 0, NULL);
        reopen(a_direct, l_direct);
    }
    CHECK(bad_iters == 0, "%s: %d of %d iterations left an element that is not the exact minimum (or the clear value)", what, bad_iters, MIN_ITERS);
}

/* Groupshared: every group of 256 threads leaves the minimum of its 4 slots
 * (64 contenders each) in gkeys[group * 4 + slot]. */
static void case_min_groupshared(void) {
    static UINT64 want[GROUPS * 4];
    int bad_iters = 0, reports = 0;
    UINT it, i;
    for (it = 1; it <= MIN_ITERS; it++) {
        const UINT64 *p = NULL;
        UINT c[4] = { it, CANDIDATES, 4, 0 };
        int bad = 0, first = -1;
        for (i = 0; i < GROUPS * 4; i++) want[i] = ~(UINT64)0;
        for (i = 0; i < CANDIDATES; i++) { UINT64 k = candidate_key(it, i); UINT j = (i / 256) * 4 + i % 4; if (k < want[j]) want[j] = k; }
        ID3D12GraphicsCommandList_SetComputeRootSignature(l_direct, rs);
        ID3D12GraphicsCommandList_SetPipelineState(l_direct, pso_min_gs);
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(l_direct, 0, 4, c, 0);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(l_direct, 1, ID3D12Resource_GetGPUVirtualAddress(gkeys));
        ID3D12GraphicsCommandList_Dispatch(l_direct, GROUPS, 1, 1);
        barrier(l_direct, gkeys, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(l_direct, rb_keys, 0, gkeys, 0, (UINT64)GROUPS * 4 * 8);
        barrier(l_direct, gkeys, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        execute(q_direct, l_direct);
        ID3D12CommandQueue_Signal(q_direct, fence, ++fence_v);
        if (!event_until(fence, fence_v)) { bad_iters = MIN_ITERS; break; }
        ID3D12Resource_Map(rb_keys, 0, NULL, (void **)&p);
        if (!p) { bad_iters = MIN_ITERS; break; }
        for (i = 0; i < GROUPS * 4; i++) if (p[i] != want[i]) { if (first < 0) first = (int)i; bad++; }
        if (bad && reports++ < 3) {
            UINT64 g = p[first], w = want[first];
            printf("        groupshared, iteration %u: %d of %d slots wrong; group %d slot %d: %#llx (t bits %#x, thread %u), expected %#llx "
                   "(t bits %#x, thread %u)%s\n", it, bad, GROUPS * 4, first / 4, first % 4, (unsigned long long)g, (UINT32)(g >> 32),
                   (UINT32)g, (unsigned long long)w, (UINT32)(w >> 32), (UINT32)w,
                   g == ~(UINT64)0 ? " -- never lowered" :
                   (UINT32)g < CANDIDATES && candidate_key(it, (UINT32)g) != g ? " -- t of one thread, index of another" : "");
        }
        if (bad) bad_iters++;
        ID3D12Resource_Unmap(rb_keys, 0, NULL);
        reopen(a_direct, l_direct);
    }
    CHECK(bad_iters == 0, "groupshared uint64_t InterlockedMin: %d of %d iterations left a slot that is not the exact minimum", bad_iters, MIN_ITERS);
}

int main(void) {
    HRESULT hr;
    int k;
    printf("wine-lab d3d12: compute results read back through a READBACK buffer when the fence passes\n\n");

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
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;  ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q_direct);
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;    ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q_copy);
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE; ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q_compute);
    }
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a_direct);
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_COPY, &IID_ID3D12CommandAllocator, (void **)&a_copy);
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_COMPUTE, &IID_ID3D12CommandAllocator, (void **)&a_compute);
    for (k = 0; k < 3; k++)
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a_side[k]);
    if (a_direct) ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_direct, NULL, &IID_ID3D12GraphicsCommandList, (void **)&l_direct);
    if (a_copy) ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_COPY, a_copy, NULL, &IID_ID3D12GraphicsCommandList, (void **)&l_copy);
    if (a_compute) ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_COMPUTE, a_compute, NULL, &IID_ID3D12GraphicsCommandList, (void **)&l_compute);
    for (k = 0; k < 3; k++)
        if (a_side[k]) ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_side[k], NULL, &IID_ID3D12GraphicsCommandList, (void **)&l_side[k]);
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence_b);
    CHECK(q_direct && q_copy && q_compute && l_direct && l_copy && l_compute && l_side[0] && l_side[1] && l_side[2] && fence && fence_b,
          "direct, copy and compute queues, their lists, and two fences created");
    if (!q_direct || !q_copy || !q_compute || !l_direct || !l_copy || !l_compute || !l_side[0] || !l_side[1] || !l_side[2] || !fence || !fence_b)
        goto done;

    hr = ID3D12Device_CreateRootSignature(dev, 0, rb_fill_dxil, sizeof rb_fill_dxil, &IID_ID3D12RootSignature, (void **)&rs);
    if (rs) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
        pd.pRootSignature = rs;
        pd.CS.pShaderBytecode = rb_fill_dxil; pd.CS.BytecodeLength = sizeof rb_fill_dxil;
        hr = ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_fill);
        pd.CS.pShaderBytecode = rb_min_raw_dxil; pd.CS.BytecodeLength = sizeof rb_min_raw_dxil;
        ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_min_raw);
        pd.CS.pShaderBytecode = rb_min_struct_dxil; pd.CS.BytecodeLength = sizeof rb_min_struct_dxil;
        ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_min_struct);
        pd.CS.pShaderBytecode = rb_min_groupshared_dxil; pd.CS.BytecodeLength = sizeof rb_min_groupshared_dxil;
        ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_min_gs);
    }
    ID3D12Device_CreateRootSignature(dev, 0, rb_min_typed_dxil, sizeof rb_min_typed_dxil, &IID_ID3D12RootSignature, (void **)&rs_typed);
    if (rs_typed) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd; ZeroMemory(&pd, sizeof pd);
        pd.pRootSignature = rs_typed;
        pd.CS.pShaderBytecode = rb_min_typed_dxil; pd.CS.BytecodeLength = sizeof rb_min_typed_dxil;
        ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_min_typed);
    }
    CHECK(rs && pso_fill, "root signature (embedded) and the fill pipeline created (%#lx)", hr);
    CHECK(pso_min_raw && pso_min_struct && pso_min_typed && pso_min_gs,
          "64-bit InterlockedMin pipelines created (raw, structured, typed and groupshared, cs_6_6)");
    if (!pso_fill) goto done;

    def = make_buffer(D3D12_HEAP_TYPE_DEFAULT, BYTES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    side = make_buffer(D3D12_HEAP_TYPE_DEFAULT, 64 * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, BYTES, D3D12_RESOURCE_STATE_COPY_DEST);
    rb_big = make_buffer(D3D12_HEAP_TYPE_READBACK, BIG, D3D12_RESOURCE_STATE_COPY_DEST);
    keys = make_buffer(D3D12_HEAP_TYPE_DEFAULT, SLOTS * 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    keys_init = make_buffer(D3D12_HEAP_TYPE_UPLOAD, SLOTS * 8, D3D12_RESOURCE_STATE_GENERIC_READ);
    rb_keys = make_buffer(D3D12_HEAP_TYPE_READBACK, (UINT64)TKEYS * 8, D3D12_RESOURCE_STATE_COPY_DEST);
    tkeys = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)TKEYS * 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gkeys = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)GROUPS * 4 * 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(def && side && rb && rb_big && keys && keys_init && rb_keys && tkeys && gkeys, "buffers created (1 MB and 16 MB readback)");
    if (!def || !side || !rb || !rb_big || !keys || !keys_init || !rb_keys || !tkeys || !gkeys) goto done;
    {   /* the R32G32_UINT view of tkeys, in a CPU-only heap (the clear's CPU handle) and a shader-visible one */
        D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
        D3D12_UNORDERED_ACCESS_VIEW_DESC u; ZeroMemory(&u, sizeof u);
        D3D12_CPU_DESCRIPTOR_HANDLE c;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&cpuh);
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&gpuh);
        if (!cpuh || !gpuh) { CHECK(FALSE, "descriptor heaps created"); goto done; }
        u.Format = DXGI_FORMAT_R32G32_UINT; u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; u.Buffer.NumElements = TKEYS;
        cpuh->lpVtbl->GetCPUDescriptorHandleForHeapStart(cpuh, &c);
        ID3D12Device_CreateUnorderedAccessView(dev, tkeys, NULL, &u, c);
        gpuh->lpVtbl->GetCPUDescriptorHandleForHeapStart(gpuh, &c);
        ID3D12Device_CreateUnorderedAccessView(dev, tkeys, NULL, &u, c);
    }
    {
        void *p = NULL;
        ID3D12Resource_Map(keys_init, 0, NULL, &p);
        if (p) { memset(p, 0xff, SLOTS * 8); ID3D12Resource_Unmap(keys_init, 0, NULL); }
    }

    printf("\n[1] one list, Signal, the CPU spins on GetCompletedValue (%d iterations)\n", ITERS);
    case_one_list();
    printf("\n[2] dispatch and copy batched with other lists\n");
    case_batched();
    printf("\n[3] copy on a copy queue behind Wait\n");
    case_copy_queue();
    printf("\n[4] compute queue\n");
    case_compute_queue();
    case_forward();
    printf("\n[5] frames in flight, 16 MB readback buffer\n");
    case_ring();
    printf("\n[6] 64-bit InterlockedMin, %d threads onto %d slots\n", CANDIDATES, SLOTS);
    if (pso_min_raw) case_min64(pso_min_raw, "RWByteAddressBuffer InterlockedMin64", FALSE);
    if (pso_min_struct) case_min64(pso_min_struct, "RWStructuredBuffer<uint64_t> InterlockedMin", FALSE);
    if (pso_min_typed) case_min64(pso_min_typed, "RWBuffer<uint64_t> (R32G32_UINT, cleared to ones by ClearUnorderedAccessViewUint) InterlockedMin", TRUE);
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9; ZeroMemory(&o9, sizeof o9);
        ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof o9);
        if (o9.AtomicInt64OnGroupSharedSupported && pso_min_gs) case_min_groupshared();
        else CHECK(!o9.AtomicInt64OnGroupSharedSupported, "groupshared 64-bit atomics not claimed (D3D12_OPTIONS9); the groupshared case is skipped");
    }

done:
    if (gpuh) ID3D12DescriptorHeap_Release(gpuh);
    if (cpuh) ID3D12DescriptorHeap_Release(cpuh);
    if (gkeys) ID3D12Resource_Release(gkeys);
    if (tkeys) ID3D12Resource_Release(tkeys);
    if (rb_keys) ID3D12Resource_Release(rb_keys);
    if (keys_init) ID3D12Resource_Release(keys_init);
    if (keys) ID3D12Resource_Release(keys);
    if (rb_big) ID3D12Resource_Release(rb_big);
    if (rb) ID3D12Resource_Release(rb);
    if (side) ID3D12Resource_Release(side);
    if (def) ID3D12Resource_Release(def);
    if (pso_min_gs) ID3D12PipelineState_Release(pso_min_gs);
    if (pso_min_typed) ID3D12PipelineState_Release(pso_min_typed);
    if (pso_min_struct) ID3D12PipelineState_Release(pso_min_struct);
    if (pso_min_raw) ID3D12PipelineState_Release(pso_min_raw);
    if (pso_fill) ID3D12PipelineState_Release(pso_fill);
    if (rs_typed) ID3D12RootSignature_Release(rs_typed);
    if (rs) ID3D12RootSignature_Release(rs);
    if (fence_b) ID3D12Fence_Release(fence_b);
    if (fence) ID3D12Fence_Release(fence);
    for (k = 0; k < 3; k++) {
        if (l_side[k]) ID3D12GraphicsCommandList_Release(l_side[k]);
        if (a_side[k]) ID3D12CommandAllocator_Release(a_side[k]);
    }
    if (l_compute) ID3D12GraphicsCommandList_Release(l_compute);
    if (l_copy) ID3D12GraphicsCommandList_Release(l_copy);
    if (l_direct) ID3D12GraphicsCommandList_Release(l_direct);
    if (a_compute) ID3D12CommandAllocator_Release(a_compute);
    if (a_copy) ID3D12CommandAllocator_Release(a_copy);
    if (a_direct) ID3D12CommandAllocator_Release(a_direct);
    if (q_compute) ID3D12CommandQueue_Release(q_compute);
    if (q_copy) ID3D12CommandQueue_Release(q_copy);
    if (q_direct) ID3D12CommandQueue_Release(q_direct);
    if (dev) ID3D12Device_Release(dev);
    printf("\n%d checks, %d failures\n", checks, fails);
    fflush(stdout);
    return fails;
}
