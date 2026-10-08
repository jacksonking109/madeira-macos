/*
 * Unix side of d3d12.dll (wine-lab port of madeira-d3d12): the Metal calls
 * madeira-d3d12 needs beyond DXMT v0.80's winemetal. See src/madeira_ext.h.
 *
 * Handles are winemetal's (Objective-C object pointers), so objects created by
 * DXMT's winemetal.so can be used here directly, and objects returned from here
 * are released with winemetal's NSObject_release.
 *
 * Pipeline and texture-descriptor code follows DXMT's winemetal_unix.c
 * (Copyright (c) 2023 Feifan He, MIT License).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What Wine's unixlib.h provides; DXMT's unix side defines these itself too. */
typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *args);
#define STATUS_SUCCESS 0
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002)
#include "madeira_ext.h"
#include "madeira_ir_abi.h"

static MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format)
{
    return (MTLPixelFormat)ORIGINAL_FORMAT(format);
}

static void fill_texture_descriptor(MTLTextureDescriptor *desc, const struct WMTTextureInfo *info)
{
    desc.textureType = (MTLTextureType)info->type;
    desc.pixelFormat = to_metal_pixel_format(info->pixel_format);
    desc.width = info->width;
    desc.height = info->height;
    desc.depth = info->depth;
    desc.arrayLength = info->array_length;
    desc.mipmapLevelCount = info->mipmap_level_count;
    desc.sampleCount = info->sample_count;
    desc.usage = (MTLTextureUsage)info->usage;
    desc.resourceOptions = (MTLResourceOptions)info->options;
}

/* ---- placement heaps ---------------------------------------------------- */

static NTSTATUS new_placement_heap(void *args)
{
    struct madeira_unix_heap_params *p = args;
    MTLHeapDescriptor *d = [[MTLHeapDescriptor alloc] init];
    d.type = MTLHeapTypePlacement;
    d.size = p->arg;
    d.resourceOptions = (MTLResourceOptions)p->options;
    p->ret = (obj_handle_t)[(id<MTLDevice>)p->owner newHeapWithDescriptor:d];
    [d release];
    return STATUS_SUCCESS;
}

static NTSTATUS heap_buffer_size_and_align(void *args)
{
    struct madeira_unix_heap_params *p = args;
    MTLSizeAndAlign sa = [(id<MTLDevice>)p->owner heapBufferSizeAndAlignWithLength:p->arg
                                                                           options:(MTLResourceOptions)p->options];
    p->ret_size = sa.size;
    p->ret_align = sa.align;
    return STATUS_SUCCESS;
}

static NTSTATUS heap_texture_size_and_align(void *args)
{
    struct madeira_unix_heap_params *p = args;
    MTLTextureDescriptor *d = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(d, (const struct WMTTextureInfo *)(uintptr_t)p->info);
    MTLSizeAndAlign sa = [(id<MTLDevice>)p->owner heapTextureSizeAndAlignWithDescriptor:d];
    [d release];
    p->ret_size = sa.size;
    p->ret_align = sa.align;
    return STATUS_SUCCESS;
}

/* Fills the same outputs as winemetal's MTLDevice_newBuffer. */
static NTSTATUS heap_new_buffer_at_offset(void *args)
{
    struct madeira_unix_heap_params *p = args;
    struct WMTBufferInfo *info = (struct WMTBufferInfo *)(uintptr_t)p->info;
    id<MTLBuffer> b = [(id<MTLHeap>)p->owner newBufferWithLength:info->length
                                                         options:(MTLResourceOptions)info->options
                                                          offset:p->arg];
    info->memory.ptr = (b && [b storageMode] != MTLStorageModePrivate) ? [b contents] : NULL;
    info->gpu_address = b ? [b gpuAddress] : 0;
    p->ret = (obj_handle_t)b;
    return STATUS_SUCCESS;
}

/* Fills the same outputs as winemetal's MTLDevice_newTexture. */
static NTSTATUS heap_new_texture_at_offset(void *args)
{
    struct madeira_unix_heap_params *p = args;
    struct WMTTextureInfo *info = (struct WMTTextureInfo *)(uintptr_t)p->info;
    MTLTextureDescriptor *d = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(d, info);
    id<MTLTexture> t = [(id<MTLHeap>)p->owner newTextureWithDescriptor:d offset:p->arg];
    [d release];
    info->gpu_resource_id = t ? [t gpuResourceID]._impl : 0;
    info->mach_port = 0;
    p->ret = (obj_handle_t)t;
    return STATUS_SUCCESS;
}

