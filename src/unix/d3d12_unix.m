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
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* What Wine's unixlib.h provides; DXMT's unix side defines these itself too. */
typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *args);
#define STATUS_SUCCESS 0
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002)
#include "madeira_ext.h"
#include "madeira_ir_abi.h"
#include "madeira_cfg.h"

int madeira_ir_convert(void *args);   /* madeira_ir_unix.mm */

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
    NSError *err = nil;
    unsigned i;

    @autoreleasepool {
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
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
    /* outlives the pool; the caller owns it (mad_nserror_drop) */
    p->ret_error = (obj_handle_t)(!p->ret_pso && err ? [err retain] : nil);
    [d release];
    }
    return STATUS_SUCCESS;
}

/* wine-lab port: Metal Shader Converter geometry/tessellation emulation.
 *
 * Builds the object/mesh pipeline IRRuntimeNewGeometryEmulationPipeline and
 * IRRuntimeNewGeometryTessellationEmulationPipeline build (Apple's
 * metal_irconverter_runtime.h, Apache-2.0; followed, not included, since its
 * Objective-C path assumes ARC and this file is MRC), as Madeira's rmetald does
 * for RM_OP_NEW_GEOM_PSO_INFO:
 *
 *   geometry (ge->tessellation == 0): object = "<vs>.dxil_irconverter_object_shader"
 *     specialised with tessellationEnabled = NO; mesh = the geometry function with
 *     vertex_shader_output_size_fc; the stage-in function linked to the object stage.
 *   tessellation (ge->tessellation != 0): object = the same with tessellationEnabled
 *     = YES, linking irconverter_stage_in_shader and the hull library's
 *     irconverter_hull_shader; mesh = the geometry function, or with no geometry
 *     library of its own (madeira_d3d12.c passes the domain library for both) the
 *     domain library's passthrough named by geometry_function, linking the hull
 *     library's irconverter_tessellator and the domain library's
 *     irconverter_dxil_domain_shader.
 *
 * madeira_d3d12.c then encodes DrawMeshThreadgroups with the converter's draw
 * contract (mad_gs_draw / mad_ts_draw). Returns the PSO (+1) and any NSError
 * retained, like new_render_pipeline_state_vd. */
static NSError *geom_error(NSString *fmt, ...)
{
    NSString *msg;
    va_list ap;
    va_start(ap, fmt);
    msg = [[[NSString alloc] initWithFormat:fmt arguments:ap] autorelease];
    va_end(ap);
    return [NSError errorWithDomain:@"madeira.d3d12.geometry-emulation" code:1
                           userInfo:@{ NSLocalizedDescriptionKey : msg }];
}

/* A mesh pipeline must have a fragment function (Metal asserts on a nil one,
 * where a vertex pipeline accepts it), so a pipeline without a pixel shader
 * (depth-only passes) gets an empty one. One per device, kept for the process; a compile that failed is
 * remembered (cached_fn nil) rather than retried on every pipeline. The result is autoreleased so a caller
 * on another thread keeps it alive across a switch of device. */
static id<MTLFunction> empty_fragment_function(id<MTLDevice> dev)
{
    static id<MTLDevice> cached_dev;
    static id<MTLFunction> cached_fn;
    id<MTLFunction> fn = nil;
    @synchronized ([MTLMeshRenderPipelineDescriptor class]) {
        if (cached_dev != dev) {
            NSError *e = nil;
            id<MTLLibrary> l = [dev newLibraryWithSource:@"fragment void madeira_empty_fragment() {}" options:nil error:&e];
            [cached_fn release];
            [cached_dev release];
            cached_fn = [l newFunctionWithName:@"madeira_empty_fragment"];
            cached_dev = [dev retain];
            [l release];
            if (!cached_fn) fprintf(stderr, "[d3d12.so] empty fragment function did not compile: %s\n",
                                    e ? [[e localizedDescription] UTF8String] : "(no error)");
        }
        fn = [[cached_fn retain] autorelease];
    }
    return fn;
}

