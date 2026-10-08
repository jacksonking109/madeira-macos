/* wine-lab port: host test for d3d12.so's geometry/tessellation emulation pipeline
 * (new_geometry_emulation_pipeline_state in src/unix/d3d12_unix.m).
 *
 * The vertex, stage-in and pixel stages are REAL converter output: the repo's
 * cube_vs.dxil converted with --enable-gs-ts-emulation --vertex-stage-in (the
 * object-shader variant "MainVS.dxil_irconverter_object_shader" plus
 * irconverter_stage_in_shader) and cube_ps.dxil. There is no DXIL geometry,
 * hull or domain shader in this tree and nothing here compiles HLSL to DXIL,
 * so those are MSL stand-ins carrying the converter's names and function
 * constants (the mesh function with vertex_shader_output_size_fc,
 * irconverter_hull_shader / irconverter_tessellator with
 * max_tessellation_factor_fc, irconverter_dxil_domain_shader and the triangle
 * passthrough). The test calls the unix entry point through d3d12.so's
 * __wine_unix_call_funcs exactly as the PE side does and checks that Metal
 * builds the object/mesh pipeline, and that missing pieces come back as an
 * NSError instead of a crash. It does not draw: the draw contract
 * (argument buffers, payload layout) belongs to madeira_d3d12.c.
 *
 * Build and run (from the repo root, after building out/d3d12.so):
 *   O=out/geomtest; mkdir -p $O; echo '{"InputElements":[],"SemanticNames":[]}' > $O/empty.json
 *   metal-shaderconverter --enable-gs-ts-emulation --vertex-stage-in --vertex-input-layout-file=$O/empty.json \
 *       shaders/cube_vs.dxil -o=$O/vs.metallib          # also writes $O/vs.stageIn.metallib
 *   metal-shaderconverter shaders/cube_ps.dxil -o=$O/ps.metallib
 *   clang -arch x86_64 -fobjc-arc -Isrc -I$HOME/wine-lab/dxmt-src/src/winemetal \
 *       tests/native/geom_emu_test.m -o $O/geom_emu_test -framework Foundation -framework Metal
 *   $O/geom_emu_test out/d3d12.so $O/vs.metallib $O/vs.stageIn.metallib $O/ps.metallib
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *args);
#include "madeira_ext.h"

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)


/* Stand-ins for the stages with no DXIL in this tree. V matches cube_ps's
 * inputs (the converter names them after the HLSL semantics). */
static NSString *const kCommon =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 pos [[position]]; float4 col [[user(color0)]]; float3 nrm [[user(normal0)]]; };\n"
     "struct Payload { uint data[16]; };\n"
     "using M = metal::mesh<V, void, 3, 1, topology::triangle>;\n";

static NSString *const kGeometry =
    @"constant int vertex_shader_output_size_fc [[function_constant(0)]];\n"
     "[[mesh]] void gs_main(M m, const object_data Payload &p [[payload]], uint tid [[thread_index_in_threadgroup]]) {\n"
     "  if (tid < 3) { V v; v.pos = float4(p.data[tid], 0, vertex_shader_output_size_fc, 1); v.col = 1; v.nrm = 0; m.set_vertex(tid, v); m.set_index(tid, tid); }\n"
     "  if (tid == 0) m.set_primitive_count(1);\n"
     "}\n";

static NSString *const kStageInStandIn =
    @"[[visible]] float4 irconverter_stage_in_shader(uint vid) { return float4(vid); }\n";

static NSString *const kHull =
    @"constant int vertex_shader_output_size_fc [[function_constant(0)]];\n"
     "constant float max_tessellation_factor_fc [[function_constant(1)]];\n"
     "[[visible]] float irconverter_hull_shader(float x) { return x * max_tessellation_factor_fc + vertex_shader_output_size_fc; }\n"
     "[[visible]] float irconverter_tessellator(float x) { return min(x, max_tessellation_factor_fc); }\n";

