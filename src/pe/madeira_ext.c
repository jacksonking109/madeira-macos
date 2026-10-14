/*
 * PE side of the wine-lab port's extra Metal calls: thunks into d3d12.so.
 * See src/madeira_ext.h.
 */
#include <windows.h>
#include <string.h>
#include "wineunixlib.h"
#include "madeira_ext.h"
#include "madeira_ir_abi.h"

#define CALL(code, params) WINE_UNIX_CALL(madeira_unix_##code, params)
#define P(x) ((uint64_t)(ULONG_PTR)(x))

int madeira_ext_init(void)
{
    return !__wine_init_unix_call();
}

obj_handle_t MTLDevice_newPlacementHeap(obj_handle_t device, uint64_t size, uint64_t options)
{
    struct madeira_unix_heap_params p = { .owner = device, .arg = size, .options = options };
    CALL(newPlacementHeap, &p);
    return p.ret;
}

void MTLDevice_heapBufferSizeAndAlign(obj_handle_t device, uint64_t length, uint64_t options, uint64_t *size, uint64_t *align)
{
    struct madeira_unix_heap_params p = { .owner = device, .arg = length, .options = options };
    CALL(heapBufferSizeAndAlign, &p);
    *size = p.ret_size;
    *align = p.ret_align;
}

void MTLDevice_heapTextureSizeAndAlign(obj_handle_t device, struct WMTTextureInfo *info, uint64_t *size, uint64_t *align)
{
    struct madeira_unix_heap_params p = { .owner = device, .info = P(info) };
    CALL(heapTextureSizeAndAlign, &p);
    *size = p.ret_size;
    *align = p.ret_align;
}

obj_handle_t MTLHeap_newBufferAtOffset(obj_handle_t heap, struct WMTBufferInfo *info, uint64_t offset)
{
    struct madeira_unix_heap_params p = { .owner = heap, .arg = offset, .info = P(info) };
    CALL(heapNewBufferAtOffset, &p);
    return p.ret;
}

obj_handle_t MTLHeap_newTextureAtOffset(obj_handle_t heap, struct WMTTextureInfo *info, uint64_t offset)
{
    struct madeira_unix_heap_params p = { .owner = heap, .arg = offset, .info = P(info) };
    CALL(heapNewTextureAtOffset, &p);
    return p.ret;
}

obj_handle_t MTLDevice_newResidencySet(obj_handle_t device, uint64_t capacity)
{
    struct madeira_unix_heap_params p = { .owner = device, .arg = capacity };
    CALL(newResidencySet, &p);
    return p.ret;
}

void MTLCommandQueue_addResidencySet(obj_handle_t queue, obj_handle_t set)
{
    struct madeira_unix_pair_params p = { queue, set };
    CALL(queueAddResidencySet, &p);
}

void MTLResidencySet_addAllocation(obj_handle_t set, obj_handle_t allocation)
{
    struct madeira_unix_pair_params p = { set, allocation };
    CALL(residencySetAddAllocation, &p);
}

void MTLResidencySet_removeAllocation(obj_handle_t set, obj_handle_t allocation)
{
    struct madeira_unix_pair_params p = { set, allocation };
    CALL(residencySetRemoveAllocation, &p);
}

void MTLResidencySet_commit(obj_handle_t set)
{
    struct madeira_unix_pair_params p = { set, 0 };
    CALL(residencySetCommit, &p);
}

obj_handle_t MTLDevice_newRenderPipelineStateVD(obj_handle_t device, const struct WMTRenderPipelineInfo *info,
                                                const struct WMTVertexDescriptorInfo *vd, obj_handle_t *err_out)
{
    struct madeira_unix_pso_params p = { .device = device, .info = P(info), .extra = P(vd) };
    CALL(newRenderPipelineStateVD, &p);
    if (err_out) *err_out = p.ret_error;
    return p.ret_pso;
}

