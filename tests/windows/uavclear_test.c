/*  Texture UAV clears across formats, for the wine-lab D3D12 port.
 *
 *  ClearUnorderedAccessViewUint writes each value's low bits into its channel
 *  whatever the view's format, so a Uint clear of an R16_FLOAT view to 0x3c00
 *  stores 1.0h; ClearUnorderedAccessViewFloat converts, to integers for
 *  UINT/SINT views. The Last of Us Part I clears R16_FLOAT views with Uint
 *  values; eight of its clears were skipped.
 *
 *  Each case clears a 64x16 texture (created in UNORDERED_ACCESS, one mip)
 *  through a UAV written to a CPU-only heap and to a shader-visible one, copies
 *  it to a readback buffer and checks every texel.
 *
 *  Cases:
 *    1  R16_FLOAT view, Uint 0x3c00: every texel 1.0h
 *    2  R16_TYPELESS resource, R16_FLOAT view, Uint 0x4000: every texel 2.0h
 *    3  R32_FLOAT view, Uint 0x3f800000: every texel 1.0f
 *    4  Float clears of integer views: R16_UINT to 3.0 (3), R32_SINT to -2.0 (-2)
 *    5  the view in the descriptor belongs to another resource of another texel
 *       size (an R16_FLOAT view of texture 1), the call names an R32_TYPELESS
 *       texture: the named texture is cleared whole with the value's raw bits.
 *       Not valid D3D12; The Last of Us Part I's skipped clears had this shape.
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

static int checks, fails;
#define CHECK(cond, ...) do {                                                  \
    checks++;                                                                  \
    if (!(cond)) { fails++; printf("  FAIL  " __VA_ARGS__); printf("\n"); }     \
    else         { printf("  ok    " __VA_ARGS__); printf("\n"); }             \
    fflush(stdout);                                                            \
} while (0)

typedef HRESULT (WINAPI *pfn_create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef const char *(*pfn_marker)(void);

enum { W = 64, H = 16, PITCH = 256 };   /* 64 texels of up to 4 bytes: one 256-byte row */
static const UINT64 RB_SIZE = (UINT64)PITCH * H;

static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;
static ID3D12Resource     *rb;
static ID3D12DescriptorHeap *cpuh, *gpuh;

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

static ID3D12Resource *make_tex(DXGI_FORMAT fmt) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt;
    d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                         NULL, &IID_ID3D12Resource, (void **)&r);
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
    DWORD w = WaitForSingleObject(ev, 15000);
    CloseHandle(ev);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    if (w != WAIT_OBJECT_0) { printf("        fence wait timed out\n"); return FALSE; }
    return TRUE;
}

/* Writes a 2D UAV of `tex` in `fmt` to slot `slot` of both heaps. */
static void put_uav(ID3D12Resource *tex, DXGI_FORMAT fmt, UINT slot,
                    D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu) {
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c, gc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC u; ZeroMemory(&u, sizeof u);
    u.Format = fmt; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    cpuh->lpVtbl->GetCPUDescriptorHandleForHeapStart(cpuh, &c); c.ptr += (SIZE_T)slot * inc;
    gpuh->lpVtbl->GetCPUDescriptorHandleForHeapStart(gpuh, &gc); gc.ptr += (SIZE_T)slot * inc;
    gpuh->lpVtbl->GetGPUDescriptorHandleForHeapStart(gpuh, gpu); gpu->ptr += (UINT64)slot * inc;
    ID3D12Device_CreateUnorderedAccessView(dev, tex, NULL, &u, c);
    ID3D12Device_CreateUnorderedAccessView(dev, tex, NULL, &u, gc);
    *cpu = c;
}

/* Copies `tex` (texel size `bpp`, footprint format `cfmt`) to the readback
 * buffer, submits, and counts the texels that differ from `want`; the first
 * one is handed back in `first_bad`. Returns -1 when nothing was read. */