/* ---- residency sets (macOS 15+) ---------------------------------------- */

static NTSTATUS new_residency_set(void *args)
{
    struct madeira_unix_heap_params *p = args;
    p->ret = 0;
    if (@available(macOS 15.0, *)) {
        MTLResidencySetDescriptor *d = [[MTLResidencySetDescriptor alloc] init];
        NSError *e = nil;
        d.initialCapacity = p->arg;
        p->ret = (obj_handle_t)[(id<MTLDevice>)p->owner newResidencySetWithDescriptor:d error:&e];
        if (!p->ret) fprintf(stderr, "[d3d12.so] newResidencySet: %s\n", e ? [[e localizedDescription] UTF8String] : "?");
        [d release];
    }
    return STATUS_SUCCESS;
}

static NTSTATUS queue_add_residency_set(void *args)
{
    struct madeira_unix_pair_params *p = args;
    if (@available(macOS 15.0, *)) [(id<MTLCommandQueue>)p->a addResidencySet:(id<MTLResidencySet>)p->b];
    return STATUS_SUCCESS;
}

static NTSTATUS residency_set_add(void *args)
{
    struct madeira_unix_pair_params *p = args;
    if (@available(macOS 15.0, *)) [(id<MTLResidencySet>)p->a addAllocation:(id<MTLAllocation>)p->b];
    return STATUS_SUCCESS;
}

static NTSTATUS residency_set_remove(void *args)
{
    struct madeira_unix_pair_params *p = args;
    if (@available(macOS 15.0, *)) [(id<MTLResidencySet>)p->a removeAllocation:(id<MTLAllocation>)p->b];
    return STATUS_SUCCESS;
}

static NTSTATUS residency_set_commit(void *args)
{
    struct madeira_unix_pair_params *p = args;
    if (@available(macOS 15.0, *)) [(id<MTLResidencySet>)p->a commit];
    return STATUS_SUCCESS;
}

/* ---- pipelines ---------------------------------------------------------- */

/* winemetal's MTLDevice_newRenderPipelineState, plus a vertex descriptor. */
static NTSTATUS new_render_pipeline_state_vd(void *args)
{
    struct madeira_unix_pso_params *p = args;
    const struct WMTRenderPipelineInfo *info = (const void *)(uintptr_t)p->info;
    const struct WMTVertexDescriptorInfo *vd = (const void *)(uintptr_t)p->extra;
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    NSError *err = nil;
    unsigned i;

    for (i = 0; i < 8; i++) {
        d.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
        d.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
        d.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;
        d.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
        d.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;
        d.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
        d.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
        d.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
        d.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
    }
    for (i = 0; i < 31; i++) {
        if (info->immutable_fragment_buffers & (1u << i)) d.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
        if (info->immutable_vertex_buffers & (1u << i)) d.vertexBuffers[i].mutability = MTLMutabilityImmutable;
    }
    d.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
    d.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
    d.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
    d.rasterizationEnabled = info->rasterization_enabled;
    d.rasterSampleCount = info->raster_sample_count;
    d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)info->input_primitive_topology;
    d.tessellationPartitionMode = (MTLTessellationPartitionMode)info->tessellation_partition_mode;
    d.tessellationFactorStepFunction = (MTLTessellationFactorStepFunction)info->tessellation_factor_step;
    d.tessellationOutputWindingOrder = (MTLWinding)info->tessellation_output_winding_order;
    d.maxTessellationFactor = info->max_tessellation_factor;
    d.vertexFunction = (id<MTLFunction>)info->vertex_function;
    d.fragmentFunction = (id<MTLFunction>)info->fragment_function;

    if (vd && (vd->attribute_mask || vd->layout_mask)) {
        MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
        for (i = 0; i < 31; i++) {
            if (vd->attribute_mask & (1u << i)) {
                v.attributes[i].format = (MTLVertexFormat)vd->attributes[i].format;
                v.attributes[i].offset = vd->attributes[i].offset;
                v.attributes[i].bufferIndex = vd->attributes[i].buffer_index;
            }
            if (vd->layout_mask & (1u << i)) {
                v.layouts[i].stride = vd->layouts[i].stride;
                v.layouts[i].stepFunction = (MTLVertexStepFunction)vd->layouts[i].step_function;
                v.layouts[i].stepRate = vd->layouts[i].step_rate;
            }
        }
        d.vertexDescriptor = v;
    }

    p->ret_pso = (obj_handle_t)[(id<MTLDevice>)p->device newRenderPipelineStateWithDescriptor:d error:&err];
    p->ret_error = (obj_handle_t)(err ? [err retain] : nil);
    [d release];
    return STATUS_SUCCESS;
}