obj_handle_t MTLDevice_newGeometryEmulationPipelineState(obj_handle_t device, const struct WMTMeshRenderPipelineInfo *info,
                                                         const struct WMTGeometryEmulationInfo *ge, obj_handle_t *err_out)
{
    struct madeira_unix_pso_params p = { .device = device, .info = P(info), .extra = P(ge) };
    CALL(newGeometryEmulationPipelineState, &p);
    if (err_out) *err_out = p.ret_error;
    return p.ret_pso;
}

void MadeiraCtl(struct madeira_ctl_args *args)
{
    CALL(ctl, args);
}

void MadeiraIRConvert(struct madeira_ir_convert_args *args)
{
    CALL(ir_convert, args);
}

obj_handle_t madeira_cb_with_error_info(obj_handle_t queue)
{
    struct madeira_unix_pair_params p = { queue, 0 };
    CALL(commandBufferErrorInfo, &p);
    return p.b;
}

obj_handle_t madeira_render_encoder_sampled(obj_handle_t cmdbuf, struct WMTRenderPassInfo *info, obj_handle_t sample_buffer, uint32_t start_index, uint32_t end_index)
{
    struct madeira_unix_sampled_encoder_params p;
    memset(&p, 0, sizeof p);
    p.cmdbuf = cmdbuf; p.info = P(info); p.sample_buffer = sample_buffer; p.start_index = start_index; p.end_index = end_index;
    CALL(renderEncoderSampled, &p);
    return p.ret;
}

obj_handle_t madeira_compute_encoder_sampled(obj_handle_t cmdbuf, int concurrent, obj_handle_t sample_buffer, uint32_t start_index, uint32_t end_index)
{
    struct madeira_unix_sampled_encoder_params p;
    memset(&p, 0, sizeof p);
    p.cmdbuf = cmdbuf; p.sample_buffer = sample_buffer; p.start_index = start_index; p.end_index = end_index; p.concurrent = concurrent ? 1 : 0;
    CALL(computeEncoderSampled, &p);
    return p.ret;
}

void madeira_render_set_store_actions(obj_handle_t encoder, const uint8_t color[8], uint8_t depth, uint8_t stencil)
{
    struct madeira_unix_store_actions_params p;
    memset(&p, 0, sizeof p);
    p.encoder = encoder; memcpy(p.color, color, 8); p.depth = depth; p.stencil = stencil;
    CALL(renderSetStoreActions, &p);
}

int madeira_fencepost(obj_handle_t h, int op)
{
    struct madeira_unix_pair_params p = { h, (obj_handle_t)op };
    CALL(fencepost, &p);
    return p.b != 0;
}

void madeira_cb_enqueue(const obj_handle_t *cbs, unsigned n)
{
    struct madeira_unix_pair_params p = { P(cbs), n };   /* a = the array, b = its length */
    CALL(commandBufferEnqueue, &p);
}

void madeira_blit_t2b(obj_handle_t encoder, const struct wmtcmd_blit_copy_from_texture_to_buffer *cmd, uint32_t options)
{
    struct madeira_unix_blit_params p = { encoder, P(cmd), options };
    if (!options) {
        MTLBlitCommandEncoder_encodeCommands(encoder, (const struct wmtcmd_base *)cmd);
        return;
    }
    CALL(blitT2B, &p);
}

obj_handle_t madeira_new_clear_pipeline(obj_handle_t device, uint32_t depth_pf, uint32_t stencil_pf, uint32_t samples)
{
    struct madeira_unix_pso_params p = { .device = device, .info = (uint64_t)depth_pf | ((uint64_t)stencil_pf << 32), .extra = samples };
    CALL(newClearPipeline, &p);
    return p.ret_pso;
}

void madeira_blit_b2t(obj_handle_t encoder, const struct wmtcmd_blit_copy_from_buffer_to_texture *cmd, uint32_t options)
{
    struct madeira_unix_blit_params p = { encoder, P(cmd), options };
    if (!options) {
        MTLBlitCommandEncoder_encodeCommands(encoder, (const struct wmtcmd_base *)cmd);
        return;
    }
    CALL(blitB2T, &p);
}
