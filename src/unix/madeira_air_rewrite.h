/* wine-lab port: rewrites of the Metal Shader Converter's OUTPUT (AIR in a
 * metallib), madeira_air_rewrite.cpp, LLVM 15. Plain C for madeira_ir_unix.mm.
 *
 * RGB32 typed buffers. Metal has no 3-channel 32-bit texture-buffer format, so
 * the runtime binds a typed SRV of R32G32B32_{FLOAT,UINT,SINT,TYPELESS} as an
 * R32 texture buffer of 3 x NumElements over the same memory and sets
 * MADEIRA_DESC_RGB32 in the descriptor's metadata word (bits MSC never reads:
 * it uses 0-31 size, 32-39 texture-view offset, 63 typed). Every typed-buffer
 * read in the converted shader then tests that bit in the descriptor it read
 * the texture from: set, it loads x, y, z from the descriptor's gpuVA at
 * element * 12 and returns (x, y, z, 1); clear, it reads the texture exactly as
 * before. GetDimensions returns size / 12 for such a view. Genuine typed views
 * never have the bit, so they cannot be confused with it. */
#ifndef MADEIRA_AIR_REWRITE_H
#define MADEIRA_AIR_REWRITE_H

#include <stddef.h>
#include <stdint.h>

#define MADEIRA_DESC_RGB32 (1ull << 62)

#ifdef __cplusplus
extern "C" {
#endif

struct madeira_air_rgb32_info {
    unsigned reads;       /* typed-buffer reads given the RGB32 path */
    unsigned widths;      /* GetDimensions given it */
    unsigned unmatched;   /* typed-buffer reads or widths whose descriptor could not be found: they read
                             an RGB32 view as R32 data */
    char note[160];
};

/* Returns 1 and a malloc'd metallib when something was rewritten, 0 when the
 * library has no typed-buffer access (nothing allocated), -1 when it has some
 * but could not be rewritten (note says why; unmatched counts it). */
int madeira_air_rgb32_rewrite(const void *lib, size_t len, void **out, size_t *out_len,
                              struct madeira_air_rgb32_info *info);

#ifdef __cplusplus
}
#endif

#endif /* MADEIRA_AIR_REWRITE_H */