static NTSTATUS new_geometry_emulation_pipeline_state(void *args)
{
    struct madeira_unix_pso_params *p = args;
    static int said;
    if (!said++) fprintf(stderr, "[d3d12.so] geometry/tessellation emulation pipelines are not ported yet\n");
    p->ret_pso = 0;
    p->ret_error = 0;
    return STATUS_SUCCESS;
}

/* ---- control and blits ------------------------------------------------- */

/* Config reads come from the environment: madeira.cfg key "upload-swap" is
 * MADEIRA_UPLOAD_SWAP. Everything else (frame capture, overlay switches,
 * shared perf counters) is off in this port. */
static NTSTATUS ctl(void *args)
{
    struct madeira_ctl_args *a = args;
    a->ret = 0;
    switch (a->op) {
    case 2: {
        char env[300] = "MADEIRA_", *q = env + 8;
        const char *k, *v;
        for (k = a->name; *k && q < env + sizeof(env) - 1; k++, q++)
            *q = (*k == '-' || *k == '.') ? '_' : (char)((*k >= 'a' && *k <= 'z') ? *k - 32 : *k);
        *q = 0;
        if ((v = getenv(env)) && a->ptr && a->len) {
            snprintf((char *)(uintptr_t)a->ptr, a->len, "%s", v);
            a->ret = 1;
        }
        break;
    }
    case 3: {   /* GPU start/end of a command buffer: struct { u64 cb; double start, end; } */
        struct { uint64_t cb; double start, end; } *t = (void *)(uintptr_t)a->ptr;
        if (t && a->len >= sizeof(*t) && t->cb) {
            id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)t->cb;
            t->start = cb.GPUStartTime;
            t->end = cb.GPUEndTime;
            a->ret = 1;
        }
        break;
    }
    default:
        break;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ir_convert(void *args)
{
    struct madeira_ir_convert_args *a = args;
    static int said;
    if (!said++) fprintf(stderr, "[d3d12.so] Metal Shader Converter service not ported yet\n");
    (void)a;
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS blit_t2b(void *args)
{
    struct madeira_unix_blit_params *p = args;
    const struct wmtcmd_blit_copy_from_texture_to_buffer *c = (const void *)(uintptr_t)p->cmd;
    [(id<MTLBlitCommandEncoder>)p->encoder copyFromTexture:(id<MTLTexture>)c->src
                                               sourceSlice:c->slice
                                               sourceLevel:c->level
                                              sourceOrigin:MTLOriginMake(c->origin.x, c->origin.y, c->origin.z)
                                                sourceSize:MTLSizeMake(c->size.width, c->size.height, c->size.depth)
                                                  toBuffer:(id<MTLBuffer>)c->dst
                                         destinationOffset:c->offset
                                    destinationBytesPerRow:c->bytes_per_row
                                  destinationBytesPerImage:c->bytes_per_image
                                                   options:(MTLBlitOption)p->options];
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] = {
    [madeira_unix_newPlacementHeap] = new_placement_heap,
    [madeira_unix_heapBufferSizeAndAlign] = heap_buffer_size_and_align,
    [madeira_unix_heapTextureSizeAndAlign] = heap_texture_size_and_align,
    [madeira_unix_heapNewBufferAtOffset] = heap_new_buffer_at_offset,
    [madeira_unix_heapNewTextureAtOffset] = heap_new_texture_at_offset,
    [madeira_unix_newResidencySet] = new_residency_set,
    [madeira_unix_queueAddResidencySet] = queue_add_residency_set,
    [madeira_unix_residencySetAddAllocation] = residency_set_add,
    [madeira_unix_residencySetRemoveAllocation] = residency_set_remove,
    [madeira_unix_residencySetCommit] = residency_set_commit,
    [madeira_unix_newRenderPipelineStateVD] = new_render_pipeline_state_vd,
    [madeira_unix_newGeometryEmulationPipelineState] = new_geometry_emulation_pipeline_state,
    [madeira_unix_ctl] = ctl,
    [madeira_unix_ir_convert] = ir_convert,
    [madeira_unix_blitT2B] = blit_t2b,
};

_Static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == madeira_unix_count,
               "every unix call needs an entry");
