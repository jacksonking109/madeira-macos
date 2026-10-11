/* wine-lab port: host test for src/unix/madeira_dxil_rewrite.cpp's rewrite that
 * lets the Metal Shader Converter take 64-bit atomics whose old value is read.
 *
 * Converts each shader through the converter twice, the way famtest does (no
 * root signature; GPU families Metal3, Apple8 and Apple9): as given,
 * which must fail with IRErrorCode 7 (UnsupportedInstruction), and after
 * madeira_dxil_rewrite, which must convert. The shaders are the ps_6_6
 * arrays of tests/windows/atomic64_dxil.h plus any containers named on the
 * command line -- the three TLOU Part I k-buffer shaders the runtime saved.
 * ps_cmpxchg (a texture compare-exchange whose old value is used) crashes the
 * converter, so it is never converted here: the rewrite must refuse it.
 *
 *   cd ~/wine-lab/d3d12-port; LL=~/wine-lab/deps/llvm15-x86_64
 *   clang++ -arch x86_64 -O1 -std=c++17 -fno-rtti -I$LL/include -Ithird_party/metal-shader-converter/include \
 *       -Isrc/unix -Itests/windows tests/native/atomic64_rewrite_test.cpp src/unix/madeira_dxil_rewrite.cpp \
 *       $($LL/bin/llvm-config --libfiles bitreader bitwriter core) -lz -lcurses \
 *       -L ~/wine-lab/wine11/lib/wine/x86_64-unix -lmetalirconverter \
 *       -Wl,-rpath,$HOME/wine-lab/wine11/lib/wine/x86_64-unix -o out/atomic64_rewrite_test
 *   out/atomic64_rewrite_test ~/wine-lab/d3d12-config/failed-shaders-tlou1-2026-10-09/shader_0{2,3,4}_code7.bin
 *
 * Exit code = number of failed checks. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <metal_irconverter/metal_irconverter.h>
#include "atomic64_dxil.h"
#include "madeira_dxil_rewrite.h"

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL  "); } else printf("  ok    "); \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

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

/* expect_n: atomics the rewrite must fix (0: a shader the converter crashes
 * on, which the rewrite must refuse without it being converted). */
static void run(const char *name, const void *b, size_t n, unsigned expect_n)
{
    void *nb = nullptr;
    size_t nl = 0;
    madeira_dxil_rewrite_opts o = {};
    madeira_dxil_rewrite_info inf;
    o.atomic64 = 1;
    CHECK(madeira_atomic64_candidate(b, n), "%s: SFI0 makes it a candidate", name);
    int rc = madeira_dxil_rewrite(b, n, &o, &nb, &nl, &inf);
    unsigned got_n = inf.a64_rewritten;
    if (inf.a64_note[0]) printf("        note: %s\n", inf.a64_note);
    if (!expect_n) {
        CHECK(rc == 0 && !nb && inf.a64_refuse && strstr(inf.a64_note, "compare-exchange"),
              "%s: refused unconverted, the note names it (rc %d)", name, rc);
        return;
    }
    unsigned before = convert(b, n);
    CHECK(before == IRErrorCodeUnsupportedInstruction, "%s: refused by the converter as given (code %u)", name, before);
    CHECK(rc == 1 && nb && got_n == expect_n, "%s: %u of %u atomic(s) rewritten (rc %d, %zu -> %zu bytes)",
          name, got_n, expect_n, rc, n, nl);
    if (rc == 1 && nb) {
        unsigned after = convert(nb, nl);
        CHECK(after == 0, "%s: converts after the rewrite (code %u)", name, after);
        void *nb2 = nullptr;
        size_t nl2 = 0;
        CHECK(madeira_dxil_rewrite(nb, nl, &o, &nb2, &nl2, &inf) == 0 && !nb2 && !inf.a64_left,
              "%s: nothing left to rewrite in the result", name);
        free(nb2);
    }
    free(nb);
}

int main(int argc, char **argv)
{
    setbuf(stdout, nullptr);   /* the converter can abort in teardown */
    printf("atomic64 rewrite: converter %d.%d.%d\n", IR_VERSION_MAJOR, IR_VERSION_MINOR, IR_VERSION_PATCH);
#define EMB(x, n) run(#x, x, sizeof x, n)
    EMB(a64_ps_tex_dxil, 1);
    EMB(a64_ps_raw_dxil, 1);
    EMB(a64_ps_struct_dxil, 1);
    EMB(a64_ps_typed_dxil, 1);
    EMB(a64_ps_tex3d_dxil, 1);
    EMB(a64_ps_tex2darray_dxil, 1);
    EMB(a64_ps_tex1darray_dxil, 1);
    EMB(a64_ps_cmpxchg_dxil, 0);
#undef EMB
    CHECK(!madeira_atomic64_candidate(a64_vs_dxil, sizeof a64_vs_dxil), "a64_vs_dxil: no 64-bit feature bits, not a candidate");
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
        run(base ? base + 1 : argv[i], b, got, 3);   /* TLOU k-buffer insert: 4 layers, the last drops its old value */
        free(b);
    }
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails;
}
