/*  Depth-stencil PLANE copies, for the wine-lab D3D12 port.
 *
 *  CopyTextureRegion names one plane of a D32_FLOAT_S8X24_UINT texture by its
 *  subresource (0 = depth, 1 = stencil) and must leave the other plane alone.
 *  Metal moves one plane of a combined depth-stencil texture only through a
 *  buffer with MTLBlitOptionDepthFromDepthStencil / StencilFromDepthStencil.
 *  DXMT's winemetal encodes copyFromBuffer:toTexture: without options, so a
 *  stencil-plane copy INTO such a texture landed in its DEPTH plane: The Last
 *  of Us Part I copies a stencil plane into the depth target its forward
 *  translucency passes test against (MainPsCheapForward), whose depth then read
 *  0x71717171 (1.2e30) / 0x01010101 -- translucent surfaces drew where they are
 *  hidden (dark "shadows") and wrote garbage HDR values (green/magenta when
 *  blurred). And a depth-plane copy between two combined textures was a whole
 *  texture copy that also overwrote the destination's stencil.
 *
 *  Each case reads both planes of the 64x16 destination back per texel.
 *    1  stencil plane A -> B: B's stencil = A's, B's depth unchanged
 *    2  depth plane A -> B: B's depth = A's, B's stencil unchanged
 *    3  upload buffer -> B's stencil plane (a pattern): depth unchanged
 *    4  upload buffer -> B's depth plane (a pattern): stencil unchanged
 *    5  R8_UINT colour texture -> B's stencil plane: depth unchanged
 *    6  R32_FLOAT colour texture -> B's depth plane: stencil unchanged
 *    7  B's stencil plane -> R8_UINT texture, B's depth plane -> R32_FLOAT texture
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

enum { W = 64, H = 16, PITCH = 256, PLANE_BYTES = PITCH * H };

static ID3D12Device       *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence        *fence;
static UINT64              fence_v;
static ID3D12Resource     *rb, *up;
static ID3D12DescriptorHeap *dsvh;

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

static ID3D12Resource *make_tex(DXGI_FORMAT fmt, BOOL ds) {
    D3D12_HEAP_PROPERTIES hp; ZeroMemory(&hp, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d; ZeroMemory(&d, sizeof d);
    D3D12_CLEAR_VALUE cv; ZeroMemory(&cv, sizeof cv);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = ds ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE;
    cv.Format = fmt; cv.DepthStencil.Depth = 1.0f;
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d,
                                         ds ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COPY_DEST,
                                         ds ? &cv : NULL, &IID_ID3D12Resource, (void **)&r);
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
    DWORD w = WaitForSingleObject(ev, 15000);
    CloseHandle(ev);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    if (w != WAIT_OBJECT_0) { printf("        fence wait timed out\n"); return FALSE; }
    return TRUE;
}

static D3D12_TEXTURE_COPY_LOCATION sub_loc(ID3D12Resource *r, UINT sub) {
    D3D12_TEXTURE_COPY_LOCATION l; ZeroMemory(&l, sizeof l);
    l.pResource = r; l.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; l.SubresourceIndex = sub;
    return l;
}
static D3D12_TEXTURE_COPY_LOCATION fp_loc(ID3D12Resource *buf, UINT64 off, DXGI_FORMAT fmt) {
    D3D12_TEXTURE_COPY_LOCATION l; ZeroMemory(&l, sizeof l);
    l.pResource = buf; l.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    l.PlacedFootprint.Offset = off;
    l.PlacedFootprint.Footprint.Format = fmt;
    l.PlacedFootprint.Footprint.Width = W; l.PlacedFootprint.Footprint.Height = H;
    l.PlacedFootprint.Footprint.Depth = 1; l.PlacedFootprint.Footprint.RowPitch = PITCH;
    return l;
}

static void clear_ds(ID3D12Resource *t, UINT slot, float depth, UINT8 stencil) {
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    dsvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(dsvh, &h); h.ptr += (SIZE_T)slot * inc;
    ID3D12Device_CreateDepthStencilView(dev, t, NULL, h);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, h, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                    depth, stencil, 0, NULL);
}

/* Both planes of `t` (in COPY_SOURCE) into the readback buffer: depth at 0, stencil at PLANE_BYTES. */
static void record_read_planes(ID3D12Resource *t) {
    D3D12_TEXTURE_COPY_LOCATION d0 = fp_loc(rb, 0, DXGI_FORMAT_R32_TYPELESS), s0 = sub_loc(t, 0);
    D3D12_TEXTURE_COPY_LOCATION d1 = fp_loc(rb, PLANE_BYTES, DXGI_FORMAT_R8_TYPELESS), s1 = sub_loc(t, 1);
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &d0, 0, 0, 0, &s0, NULL);
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &d1, 0, 0, 0, &s1, NULL);
}