static int read_and_count(ID3D12Resource *tex, DXGI_FORMAT cfmt, UINT bpp, UINT32 want, UINT32 *first_bad) {
    D3D12_TEXTURE_COPY_LOCATION dl, sl; ZeroMemory(&dl, sizeof dl); ZeroMemory(&sl, sizeof sl);
    dl.pResource = rb; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint.Footprint.Format = cfmt;
    dl.PlacedFootprint.Footprint.Width = W; dl.PlacedFootprint.Footprint.Height = H;
    dl.PlacedFootprint.Footprint.Depth = 1; dl.PlacedFootprint.Footprint.RowPitch = PITCH;
    sl.pResource = tex; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    BYTE *p = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
    if (p) { memset(p, 0xCD, (size_t)RB_SIZE); ID3D12Resource_Unmap(rb, 0, NULL); }

    barrier(tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dl, 0, 0, 0, &sl, NULL);
    barrier(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!submit_and_wait()) return -1;

    p = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
    if (!p) { printf("        readback Map failed\n"); return -1; }
    int bad = 0;
    UINT x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            UINT32 v = 0;
            memcpy(&v, p + (SIZE_T)y * PITCH + (SIZE_T)x * bpp, bpp);
            if (v != want) { if (!bad) *first_bad = v; bad++; }
        }
    ID3D12Resource_Unmap(rb, 0, NULL);
    return bad;
}

static float half_to_float(UINT32 h) {
    UINT32 sign = (h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
    float f;
    if (e == 0) { f = (float)m / 16777216.0f; return sign ? -f : f; }   /* denormal: m * 2^-24 */
    if (e == 31) x = sign | 0x7f800000 | (m << 13);
    else x = sign | ((e - 15 + 127) << 23) | (m << 13);
    memcpy(&f, &x, 4);
    return f;
}

/* ------------------------------------------------------------------ cases */

/* One clear of `res` through a `view_fmt` UAV of `view_res`, checked texel by
 * texel against `want` read back in `cfmt`. */
static void clear_case(const char *name, ID3D12Resource *view_res, DXGI_FORMAT view_fmt, ID3D12Resource *res,
                       DXGI_FORMAT cfmt, UINT bpp, BOOL is_float, const void *values, UINT32 want, UINT slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    UINT32 first = 0;
    int bad;
    put_uav(view_res, view_fmt, slot, &cpu, &gpu);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &gpuh);
    if (is_float) ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(list, gpu, cpu, res, values, 0, NULL);
    else ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, gpu, cpu, res, values, 0, NULL);
    bad = read_and_count(res, cfmt, bpp, want, &first);
    if (bpp == 2 && cfmt != DXGI_FORMAT_R16_UINT)
        CHECK(bad == 0, "%s: %d of %d texels differ from %#x (%gh)", name, bad < 0 ? W * H : bad, W * H,
              want, half_to_float(want));
    else
        CHECK(bad == 0, "%s: %d of %d texels differ from %#x", name, bad < 0 ? W * H : bad, W * H, want);
    if (bad > 0) printf("        first differing texel: %#x\n", first);
}

