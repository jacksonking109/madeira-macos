/*
 * Metal entry points madeira-d3d12 needs beyond DXMT v0.80's winemetal.
 *
 * Madeira adds these to its own DXMT fork; this port serves them from d3d12.dll's
 * own unix library (d3d12.so) instead, so DXMT stays unmodified and MIT. Handles
 * are winemetal's: Objective-C object pointers in the unix address space.
 *
 * The struct layouts are this port's own (shared by src/pe/madeira_ext.c and
 * src/unix/d3d12_unix.m), reconstructed from how madeira_d3d12.c uses them.
 */
#ifndef MADEIRA_EXT_H
#define MADEIRA_EXT_H

#include <stdint.h>
#include "winemetal.h"

#define MADEIRA_FN_LEN 128

struct WMTVertexAttributeInfo {
    uint32_t format;        /* MTLVertexFormat */
    uint32_t offset;
    uint32_t buffer_index;
    uint32_t reserved;
};

struct WMTVertexLayoutInfo {
    uint32_t stride;
    uint32_t step_function; /* MTLVertexStepFunction */
    uint32_t step_rate;
    uint32_t reserved;
};

struct WMTVertexDescriptorInfo {
    uint32_t attribute_mask;    /* bit i: attributes[i] is set */
    uint32_t layout_mask;       /* bit i: layouts[i] is set */
    struct WMTVertexAttributeInfo attributes[31];
    struct WMTVertexLayoutInfo layouts[31];
};

/* Metal Shader Converter geometry/tessellation emulation, as
 * IRRuntimeNewGeometryEmulationPipeline builds it. */
struct WMTGeometryEmulationInfo {
    obj_handle_t stagein_library;
    obj_handle_t vertex_library;
    obj_handle_t geometry_library;
    obj_handle_t fragment_library;
    obj_handle_t hull_library;
    obj_handle_t domain_library;
    char vertex_function[MADEIRA_FN_LEN];
    char geometry_function[MADEIRA_FN_LEN];
    char fragment_function[MADEIRA_FN_LEN];
    uint32_t gs_vertex_size_bytes;
    uint32_t gs_max_input_primitives;
    float max_tessellation_factor;  /* wine-lab port: float, as max_tessellation_factor_fc is */
    uint32_t tessellation;
};

/* Madeira's grab-bag control call. ops: 0 capture request, 1 capture write,
 * 2 config read (name -> ptr/len), 3 GPU timestamps of a command buffer,
 * 4 thread role, 5 shared perf block, 6 fence-chain mode. */
struct madeira_ctl_args {
    uint32_t op;
    uint32_t ret;
    uint64_t ptr;
    uint64_t len;
    char name[256];
};

struct madeira_ir_convert_args;

#ifdef _WIN32
int madeira_ext_init(void);   /* DllMain: connects d3d12.so; 0 on failure */
obj_handle_t MTLDevice_newPlacementHeap(obj_handle_t device, uint64_t size, uint64_t options);
void MTLDevice_heapBufferSizeAndAlign(obj_handle_t device, uint64_t length, uint64_t options, uint64_t *size, uint64_t *align);
void MTLDevice_heapTextureSizeAndAlign(obj_handle_t device, struct WMTTextureInfo *info, uint64_t *size, uint64_t *align);
obj_handle_t MTLHeap_newBufferAtOffset(obj_handle_t heap, struct WMTBufferInfo *info, uint64_t offset);
obj_handle_t MTLHeap_newTextureAtOffset(obj_handle_t heap, struct WMTTextureInfo *info, uint64_t offset);
obj_handle_t MTLDevice_newResidencySet(obj_handle_t device, uint64_t capacity);
void MTLCommandQueue_addResidencySet(obj_handle_t queue, obj_handle_t set);
void MTLResidencySet_addAllocation(obj_handle_t set, obj_handle_t allocation);
void MTLResidencySet_removeAllocation(obj_handle_t set, obj_handle_t allocation);
void MTLResidencySet_commit(obj_handle_t set);
obj_handle_t MTLDevice_newRenderPipelineStateVD(obj_handle_t device, const struct WMTRenderPipelineInfo *info,
                                                const struct WMTVertexDescriptorInfo *vd, obj_handle_t *err_out);
obj_handle_t MTLDevice_newGeometryEmulationPipelineState(obj_handle_t device, const struct WMTMeshRenderPipelineInfo *info,
                                                         const struct WMTGeometryEmulationInfo *ge, obj_handle_t *err_out);
