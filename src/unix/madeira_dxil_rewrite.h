/* wine-lab port: the DXIL rewrites run before the Metal Shader Converter
 * (madeira_dxil_rewrite.cpp, LLVM 15). Plain C, so madeira_ir_unix.mm and the
 * host tests share one declaration.
 *
 * One call parses the module once, runs every rewrite the options ask for and
 * writes it back once, so a shader several rewrites apply to pays for one
 * parse. A shader none of them changes is handed back untouched (return 0). */
#ifndef MADEIRA_DXIL_REWRITE_H
#define MADEIRA_DXIL_REWRITE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct madeira_dxil_rewrite_opts {
    int atomic64;        /* 64-bit atomics whose old value is read get it from a load */
    uint32_t loop_cap;   /* loop watchdog: leave every loop after this many passes per entry; 0 = off */
};

struct madeira_dxil_rewrite_info {
    unsigned a64_rewritten;   /* atomics given a non-atomic old-value read */
    unsigned a64_left;        /* 64-bit atomics left as they were: the converter still refuses them */
    int a64_refuse;           /* the converter would crash on the shader: refuse it unconverted */
    char a64_note[192];       /* why some were left, or why the shader is refused */
    unsigned loops_guarded;   /* loops given an iteration cap */
    unsigned loops_skipped;   /* loops without an exiting branch every iteration passes */
};

/* Returns 1 and a malloc'd container when something was rewritten, 0 when
 * nothing was (nothing allocated; info still says what was found), -1 when
 * the bitcode could not be read (a64_note says why). */
int madeira_dxil_rewrite(const void *bc, size_t len, const struct madeira_dxil_rewrite_opts *opts,
                         void **out, size_t *out_len, struct madeira_dxil_rewrite_info *info);

/* Cheap container checks, no bitcode parse. The runtime keys its DXIL cache on
 * these answers, so a rewrite never applies where they say it cannot. */
int madeira_atomic64_candidate(const void *bc, size_t len);
/* The DXIL program's shader kind (0 pixel, 1 vertex, ..., 5 compute; the
 * high half of the DXIL part's program version), or -1. */
int madeira_dxil_shader_kind(const void *bc, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MADEIRA_DXIL_REWRITE_H */
