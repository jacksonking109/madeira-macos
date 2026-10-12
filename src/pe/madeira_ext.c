/*
 * PE side of the wine-lab port's extra Metal calls: thunks into d3d12.so.
 * See src/madeira_ext.h.
 */
#include <windows.h>
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