/* A texture-to-buffer copy; with nonzero MTLBlitOption (depth or stencil plane of a
 * combined format), which DXMT v0.80's command encoding cannot express. */
void madeira_blit_t2b(obj_handle_t encoder, const struct wmtcmd_blit_copy_from_texture_to_buffer *cmd, uint32_t options);
/* wine-lab port: the buffer-to-texture direction. DXMT's winemetal encodes
 * copyFromBuffer:toTexture: WITHOUT options, so a copy into the depth or stencil
 * plane of a combined depth-stencil texture must come here: without
 * MTLBlitOptionStencilFromDepthStencil, a stencil-plane copy lands in the DEPTH
 * plane (TLOU Part I: 1 B/px stencil read as depth -> 0x71717171 = 1.2e30). */
void madeira_blit_b2t(obj_handle_t encoder, const struct wmtcmd_blit_copy_from_buffer_to_texture *cmd, uint32_t options);
/* wine-lab port: a vertex-only pipeline (fullscreen triangle at z = 0, no fragment
 * function) for the given depth and stencil attachment formats: ClearDepthStencilView
 * with rects draws it once per rect under a viewport whose znear = zfar = the clear
 * depth, and a stencil REPLACE. Metal has no partial depth/stencil clear. */
obj_handle_t madeira_new_clear_pipeline(obj_handle_t device, uint32_t depth_pf, uint32_t stencil_pf, uint32_t samples);
void MadeiraCtl(struct madeira_ctl_args *args);
/* A command buffer whose error, on failure, lists each encoder's label and state
 * (MTLCommandBufferErrorOptionEncoderExecutionStatus). Autoreleased, like winemetal's. */
obj_handle_t madeira_cb_with_error_info(obj_handle_t queue);
/* [MTLCommandBuffer enqueue] for n buffers, in array order: reserves their places
 * in the queue's GPU order before any of them is encoded (parallel-encode). */
void madeira_cb_enqueue(const obj_handle_t *cbs, unsigned n);
void MadeiraIRConvert(struct madeira_ir_convert_args *args);
#endif

/* d3d12.so unix call numbers */
enum madeira_unix_func {
    madeira_unix_newPlacementHeap,
    madeira_unix_heapBufferSizeAndAlign,
    madeira_unix_heapTextureSizeAndAlign,
    madeira_unix_heapNewBufferAtOffset,
    madeira_unix_heapNewTextureAtOffset,
    madeira_unix_newResidencySet,
    madeira_unix_queueAddResidencySet,
    madeira_unix_residencySetAddAllocation,
    madeira_unix_residencySetRemoveAllocation,
    madeira_unix_residencySetCommit,
    madeira_unix_newRenderPipelineStateVD,
    madeira_unix_newGeometryEmulationPipelineState,
    madeira_unix_ctl,
    madeira_unix_ir_convert,
    madeira_unix_blitT2B,
    madeira_unix_commandBufferErrorInfo,
    madeira_unix_commandBufferEnqueue,
    madeira_unix_blitB2T,
    madeira_unix_newClearPipeline,
    madeira_unix_count,
};

/* Parameter blocks; pointers travel as uint64_t so PE and unix agree. */
struct madeira_unix_heap_params {
    obj_handle_t owner;     /* device or heap */
    uint64_t arg;           /* size / length / capacity / offset */
    uint64_t options;
    uint64_t info;          /* struct WMTBufferInfo * or struct WMTTextureInfo * */
    uint64_t ret_size, ret_align;
    obj_handle_t ret;
};

struct madeira_unix_pair_params {
    obj_handle_t a, b;
};

struct madeira_unix_pso_params {
    obj_handle_t device;
    uint64_t info;          /* struct WMTRenderPipelineInfo * or struct WMTMeshRenderPipelineInfo * */
    uint64_t extra;         /* struct WMTVertexDescriptorInfo * or struct WMTGeometryEmulationInfo * */
    obj_handle_t ret_pso;
    obj_handle_t ret_error;
};

struct madeira_unix_blit_params {
    obj_handle_t encoder;
    uint64_t cmd;           /* const struct wmtcmd_blit_copy_from_texture_to_buffer * (blitT2B) or
                               const struct wmtcmd_blit_copy_from_buffer_to_texture * (blitB2T) */
    uint64_t options;       /* MTLBlitOption */
};

#endif /* MADEIRA_EXT_H */