static NTSTATUS new_geometry_emulation_pipeline_state(void *args)
{
    struct madeira_unix_pso_params *p = args;
    const struct WMTMeshRenderPipelineInfo *info = (const void *)(uintptr_t)p->info;
    const struct WMTGeometryEmulationInfo *ge = (const void *)(uintptr_t)p->extra;
    id<MTLRenderPipelineState> pso = nil;
    NSError *err = nil;

    p->ret_pso = 0;
    p->ret_error = 0;
    if (!info || !ge) return STATUS_SUCCESS;

    @autoreleasepool {
        id<MTLDevice> dev = (id<MTLDevice>)p->device;
        id<MTLLibrary> Ls = (id<MTLLibrary>)ge->stagein_library, Lv = (id<MTLLibrary>)ge->vertex_library;
        id<MTLLibrary> Lg = (id<MTLLibrary>)ge->geometry_library, Lf = (id<MTLLibrary>)ge->fragment_library;
        id<MTLLibrary> Lh = (id<MTLLibrary>)ge->hull_library, Ld = (id<MTLLibrary>)ge->domain_library;
        NSString *vsname = [NSString stringWithFormat:@"%.*s.dxil_irconverter_object_shader", MADEIRA_FN_LEN, ge->vertex_function];
        NSString *gsname = [[[NSString alloc] initWithBytes:ge->geometry_function
                                                     length:strnlen(ge->geometry_function, MADEIRA_FN_LEN)
                                                   encoding:NSUTF8StringEncoding] autorelease];
        NSString *psname = [[[NSString alloc] initWithBytes:ge->fragment_function
                                                     length:strnlen(ge->fragment_function, MADEIRA_FN_LEN)
                                                   encoding:NSUTF8StringEncoding] autorelease];
        MTLMeshRenderPipelineDescriptor *d = [[[MTLMeshRenderPipelineDescriptor alloc] init] autorelease];
        MTLFunctionConstantValues *cv = [[[MTLFunctionConstantValues alloc] init] autorelease];
        id<MTLFunction> fsi = nil, fo = nil, fm = nil, ff = nil, fh = nil, ft = nil, fd = nil;
        int vsz = (int)ge->gs_vertex_size_bytes;
        BOOL tess = ge->tessellation != 0;
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
            if (info->immutable_object_buffers & (1u << i)) d.objectBuffers[i].mutability = MTLMutabilityImmutable;
            if (info->immutable_mesh_buffers & (1u << i)) d.meshBuffers[i].mutability = MTLMutabilityImmutable;
            if (info->immutable_fragment_buffers & (1u << i)) d.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
        }
        d.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
        d.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
        d.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
        d.rasterizationEnabled = info->rasterization_enabled;
        d.rasterSampleCount = info->raster_sample_count ? info->raster_sample_count : 1;
        if (info->payload_memory_length) d.payloadMemoryLength = info->payload_memory_length;
        d.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->mesh_tgsize_is_multiple_of_sgwidth;
        d.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->object_tgsize_is_multiple_of_sgwidth;

        /* Metal aborts the process (not an exception) on a mesh descriptor without
         * a mesh function, so every lookup is checked before the pipeline call. */
        if (!dev || !Lv || !gsname.length || (!Lg && !tess) || (tess && (!Ls || !Lh || !Ld))) {
            err = geom_error(@"%s pipeline is missing a library or name (stage-in %d vertex %d geometry %d fragment %d hull %d domain %d, gs '%@')",
                             tess ? "tessellation" : "geometry", !!Ls, !!Lv, !!Lg, !!Lf, !!Lh, !!Ld, gsname);
            goto done;
        }

        if (!tess) {
            BOOL off = NO;
            /* The stage-in library holds one function; a vertex stage without an
             * input layout may come without one, and then nothing is linked. */
            if (Ls && Ls.functionNames.count) fsi = [[Ls newFunctionWithName:Ls.functionNames.firstObject] autorelease];
            if (Ls && !fsi) { err = geom_error(@"stage-in library has no function"); goto done; }
            [cv setConstantValue:&off type:MTLDataTypeBool withName:@"tessellationEnabled"];
            fo = [[Lv newFunctionWithName:vsname constantValues:cv error:&err] autorelease];
            if (!fo) { if (!err) err = geom_error(@"object function '%@' not found", vsname); goto done; }
            [cv setConstantValue:&vsz type:MTLDataTypeInt withName:@"vertex_shader_output_size_fc"];
            fm = [[Lg newFunctionWithName:gsname constantValues:cv error:&err] autorelease];
            if (!fm) { if (!err) err = geom_error(@"mesh (geometry) function '%@' not found", gsname); goto done; }
            if (fsi) {
                MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions];
                lf.functions = @[ fsi ];
                d.objectLinkedFunctions = lf;
            }
        } else {
            BOOL on = YES, so = NO;
            float maxf = ge->max_tessellation_factor;
            MTLFunctionConstantValues *hc = [[[MTLFunctionConstantValues alloc] init] autorelease];
            MTLLinkedFunctions *olf = [MTLLinkedFunctions linkedFunctions], *mlf = [MTLLinkedFunctions linkedFunctions];

            fsi = [[Ls newFunctionWithName:@"irconverter_stage_in_shader"] autorelease];
            if (!fsi) { err = geom_error(@"stage-in library has no irconverter_stage_in_shader"); goto done; }
            fd = [[Ld newFunctionWithName:@"irconverter_dxil_domain_shader"] autorelease];
            if (!fd) { err = geom_error(@"domain library has no irconverter_dxil_domain_shader"); goto done; }
            [cv setConstantValue:&on type:MTLDataTypeBool withName:@"tessellationEnabled"];
            fo = [[Lv newFunctionWithName:vsname constantValues:cv error:&err] autorelease];
            if (!fo) { if (!err) err = geom_error(@"object function '%@' not found", vsname); goto done; }
            /* The hull library holds the hull function (control point and patch
             * constant phases) and the tessellator. */
            if (!(maxf >= 1.0f && maxf <= 64.0f)) maxf = 64.0f;
            [hc setConstantValue:&vsz type:MTLDataTypeInt withName:@"vertex_shader_output_size_fc"];
            [hc setConstantValue:&maxf type:MTLDataTypeFloat withName:@"max_tessellation_factor_fc"];
            fh = [[Lh newFunctionWithName:@"irconverter_hull_shader" constantValues:hc error:&err] autorelease];
            if (!fh) { if (!err) err = geom_error(@"hull library has no irconverter_hull_shader"); goto done; }
            ft = [[Lh newFunctionWithName:@"irconverter_tessellator" constantValues:hc error:&err] autorelease];
            if (!ft) { if (!err) err = geom_error(@"hull library has no irconverter_tessellator"); goto done; }
            if (Lg && Lg != Ld) {   /* a real geometry stage after the domain stage */
                MTLFunctionConstantValues *gc = [[[MTLFunctionConstantValues alloc] init] autorelease];
                [gc setConstantValue:&on type:MTLDataTypeBool withName:@"tessellationEnabled"];
                [gc setConstantValue:&so type:MTLDataTypeBool withName:@"streamOutEnabled"];
                [gc setConstantValue:&vsz type:MTLDataTypeInt withName:@"vertex_shader_output_size_fc"];
                fm = [[Lg newFunctionWithName:gsname constantValues:gc error:&err] autorelease];
            } else {                /* kIR{Triangle,Line,Point}PassthroughGeometryShader */
                fm = [[Ld newFunctionWithName:gsname] autorelease];
            }
            if (!fm) { if (!err) err = geom_error(@"mesh (geometry) function '%@' not found", gsname); goto done; }
            olf.functions = @[ fsi, fh ];
            d.objectLinkedFunctions = olf;
            mlf.functions = @[ ft, fd ];
            d.meshLinkedFunctions = mlf;
        }

        if (psname.length) {
            ff = Lf ? [[Lf newFunctionWithName:psname] autorelease] : nil;
            if (!ff) { err = geom_error(@"fragment function '%@' not found", psname); goto done; }
        } else if (info->fragment_function) {
            ff = (id<MTLFunction>)info->fragment_function;
        } else if (!(ff = empty_fragment_function(dev))) {
            err = geom_error(@"no pixel shader, and the empty fragment function did not compile");
            goto done;
        }

        d.objectFunction = fo;
        d.meshFunction = fm;
        d.fragmentFunction = ff;
        err = nil;
        pso = [dev newRenderPipelineStateWithMeshDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&err];
        if (pso) err = nil;
    done:
        [err retain];   /* outlives the pool; the caller owns it, as with new_render_pipeline_state_vd */
    }
    if (!pso) {
        static unsigned said;
        if (said++ < 8) @autoreleasepool {
            fprintf(stderr, "[d3d12.so] %s emulation pipeline failed: %s\n", ge->tessellation ? "tessellation" : "geometry",
                    err ? [[err localizedDescription] UTF8String] : "(no error)");
        }
    }
    p->ret_pso = (obj_handle_t)pso;
    p->ret_error = (obj_handle_t)err;
    return STATUS_SUCCESS;
}