static NSString *const kDomain =
    @"[[visible]] float irconverter_dxil_domain_shader(float x) { return x * 2; }\n"
     "[[mesh]] void irconverter_domain_shader_triangle_passthrough(M m, const object_data Payload &p [[payload]], uint tid [[thread_index_in_threadgroup]]) {\n"
     "  if (tid < 3) { V v; v.pos = float4(p.data[tid], 0, 0, 1); v.col = 1; v.nrm = 0; m.set_vertex(tid, v); m.set_index(tid, tid); }\n"
     "  if (tid == 0) m.set_primitive_count(1);\n"
     "}\n";

static id<MTLLibrary> src_lib(id<MTLDevice> dev, NSString *src)
{
    NSError *e = nil;
    id<MTLLibrary> l = [dev newLibraryWithSource:[kCommon stringByAppendingString:src] options:nil error:&e];
    if (!l) printf("library: %s\n", e.localizedDescription.UTF8String);
    return l;
}

static id<MTLLibrary> file_lib(id<MTLDevice> dev, const char *path)
{
    NSError *e = nil;
    id<MTLLibrary> l = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&e];
    if (!l) printf("%s: %s\n", path, e.localizedDescription.UTF8String);
    return l;
}

static unixlib_entry_t g_call;

static id<MTLRenderPipelineState> build(id<MTLDevice> dev, struct WMTGeometryEmulationInfo *ge, NSError **err_out)
{
    struct WMTMeshRenderPipelineInfo mp;
    struct madeira_unix_pso_params p;
    memset(&mp, 0, sizeof mp);
    mp.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
    mp.colors[0].write_mask = 0xf;
    mp.rasterization_enabled = true;
    mp.raster_sample_count = 1;
    mp.depth_pixel_format = WMTPixelFormatInvalid;
    mp.stencil_pixel_format = WMTPixelFormatInvalid;
    memset(&p, 0, sizeof p);
    p.device = (obj_handle_t)(__bridge void *)dev;
    p.info = (uint64_t)(uintptr_t)&mp;
    p.extra = (uint64_t)(uintptr_t)ge;
    g_call(&p);
    *err_out = p.ret_error ? (__bridge_transfer NSError *)(void *)p.ret_error : nil;
    return p.ret_pso ? (__bridge_transfer id<MTLRenderPipelineState>)(void *)p.ret_pso : nil;
}

#define H(x) ((obj_handle_t)(__bridge void *)(x))

