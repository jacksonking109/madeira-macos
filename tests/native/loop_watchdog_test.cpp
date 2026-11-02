/* wine-lab port: host test for the loop watchdog in src/unix/madeira_dxil_rewrite.cpp.
 *
 * Conversion only -- nothing here runs on the GPU. Each compute shader goes
 * through the rewrite with the default cap and then through the converter (no
 * root signature; GPU families Metal3, Apple8 and Apple9), which must take it
 * as it took the original. The shaders are tests/windows/loopwd_dxil.h and any
 * containers named on the command line, e.g. the TLOU Part I kernels the
 * runtime captured (CS_ComputeVolProbeBufferWriteCache walks a BVH with a
 * local stack and, fed bad node data, never left its loop):
 *
 *   cd ~/wine-lab/d3d12-port; LL=~/wine-lab/deps/llvm15-x86_64
 *   clang++ -arch x86_64 -O1 -std=c++17 -fno-rtti -I$LL/include -Ithird_party/metal-shader-converter/include \
 *       -Isrc/unix -Itests/windows tests/native/loop_watchdog_test.cpp src/unix/madeira_dxil_rewrite.cpp \
 *       $($LL/bin/llvm-config --libfiles bitreader bitwriter core analysis) -lz -lcurses \
 *       -L ~/wine-lab/wine11/lib/wine/x86_64-unix -lmetalirconverter \
 *       -Wl,-rpath,$HOME/wine-lab/wine11/lib/wine/x86_64-unix -o out/loop_watchdog_test
 *   out/loop_watchdog_test ~/wine-lab/prefix-steam/drive_c/madeira-cs/CS_CS_ComputeVolProbeBuffer*.dxil
 *
 * Also prints what the rewrite costs per shader (parse, guard, write).
 * Exit code = number of failed checks. */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <metal_irconverter/metal_irconverter.h>
#include "loopwd_dxil.h"
#include "atomic64_dxil.h"
#include "madeira_dxil_rewrite.h"

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL  "); } else printf("  ok    "); \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static const uint32_t kCap = 1u << 20;

/* 0 when the converter takes it for every family, else the first error code. */
static unsigned convert(const void *b, size_t n)
{
    static const IRGPUFamily fams[] = { IRGPUFamilyMetal3, IRGPUFamilyApple8, IRGPUFamilyApple9 };
    for (IRGPUFamily f : fams) {
        IRCompiler *c = IRCompilerCreate();
        IRCompilerSetMinimumGPUFamily(c, f);
        IRCompilerSetCompatibilityFlags(c, IRCompatibilityFlagForceTextureArray);
        IRObject *o = IRObjectCreateFromDXIL((const uint8_t *)b, n, IRBytecodeOwnershipNone);
        IRError *e = nullptr;
        IRObject *out = o ? IRCompilerAllocCompileAndLink(c, nullptr, o, &e) : nullptr;
        unsigned code = out ? 0 : e ? IRErrorGetCode(e) : ~0u;
        if (e) IRErrorDestroy(e);
        if (out) IRObjectDestroy(out);
        if (o) IRObjectDestroy(o);
        IRCompilerDestroy(c);
        if (code) return code;
    }
    return 0;
}

/* min_clamped / min_guarded: what the rewrite must have done with the loops (a zero minimum asserts nothing). */
static void run(const char *name, const void *b, size_t n, unsigned min_clamped = 0, unsigned min_guarded = 0)
{
    madeira_dxil_rewrite_opts o = {};
    madeira_dxil_rewrite_info inf;
    void *nb = nullptr;
    size_t nl = 0;
    o.loop_cap = kCap;
    o.loop_clamp = 1;
    CHECK(madeira_dxil_shader_kind(b, n) == 5, "%s: a compute shader", name);
    auto t0 = std::chrono::steady_clock::now();
    int rc = madeira_dxil_rewrite(b, n, &o, &nb, &nl, &inf);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(rc == 1 && nb && inf.loops_guarded + inf.loops_clamped && !inf.loops_skipped,
          "%s: %u loop(s) clamped, %u counter-guarded, %u skipped (rc %d, %zu -> %zu bytes, %lld us)",
          name, inf.loops_clamped, inf.loops_guarded, inf.loops_skipped, rc, n, nl, (long long)us);
    if (min_clamped || min_guarded)
        CHECK(inf.loops_clamped >= min_clamped && inf.loops_guarded >= min_guarded,
              "%s: at least %u clamped and %u counter-guarded", name, min_clamped, min_guarded);
    unsigned before = convert(b, n);
    CHECK(before == 0, "%s: the original converts (code %u)", name, before);
    if (rc == 1 && nb) {
        unsigned after = convert(nb, nl);
        CHECK(after == 0, "%s: converts after the rewrite (code %u)", name, after);
    }
    free(nb);
}

int main(int argc, char **argv)
{
    setbuf(stdout, nullptr);   /* the converter can abort in teardown */
    printf("loop watchdog: converter %d.%d.%d, cap %u\n", IR_VERSION_MAJOR, IR_VERSION_MINOR, IR_VERSION_PATCH, kCap);
    /* loopwd.hlsl: two canonical counted loops (zero start, and start 4) are
     * clamped; the step-2 loop and the data-dependent walk keep the counter. */
    run("lw_cs_dxil", lw_cs_dxil, sizeof lw_cs_dxil, 2, 2);
    {   /* no loops: handed back untouched */
        madeira_dxil_rewrite_opts o = {};
        madeira_dxil_rewrite_info inf;
        void *nb = nullptr;
        size_t nl = 0;
        o.loop_cap = kCap;
        o.loop_clamp = 1;
        int rc = madeira_dxil_rewrite(a64_vs_dxil, sizeof a64_vs_dxil, &o, &nb, &nl, &inf);
        CHECK(rc == 0 && !nb && !inf.loops_guarded, "a64_vs_dxil: no loops, nothing rewritten (rc %d)", rc);
    }
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { CHECK(0, "%s: readable", argv[i]); continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        rewind(f);
        void *b = malloc((size_t)n);
        size_t got = fread(b, 1, (size_t)n, f);
        fclose(f);
        const char *base = strrchr(argv[i], '/');
        run(base ? base + 1 : argv[i], b, got);
        free(b);
    }
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails;
}