static float pat_depth(UINT x, UINT y) { return (float)(x + y * W) / (float)(W * H); }
static UINT8 pat_stencil(UINT x, UINT y, UINT salt) { return (UINT8)(x * 7 + y * 13 + salt); }

/* Counts texels whose depth differs from want_d (or the depth pattern when use_pat_d) and whose stencil
 * differs from want_s (or the stencil pattern with salt when pat_s >= 0). */
static void check_planes(const char *what, BOOL use_pat_d, float want_d, int pat_s, UINT8 want_s) {
    BYTE *p = NULL; UINT x, y, bad_d = 0, bad_s = 0; UINT32 first_d = 0; UINT first_s = 0;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
    if (!p) { CHECK(0, "%s: readback mapped", what); return; }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            float d; UINT8 s;
            memcpy(&d, p + y * PITCH + x * 4, 4);
            s = p[PLANE_BYTES + y * PITCH + x];
            if (d != (use_pat_d ? pat_depth(x, y) : want_d)) { if (!bad_d) memcpy(&first_d, &d, 4); bad_d++; }
            if (s != (pat_s >= 0 ? pat_stencil(x, y, (UINT)pat_s) : want_s)) { if (!bad_s) first_s = s; bad_s++; }
        }
    ID3D12Resource_Unmap(rb, 0, NULL);
    CHECK(bad_d == 0, "%s: depth plane %s (%u/%u texels wrong, first %#x)", what,
          use_pat_d ? "= the pattern" : "as expected", bad_d, W * H, first_d);
    CHECK(bad_s == 0, "%s: stencil plane %s (%u/%u texels wrong, first %#x)", what,
          pat_s >= 0 ? "= the pattern" : "as expected", bad_s, W * H, first_s);
}

/* Readback memory to a known non-answer before each read. */
static void poison_rb(void) {
    BYTE *p = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
    if (p) { memset(p, 0xCD, 2 * PLANE_BYTES); ID3D12Resource_Unmap(rb, 0, NULL); }
}