static void names(struct WMTGeometryEmulationInfo *ge, const char *gs, const char *ps)
{
    snprintf(ge->vertex_function, sizeof ge->vertex_function, "MainVS");
    snprintf(ge->geometry_function, sizeof ge->geometry_function, "%s", gs);
    snprintf(ge->fragment_function, sizeof ge->fragment_function, "%s", ps);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        void *h;
        const unixlib_entry_t *funcs;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLLibrary> Lv, Ls, Lf, Lg, Lh, Ld, Lsx;
        struct WMTGeometryEmulationInfo ge;
        id<MTLRenderPipelineState> pso;
        NSError *err = nil;

        if (argc < 5) { printf("usage: %s d3d12.so vs.metallib vs.stageIn.metallib ps.metallib\n", argv[0]); return 2; }
        if (!(h = dlopen(argv[1], RTLD_NOW))) { printf("dlopen %s: %s\n", argv[1], dlerror()); return 2; }
        funcs = dlsym(h, "__wine_unix_call_funcs");
        if (!funcs || !dev) { printf("no unix call table or no device\n"); return 2; }
        g_call = funcs[madeira_unix_newGeometryEmulationPipelineState];
        printf("device: %s\n", dev.name.UTF8String);

        Lv = file_lib(dev, argv[2]); Ls = file_lib(dev, argv[3]); Lf = file_lib(dev, argv[4]);
        Lg = src_lib(dev, kGeometry); Lh = src_lib(dev, kHull); Ld = src_lib(dev, kDomain); Lsx = src_lib(dev, kStageInStandIn);
        if (!Lv || !Ls || !Lf || !Lg || !Lh || !Ld || !Lsx) return 2;

        /* 1. geometry-shader pipeline as madeira_d3d12.c passes it (gs_emu == 1) */
        memset(&ge, 0, sizeof ge);
        ge.stagein_library = H(Ls); ge.vertex_library = H(Lv); ge.geometry_library = H(Lg); ge.fragment_library = H(Lf);
        names(&ge, "gs_main", "MainPS");
        ge.gs_vertex_size_bytes = 48; ge.gs_max_input_primitives = 1;
        pso = build(dev, &ge, &err);
        CHECK(pso && !err, "geometry pipeline: %s", err.localizedDescription.UTF8String);

        /* 2. no stage-in library (madeira_d3d12.c allows that without an input layout) */
        ge.stagein_library = 0;
        pso = build(dev, &ge, &err);
        printf("  (geometry pipeline without a stage-in library: %s)\n", pso ? "built" : err.localizedDescription.UTF8String);
        CHECK((pso != nil) != (err != nil), "no stage-in: pso %d err %d", !!pso, !!err);

        /* 3. no pixel shader */
        ge.stagein_library = H(Ls);
        names(&ge, "gs_main", "");
        pso = build(dev, &ge, &err);
        CHECK(pso && !err, "geometry pipeline without fragment: %s", err.localizedDescription.UTF8String);

        /* 4. failures come back as an NSError, never a nil mesh function handed to Metal */
        names(&ge, "no_such_gs", "MainPS");
        pso = build(dev, &ge, &err);
        CHECK(!pso && err, "missing geometry function was not refused");
        if (err) printf("  (refusal: %s)\n", err.localizedDescription.UTF8String);
        names(&ge, "gs_main", "no_such_ps");
        pso = build(dev, &ge, &err);
        CHECK(!pso && err, "missing fragment function was not refused");
        ge.geometry_library = 0; names(&ge, "gs_main", "MainPS");
        pso = build(dev, &ge, &err);
        CHECK(!pso && err, "missing geometry library was not refused");
        ge.geometry_library = H(Lg); ge.vertex_library = H(Lf);   /* a library without the object variant */
        pso = build(dev, &ge, &err);
        CHECK(!pso && err, "missing object function was not refused");
        if (err) printf("  (refusal: %s)\n", err.localizedDescription.UTF8String);

        /* 5. DXIL tessellation as madeira_d3d12.c passes it (gs_emu == 2): the
         * geometry library is the domain library, the mesh function its
         * passthrough. The hull/domain stand-ins do not have the converter's
         * signatures, so Metal may refuse the link; the descriptor up to there
         * is what this checks, and that a refusal is an NSError. */
        memset(&ge, 0, sizeof ge);
        ge.stagein_library = H(Ls); ge.vertex_library = H(Lv); ge.fragment_library = H(Lf);
        ge.geometry_library = H(Ld); ge.domain_library = H(Ld); ge.hull_library = H(Lh);
        names(&ge, "irconverter_domain_shader_triangle_passthrough", "MainPS");
        ge.gs_vertex_size_bytes = 48; ge.gs_max_input_primitives = 8; ge.max_tessellation_factor = 15.5f; ge.tessellation = 1;
        pso = build(dev, &ge, &err);
        printf("  (tessellation pipeline: %s)\n", pso ? "built" : err.localizedDescription.UTF8String);
        CHECK((pso != nil) != (err != nil), "tessellation: pso %d err %d", !!pso, !!err);

        /* 6. tessellation followed by a real geometry stage */
        ge.geometry_library = H(Lg); names(&ge, "gs_main", "MainPS");
        pso = build(dev, &ge, &err);
        printf("  (tessellation + geometry pipeline: %s)\n", pso ? "built" : err.localizedDescription.UTF8String);
        CHECK((pso != nil) != (err != nil), "tessellation + geometry: pso %d err %d", !!pso, !!err);

        /* 7. tessellation pieces missing */
        ge.geometry_library = H(Ld); names(&ge, "irconverter_domain_shader_triangle_passthrough", "MainPS");
        ge.hull_library = 0;
        pso = build(dev, &ge, &err);
        CHECK(!pso && err, "tessellation without a hull library was not refused");
        ge.hull_library = H(Lh); ge.domain_library = H(Lg);
        pso = build(dev, &ge, &err);
        CHECK(!pso && err, "tessellation with a domain library lacking irconverter_dxil_domain_shader was not refused");
        ge.domain_library = H(Ld); ge.hull_library = H(Ld);
        pso = build(dev, &ge, &err);
        CHECK(!pso && err, "tessellation with a hull library lacking the hull functions was not refused");
    }
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