/* ---- control and blits ------------------------------------------------- */

/* Config reads: an environment override (madeira.cfg key "upload-swap" is
 * MADEIRA_UPLOAD_SWAP), else madeira.cfg in $MADEIRA_DOCS_DIR (default
 * ~/Documents). Everything else (frame capture, overlay switches, shared perf
 * counters) is off in this port. */
static NTSTATUS ctl(void *args)
{
    struct madeira_ctl_args *a = args;
    a->ret = 0;
    switch (a->op) {
    case 0: {   /* wine-lab port: capture request -- creating $MADEIRA_DOCS_DIR/capture-now captures the next frame */
        const char *dir = getenv("MADEIRA_DOCS_DIR");
        char path[1024];
        if (dir && snprintf(path, sizeof path, "%s/capture-now", dir) < (int)sizeof path && unlink(path) == 0) {
            fprintf(stderr, "[d3d12.so] capture-now: capturing the next frame\n");
            a->ret = 1;
        }
        break;
    }
    case 2: {
        char env[300] = "MADEIRA_", *q = env + 8;
        const char *k, *v;
        for (k = a->name; *k && q < env + sizeof(env) - 1; k++, q++)
            *q = (*k == '-' || *k == '.') ? '_' : (char)((*k >= 'a' && *k <= 'z') ? *k - 32 : *k);
        *q = 0;
        if (!a->ptr || !a->len) break;
        if ((v = getenv(env))) {
            snprintf((char *)(uintptr_t)a->ptr, a->len, "%s", v);
            a->ret = 1;
        } else if (madeira_cfg_get(a->name, (char *)(uintptr_t)a->ptr, a->len)) {
            a->ret = 1;
        }
        break;
    }
    case 1: {   /* wine-lab port: frame capture file -> $MADEIRA_DOCS_DIR/capture/<name> */
        const char *docs = getenv("MADEIRA_DOCS_DIR");
        char path[1200];
        FILE *f;
        if (!docs || !*docs || !a->ptr || !a->len || strchr(a->name, '/')) break;
        snprintf(path, sizeof path, "%s/capture", docs);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/capture/%s", docs, a->name);
        if ((f = fopen(path, "wb"))) {
            a->ret = fwrite((const void *)(uintptr_t)a->ptr, 1, a->len, f) == a->len;
            fclose(f);
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
    madeira_ir_convert(args);   /* the outcome travels in ret_status */
    return STATUS_SUCCESS;
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

/* wine-lab port: winemetal's copyFromBuffer:toTexture: has no options; a depth or
 * stencil plane of a combined depth-stencil destination needs them (see madeira_ext.h). */
static NTSTATUS blit_b2t(void *args)
{
    struct madeira_unix_blit_params *p = args;
    const struct wmtcmd_blit_copy_from_buffer_to_texture *c = (const void *)(uintptr_t)p->cmd;
    [(id<MTLBlitCommandEncoder>)p->encoder copyFromBuffer:(id<MTLBuffer>)c->src
                                             sourceOffset:c->src_offset
                                        sourceBytesPerRow:c->bytes_per_row
                                      sourceBytesPerImage:c->bytes_per_image
                                               sourceSize:MTLSizeMake(c->size.width, c->size.height, c->size.depth)
                                                toTexture:(id<MTLTexture>)c->dst
                                         destinationSlice:c->slice
                                         destinationLevel:c->level
                                        destinationOrigin:MTLOriginMake(c->origin.x, c->origin.y, c->origin.z)
                                                  options:(MTLBlitOption)p->options];
    return STATUS_SUCCESS;
}

static NTSTATUS command_buffer_error_info(void *args)
{
    struct madeira_unix_pair_params *p = args;
    MTLCommandBufferDescriptor *desc = [MTLCommandBufferDescriptor new];
    desc.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
    p->b = (obj_handle_t)[(id<MTLCommandQueue>)p->a commandBufferWithDescriptor:desc];
    [desc release];
    return STATUS_SUCCESS;
}

static NTSTATUS command_buffer_enqueue(void *args)
{
    struct madeira_unix_pair_params *p = args;
    const obj_handle_t *cbs = (const obj_handle_t *)(uintptr_t)p->a;
    uint64_t i;
    for (i = 0; i < p->b; i++) [(id<MTLCommandBuffer>)cbs[i] enqueue];
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
    [madeira_unix_commandBufferErrorInfo] = command_buffer_error_info,
    [madeira_unix_commandBufferEnqueue] = command_buffer_enqueue,
    [madeira_unix_blitB2T] = blit_b2t,
};

_Static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == madeira_unix_count,
               "every unix call needs an entry");