int main(void) {
    HRESULT hr;
    ID3D12Resource *A = NULL, *B = NULL, *c8 = NULL, *c32 = NULL;
    BYTE *u = NULL; UINT x, y;
    printf("wine-lab d3d12: depth-stencil plane copies (D32_FLOAT_S8X24_UINT)\n\n");

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
        D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof hd);
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (alloc)
            ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                           &IID_ID3D12GraphicsCommandList, (void **)&list);
        ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 4;
        ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsvh);
    }
    CHECK(queue && alloc && list && fence && dsvh, "queue, allocator, list, fence and DSV heap created");
    if (!queue || !alloc || !list || !fence || !dsvh) goto done;

    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, 2 * PLANE_BYTES, D3D12_RESOURCE_STATE_COPY_DEST);
    up = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 4 * PLANE_BYTES, D3D12_RESOURCE_STATE_GENERIC_READ);
    A = make_tex(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, TRUE);
    B = make_tex(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, TRUE);
    c8 = make_tex(DXGI_FORMAT_R8_UINT, FALSE);
    c32 = make_tex(DXGI_FORMAT_R32_FLOAT, FALSE);
    CHECK(rb && up && A && B && c8 && c32, "buffers and textures created");
    if (!rb || !up || !A || !B || !c8 || !c32) goto done;

    /* upload layout: [0] stencil pattern salt 1, [1] depth pattern, [2] stencil pattern salt 2, [3] depth pattern */
    ID3D12Resource_Map(up, 0, NULL, (void **)&u);
    if (!u) { CHECK(0, "upload buffer mapped"); goto done; }
    memset(u, 0, 4 * PLANE_BYTES);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            float d = pat_depth(x, y);
            u[y * PITCH + x] = pat_stencil(x, y, 1);
            memcpy(u + PLANE_BYTES + y * PITCH + x * 4, &d, 4);
            u[2 * PLANE_BYTES + y * PITCH + x] = pat_stencil(x, y, 2);
            memcpy(u + 3 * PLANE_BYTES + y * PITCH + x * 4, &d, 4);
        }
    ID3D12Resource_Unmap(up, 0, NULL);

    /* 1: stencil plane A -> B */
    printf("\n1: stencil plane A -> B (A: 0.25 / 0x71, B: 0.75 / 0x05)\n");
    clear_ds(A, 0, 0.25f, 0x71);
    clear_ds(B, 1, 0.75f, 0x05);
    barrier(A, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(B, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION d = sub_loc(B, 1), s = sub_loc(A, 1);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &d, 0, 0, 0, &s, NULL);
    }
    barrier(B, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    poison_rb(); record_read_planes(B);
    if (submit_and_wait()) check_planes("stencil A -> B", FALSE, 0.75f, -1, 0x71);

    /* 2: depth plane A -> B */
    printf("\n2: depth plane A -> B (A re-cleared to 0.5 / 0x21)\n");
    barrier(A, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    clear_ds(A, 0, 0.5f, 0x21);
    barrier(A, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(B, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION d = sub_loc(B, 0), s = sub_loc(A, 0);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &d, 0, 0, 0, &s, NULL);
    }
    barrier(B, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    poison_rb(); record_read_planes(B);
    if (submit_and_wait()) check_planes("depth A -> B", FALSE, 0.5f, -1, 0x71);

    /* 3: upload -> stencil plane */
    printf("\n3: upload buffer -> B's stencil plane\n");
    barrier(B, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION d = sub_loc(B, 1), s = fp_loc(up, 0, DXGI_FORMAT_R8_TYPELESS);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &d, 0, 0, 0, &s, NULL);
    }
    barrier(B, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    poison_rb(); record_read_planes(B);
    if (submit_and_wait()) check_planes("upload -> stencil", FALSE, 0.5f, 1, 0);

    /* 4: upload -> depth plane */
    printf("\n4: upload buffer -> B's depth plane\n");
    barrier(B, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION d = sub_loc(B, 0), s = fp_loc(up, PLANE_BYTES, DXGI_FORMAT_R32_TYPELESS);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &d, 0, 0, 0, &s, NULL);
    }
    barrier(B, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    poison_rb(); record_read_planes(B);
    if (submit_and_wait()) check_planes("upload -> depth", TRUE, 0, 1, 0);

    /* 5 + 6: colour textures -> planes */
    printf("\n5/6: R8_UINT -> B's stencil plane, then R32_FLOAT -> B's depth plane\n");
    {
        D3D12_TEXTURE_COPY_LOCATION d8 = sub_loc(c8, 0), s8 = fp_loc(up, 2 * PLANE_BYTES, DXGI_FORMAT_R8_UINT);
        D3D12_TEXTURE_COPY_LOCATION d32 = sub_loc(c32, 0), s32 = fp_loc(up, 3 * PLANE_BYTES, DXGI_FORMAT_R32_FLOAT);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &d8, 0, 0, 0, &s8, NULL);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &d32, 0, 0, 0, &s32, NULL);
    }
    barrier(c8, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(c32, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    /* B's depth back to a constant first, so case 6 is seen to write it */
    barrier(B, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    clear_ds(B, 1, 0.125f, 0x05);
    barrier(B, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION d = sub_loc(B, 1), s = sub_loc(c8, 0);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &d, 0, 0, 0, &s, NULL);
    }
    barrier(B, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    poison_rb(); record_read_planes(B);
    if (submit_and_wait()) check_planes("R8_UINT -> stencil", FALSE, 0.125f, 2, 0);
    barrier(B, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION d = sub_loc(B, 0), s = sub_loc(c32, 0);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &d, 0, 0, 0, &s, NULL);
    }
    barrier(B, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    poison_rb(); record_read_planes(B);
    if (submit_and_wait()) check_planes("R32_FLOAT -> depth", TRUE, 0, 2, 0);

    /* 7: planes -> colour textures (the source side of the same path) */
    printf("\n7: B's planes -> fresh R8_UINT / R32_FLOAT textures\n");
    {
        ID3D12Resource *o8 = make_tex(DXGI_FORMAT_R8_UINT, FALSE), *o32 = make_tex(DXGI_FORMAT_R32_FLOAT, FALSE);
        CHECK(o8 && o32, "output textures created");
        if (o8 && o32) {
            D3D12_TEXTURE_COPY_LOCATION d8 = sub_loc(o8, 0), s8 = sub_loc(B, 1), d32 = sub_loc(o32, 0), s32 = sub_loc(B, 0);
            D3D12_TEXTURE_COPY_LOCATION r0 = fp_loc(rb, 0, DXGI_FORMAT_R32_FLOAT), r1 = fp_loc(rb, PLANE_BYTES, DXGI_FORMAT_R8_UINT);
            ID3D12GraphicsCommandList_CopyTextureRegion(list, &d8, 0, 0, 0, &s8, NULL);
            ID3D12GraphicsCommandList_CopyTextureRegion(list, &d32, 0, 0, 0, &s32, NULL);
            barrier(o8, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barrier(o32, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            poison_rb();
            ID3D12GraphicsCommandList_CopyTextureRegion(list, &r0, 0, 0, 0, &d32, NULL);
            ID3D12GraphicsCommandList_CopyTextureRegion(list, &r1, 0, 0, 0, &d8, NULL);
            if (submit_and_wait()) check_planes("planes -> colour", TRUE, 0, 2, 0);
        }
        if (o8) ID3D12Resource_Release(o8);
        if (o32) ID3D12Resource_Release(o32);
    }

done:
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails;
}