int main(void) {
    HRESULT hr;
    ID3D12Resource *t_h16 = NULL, *t_h16tl = NULL, *t_f32 = NULL, *t_u16 = NULL, *t_s32 = NULL, *t_r32tl = NULL;
    printf("wine-lab d3d12: texture UAV clears across formats\n\n");

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

    rb = make_readback(RB_SIZE);
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 8;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&cpuh);
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&gpuh);
    }
    t_h16 = make_tex(DXGI_FORMAT_R16_FLOAT);
    t_h16tl = make_tex(DXGI_FORMAT_R16_TYPELESS);
    t_f32 = make_tex(DXGI_FORMAT_R32_FLOAT);
    t_u16 = make_tex(DXGI_FORMAT_R16_UINT);
    t_s32 = make_tex(DXGI_FORMAT_R32_SINT);
    t_r32tl = make_tex(DXGI_FORMAT_R32_TYPELESS);
    CHECK(rb && cpuh && gpuh && t_h16 && t_h16tl && t_f32 && t_u16 && t_s32 && t_r32tl,
          "readback buffer, CPU and shader-visible heaps, six 64x16 UAV textures created");
    if (!rb || !cpuh || !gpuh || !t_h16 || !t_h16tl || !t_f32 || !t_u16 || !t_s32 || !t_r32tl) goto done;

    printf("\n[1] ClearUnorderedAccessViewUint, R16_FLOAT view\n");
    {
        static const UINT v[4] = { 0x3c00, 0, 0, 0 };
        clear_case("R16_FLOAT Uint 0x3c00", t_h16, DXGI_FORMAT_R16_FLOAT, t_h16, DXGI_FORMAT_R16_FLOAT, 2, FALSE, v, 0x3c00, 0);
    }
    printf("\n[2] ClearUnorderedAccessViewUint, R16_FLOAT view of an R16_TYPELESS resource\n");
    {
        static const UINT v[4] = { 0x4000, 0, 0, 0 };
        clear_case("R16_TYPELESS as R16_FLOAT Uint 0x4000", t_h16tl, DXGI_FORMAT_R16_FLOAT, t_h16tl,
                   DXGI_FORMAT_R16_FLOAT, 2, FALSE, v, 0x4000, 1);
    }
    printf("\n[3] ClearUnorderedAccessViewUint, R32_FLOAT view\n");
    {
        static const UINT v[4] = { 0x3f800000, 0, 0, 0 };
        clear_case("R32_FLOAT Uint 0x3f800000", t_f32, DXGI_FORMAT_R32_FLOAT, t_f32, DXGI_FORMAT_R32_FLOAT, 4, FALSE,
                   v, 0x3f800000, 2);
    }
    printf("\n[4] ClearUnorderedAccessViewFloat, integer views\n");
    {
        static const FLOAT three[4] = { 3.0f, 0, 0, 0 }, minus_two[4] = { -2.0f, 0, 0, 0 };
        clear_case("R16_UINT Float 3.0", t_u16, DXGI_FORMAT_R16_UINT, t_u16, DXGI_FORMAT_R16_UINT, 2, TRUE, three, 3, 3);
        clear_case("R32_SINT Float -2.0", t_s32, DXGI_FORMAT_R32_SINT, t_s32, DXGI_FORMAT_R32_SINT, 4, TRUE, minus_two,
                   (UINT32)-2, 4);
    }
    printf("\n[5] ClearUnorderedAccessViewUint naming another resource than the view's (TLOU Part I)\n");
    {
        static const UINT v[4] = { 0x12345678, 0, 0, 0 };
        clear_case("R16_FLOAT view of texture 1, R32_TYPELESS texture named", t_h16, DXGI_FORMAT_R16_FLOAT, t_r32tl,
                   DXGI_FORMAT_R32_TYPELESS, 4, FALSE, v, 0x12345678, 5);
    }

done:
    if (t_r32tl) ID3D12Resource_Release(t_r32tl);
    if (t_s32) ID3D12Resource_Release(t_s32);
    if (t_u16) ID3D12Resource_Release(t_u16);
    if (t_f32) ID3D12Resource_Release(t_f32);
    if (t_h16tl) ID3D12Resource_Release(t_h16tl);
    if (t_h16) ID3D12Resource_Release(t_h16);
    if (gpuh) ID3D12DescriptorHeap_Release(gpuh);
    if (cpuh) ID3D12DescriptorHeap_Release(cpuh);
    if (rb) ID3D12Resource_Release(rb);
    if (fence) ID3D12Fence_Release(fence);
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (dev) ID3D12Device_Release(dev);
    printf("\n%d checks, %d failures\n", checks, fails);
    fflush(stdout);
    return fails;
}
