/* wine-lab port: DXIL rewrites before the Metal Shader Converter
 * (madeira_dxil_rewrite.h): the module is parsed once, each rewrite the
 * options ask for runs on it, and it is written back once.
 *
 * ---- 64-bit atomics whose old value is read ----
 *
 * Metal's 64-bit atomics (atomic_ulong min/max on buffers and textures) return
 * nothing, so the Metal Shader Converter refuses any dx.op.atomicBinOp.i64
 * whose result has a use -- IRErrorCode 7, UnsupportedInstruction -- on
 * textures, raw and structured buffers alike, and accepts the same atomic when
 * the result is unused. The Last of Us Part I halts on that: its
 * order-independent-transparency k-buffer insert does
 *   old = InterlockedMax(kbuf[px], packed)       (RWTexture2D<uint64_t>)
 * and carries `old` on to the next layer.
 *
 * This pass rewrites the DXIL module (LLVM 15 reads DXIL bitcode, and its
 * writer's output converts, as madeira_ags.cpp found) so that each such atomic
 *   %r = call i64 @dx.op.atomicBinOp.i64(78, h, op, c0, c1, c2, v)
 * becomes a plain load of the same element followed by the atomic with its
 * result unused:
 *   textures (1D, 1D array, 2D, 2D array, 3D)  textureLoad.i32(66, h, undef, c0, c1, c2, undef x3)
 *   typed buffers                              bufferLoad.i32(68, h, c0, undef)
 *       -> %r = zext(hi) << 32 | zext(lo), from components 0 (lo) and 1 (hi)
 *   raw buffers                                rawBufferLoad.i64(139, h, c0, undef, mask 1, align 8)
 *   structured buffers                         rawBufferLoad.i64(139, h, c0, c1, mask 1, align 8)
 * (the load shapes DXC emits for a uint64_t read of each kind). The atomic
 * still happens; only the value handed back is no longer atomic with it: an
 * invocation that updates the element between the load and the atomic is
 * missed, which for TLOU's k-buffer is a rare wrong layer order, not a hang.
 *
 * The resource kind comes from the handle: dx.op.annotateHandle's
 * ResourceProperties (shader model 6.6, after createHandleFromBinding or
 * createHandleFromHeap), or dx.op.createHandle's range ID looked up in
 * dx.resources (6.0-6.5, which is what madeira_ags.cpp produces), through phis
 * and selects whose inputs all agree. Anything else -- another kind, a handle
 * that cannot be traced -- is left alone and named in the note; the converter
 * then refuses the shader as before.
 *
 * 64-bit compare-exchange (opcode 79) is not rewritten: a load cannot stand in
 * for whether the exchange happened. On raw and structured buffers the
 * converter takes it, old value and all. On a typed resource (texture or typed
 * buffer) with the old value used, MSC 4.0b2 CRASHES (a null dereference in
 * metallib::LazyLinker::CanLinkSymbols, every GPU family and flag set tried,
 * on tests/windows/atomic64.hlsl ps_cmpxchg) -- a fault inside the game's
 * process. Such a shader is reported so the runtime refuses it unconverted. */
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include "madeira_dxil_rewrite.h"

using namespace llvm;

namespace {

constexpr unsigned kOpCreateHandle = 57, kOpTextureLoad = 66, kOpBufferLoad = 68, kOpAtomicBinOp = 78,
                   kOpAtomicCmpXchg = 79, kOpBarrier = 80, kOpRawBufferLoad = 139, kOpAnnotateHandle = 216;
constexpr unsigned kBarrierUAVFenceGlobal = 2;   /* DeviceMemoryBarrier(), the one barrier any stage may use */
constexpr unsigned kClassUAV = 1;
/* DXIL::ResourceKind */
enum { kTex1D = 1, kTex2D = 2, kTex3D = 4, kTex1DArray = 6, kTex2DArray = 7,
       kTypedBuffer = 10, kRawBuffer = 11, kStructuredBuffer = 12 };
/* SFI0 feature bits that any 64-bit atomic implies */
constexpr uint64_t kSfi0Int64Ops = 0x8000, kSfi0AtomicInt64Typed = 0x400000,
                   kSfi0AtomicInt64Shared = 0x800000, kSfi0AtomicInt64Heap = 0x10000000;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static bool is_dx_op(const CallInst *ci, unsigned op)
{
    const Function *f = ci->getCalledFunction();
    if (!f || !f->getName().startswith("dx.op.") || ci->arg_size() < 1) return false;
    auto *c = dyn_cast<ConstantInt>(ci->getArgOperand(0));
    return c && c->getZExtValue() == op;
}

static const char *kind_name(int k)
{
    switch (k) {
    case kTex1D: return "Texture1D"; case kTex2D: return "Texture2D"; case kTex3D: return "Texture3D";
    case kTex1DArray: return "Texture1DArray"; case kTex2DArray: return "Texture2DArray";
    case kTypedBuffer: return "typed buffer"; case kRawBuffer: return "raw buffer";
    case kStructuredBuffer: return "structured buffer";
    case 3: return "Texture2DMS"; case 8: return "Texture2DMSArray";
    default: return "resource";
    }
}

/* dx.resources = !{srvs, uavs, cbvs, samplers}; a UAV entry's operand 0 is
 * its range ID (createHandle's rangeId), operand 6 its ResourceKind. */
static int uav_kind_by_range(Module &m, uint64_t rid)
{
    NamedMDNode *res = m.getNamedMetadata("dx.resources");
    if (!res || res->getNumOperands() != 1) return -1;
    auto *top = dyn_cast<MDTuple>(res->getOperand(0));
    if (!top || top->getNumOperands() < 2) return -1;
    auto *uavs = dyn_cast_or_null<MDTuple>(top->getOperand(1).get());
    if (!uavs) return -1;
    for (const MDOperand &op : uavs->operands()) {
        auto *e = dyn_cast_or_null<MDTuple>(op.get());
        if (!e || e->getNumOperands() < 7) continue;
        auto *id = dyn_cast_or_null<ConstantAsMetadata>(e->getOperand(0).get());
        auto *kind = dyn_cast_or_null<ConstantAsMetadata>(e->getOperand(6).get());
        auto *idc = id ? dyn_cast<ConstantInt>(id->getValue()) : nullptr;
        auto *kc = kind ? dyn_cast<ConstantInt>(kind->getValue()) : nullptr;
        if (idc && kc && idc->getZExtValue() == rid) return (int)kc->getZExtValue();
    }
    return -1;
}

/* The UAV ResourceKind behind a handle, or -1. */
static int handle_kind(Module &m, Value *h, unsigned depth = 0)
{
    if (depth > 16) return -1;
    if (auto *ci = dyn_cast<CallInst>(h)) {
        if (is_dx_op(ci, kOpAnnotateHandle) && ci->arg_size() >= 3) {
            /* ResourceProperties { u32 basic, u32 extra }: basic bits 0-7
             * ResourceKind, bit 12 IsUAV. */
            auto *props = dyn_cast<Constant>(ci->getArgOperand(2));
            auto *w0 = props ? dyn_cast_or_null<ConstantInt>(props->getAggregateElement(0u)) : nullptr;
            if (!w0) return -1;
            uint64_t basic = w0->getZExtValue();
            return (basic >> 12) & 1 ? (int)(basic & 0xff) : -1;
        }
        if (is_dx_op(ci, kOpCreateHandle) && ci->arg_size() >= 3) {
            auto *cls = dyn_cast<ConstantInt>(ci->getArgOperand(1));
            auto *rid = dyn_cast<ConstantInt>(ci->getArgOperand(2));
            if (!cls || !rid || cls->getZExtValue() != kClassUAV) return -1;
            return uav_kind_by_range(m, rid->getZExtValue());
        }
        return -1;
    }
    auto agree = [&](int acc, Value *v) {
        int k = handle_kind(m, v, depth + 1);
        return acc == -2 ? k : (acc == k ? acc : -1);
    };
    if (auto *phi = dyn_cast<PHINode>(h)) {
        int k = -2;
        for (Value *v : phi->incoming_values()) if ((k = agree(k, v)) == -1) break;
        return k == -2 ? -1 : k;
    }
    if (auto *sel = dyn_cast<SelectInst>(h)) {
        int k = agree(agree(-2, sel->getTrueValue()), sel->getFalseValue());
        return k == -2 ? -1 : k;
    }
    return -1;
}

/* A dx.op declaration by name; null when the module already has one with
 * another type (nothing to rewrite against then). */
static Function *dx_decl(Module &m, const char *name, FunctionType *ft)
{
    if (Function *f = m.getFunction(name)) return f->getFunctionType() == ft ? f : nullptr;
    Function *f = Function::Create(ft, GlobalValue::ExternalLinkage, name, m);
    f->addFnAttr(Attribute::NoUnwind);
    f->addFnAttr(Attribute::ReadOnly);   /* as DXC declares its loads */
    return f;
}

/* dx.op.barrier, declared as DXC does (nounwind noduplicate). */
static Function *dx_fence_decl(Module &m)
{
    LLVMContext &ctx = m.getContext();
    FunctionType *ft = FunctionType::get(Type::getVoidTy(ctx), {Type::getInt32Ty(ctx), Type::getInt32Ty(ctx)}, false);
    if (Function *f = m.getFunction("dx.op.barrier")) return f->getFunctionType() == ft ? f : nullptr;
    Function *f = Function::Create(ft, GlobalValue::ExternalLinkage, "dx.op.barrier", m);
    f->addFnAttr(Attribute::NoUnwind);
    f->addFnAttr(Attribute::NoDuplicate);
    return f;
}

static StructType *named_struct(LLVMContext &ctx, const char *name, ArrayRef<Type *> el)
{
    StructType *t = StructType::getTypeByName(ctx, name);
    return t ? t : StructType::create(ctx, el, name);
}

struct counts_t { unsigned rewritten = 0, typed_cmpxchg = 0, other = 0; std::string other_why; };

static bool is_typed(int kind) { return kind >= kTex1D && kind <= kTypedBuffer; }

static void rewrite_module(Module &m, counts_t &n)
{
    LLVMContext &ctx = m.getContext();
    std::vector<CallInst *> work;
    for (Function &f : m) {
        if (f.isDeclaration()) continue;
        for (BasicBlock &bb : f) for (Instruction &in : bb) {
            auto *ci = dyn_cast<CallInst>(&in);
            if (!ci || ci->use_empty() || !ci->getType()->isIntegerTy(64)) continue;
            if (is_dx_op(ci, kOpAtomicBinOp) && ci->arg_size() == 7) work.push_back(ci);
            else if (is_dx_op(ci, kOpAtomicCmpXchg) && ci->arg_size() >= 2) {
                int kind = handle_kind(m, ci->getArgOperand(1));
                if (kind < 0 || is_typed(kind)) n.typed_cmpxchg++;   /* untraceable: assume the worst */
            }
        }
    }
    if (work.empty() || n.typed_cmpxchg) return;

    Type *i8 = Type::getInt8Ty(ctx), *i32 = Type::getInt32Ty(ctx), *i64 = Type::getInt64Ty(ctx);
    Type *hty = work[0]->getArgOperand(1)->getType();
    StructType *ret32 = named_struct(ctx, "dx.types.ResRet.i32", {i32, i32, i32, i32, i32});
    StructType *ret64 = named_struct(ctx, "dx.types.ResRet.i64", {i64, i64, i64, i64, i32});
    Function *tex_ld = nullptr, *buf_ld = nullptr, *raw_ld = nullptr, *fence = nullptr;
    Value *u32 = UndefValue::get(i32);

    for (CallInst *ci : work) {
        Value *h = ci->getArgOperand(1);
        Value *c0 = ci->getArgOperand(3), *c1 = ci->getArgOperand(4), *c2 = ci->getArgOperand(5);
        int kind = handle_kind(m, h);
        IRBuilder<> b(ci);
        Value *old = nullptr, *ld = nullptr;
        switch (kind) {
        case kTex1D: case kTex2D: case kTex3D: case kTex1DArray: case kTex2DArray:
            if (!tex_ld) tex_ld = dx_decl(m, "dx.op.textureLoad.i32",
                FunctionType::get(ret32, {i32, hty, i32, i32, i32, i32, i32, i32, i32}, false));
            if (tex_ld) ld = b.CreateCall(tex_ld, {ConstantInt::get(i32, kOpTextureLoad), h, u32, c0, c1, c2, u32, u32, u32});
            break;
        case kTypedBuffer:
            if (!buf_ld) buf_ld = dx_decl(m, "dx.op.bufferLoad.i32", FunctionType::get(ret32, {i32, hty, i32, i32}, false));
            if (buf_ld) ld = b.CreateCall(buf_ld, {ConstantInt::get(i32, kOpBufferLoad), h, c0, u32});
            break;
        case kRawBuffer: case kStructuredBuffer:
            /* The atomic needs an 8-byte aligned element, so the load may say so. */
            if (!raw_ld) raw_ld = dx_decl(m, "dx.op.rawBufferLoad.i64",
                FunctionType::get(ret64, {i32, hty, i32, i32, i8, i32}, false));
            if (raw_ld) old = b.CreateExtractValue(b.CreateCall(raw_ld, {ConstantInt::get(i32, kOpRawBufferLoad), h, c0,
                                  kind == kRawBuffer ? u32 : c1, ConstantInt::get(i8, 1), ConstantInt::get(i32, 8)}), 0);
            break;
        default:
            break;
        }
        if (ld && (fence || (fence = dx_fence_decl(m)))) {
            /* Measured (tests/windows/atomic64_test.c): without a fence, a
             * texture or typed-buffer read could come back with the value the
             * atomic after it had written, for ~7-20% of the elements; buffer
             * loads were always in order. */
            b.CreateCall(fence, {ConstantInt::get(i32, kOpBarrier), ConstantInt::get(i32, kBarrierUAVFenceGlobal)});
        }
        if (ld)
            old = b.CreateOr(b.CreateShl(b.CreateZExt(b.CreateExtractValue(ld, 1), i64), 32),
                             b.CreateZExt(b.CreateExtractValue(ld, 0), i64));
        if (!old) {
            const char *why = kind < 0 ? "an untraceable handle" : kind_name(kind);
            if (n.other_why.find(why) == std::string::npos)
                n.other_why += (n.other_why.empty() ? "" : ", ") + std::string(why);
            n.other++;
            continue;
        }
        ci->replaceAllUsesWith(old);   /* the load sits right before the atomic, so it dominates every use */
        n.rewritten++;
    }
}

}  // namespace

/* Cheap gate, no bitcode parse: a DXIL container whose SFI0 feature flags allow
 * a 64-bit atomic at all (any one implies Int64Ops), or that has no SFI0. The
 * runtime keys its DXIL cache on this answer, so it must not depend on anything
 * the rewrite does not also see. */
extern "C" int madeira_atomic64_candidate(const void *bc, size_t len)
{
    const uint8_t *d = (const uint8_t *)bc;
    bool dxil = false, sfi0 = false;
    uint64_t flags = 0;
    if (!d || len < 32 || memcmp(d, "DXBC", 4)) return 0;
    uint32_t nparts = rd32(d + 28);
    if (32 + 4ull * nparts > len) return 0;
    for (uint32_t i = 0; i < nparts; i++) {
        uint32_t off = rd32(d + 32 + 4 * i);
        if (off + 8ull > len) return 0;
        uint32_t sz = rd32(d + off + 4);
        if (off + 8ull + sz > len) return 0;
        if (!memcmp(d + off, "DXIL", 4)) dxil = true;
        if (!memcmp(d + off, "SFI0", 4) && sz >= 8) { sfi0 = true; memcpy(&flags, d + off + 8, 8); }
    }
    return dxil && (!sfi0 || (flags & (kSfi0Int64Ops | kSfi0AtomicInt64Typed | kSfi0AtomicInt64Shared | kSfi0AtomicInt64Heap)));
}

/* The container's DXIL part, or false. */
static bool find_dxil(const uint8_t *d, size_t len, const uint8_t **dxil, uint32_t *size, uint32_t *part)
{
    if (!d || len < 32 || memcmp(d, "DXBC", 4)) return false;
    uint32_t nparts = rd32(d + 28);
    if (32 + 4ull * nparts > len) return false;
    for (uint32_t i = 0; i < nparts; i++) {
        uint32_t off = rd32(d + 32 + 4 * i);
        if (off + 8ull > len) return false;
        uint32_t sz = rd32(d + off + 4);
        if (off + 8ull + sz > len) return false;
        if (!memcmp(d + off, "DXIL", 4)) { *dxil = d + off + 8; *size = sz; *part = i; return true; }
    }
    return false;
}

extern "C" int madeira_dxil_rewrite(const void *bc, size_t len, const struct madeira_dxil_rewrite_opts *opts,
                                    void **out, size_t *out_len, struct madeira_dxil_rewrite_info *info)
{
    const uint8_t *d = (const uint8_t *)bc;
    const uint8_t *dxil = nullptr;
    uint32_t dxil_size = 0, dxil_part = 0;
    *out = nullptr; *out_len = 0;
    memset(info, 0, sizeof *info);
    if (!opts->atomic64) return 0;
    if (!find_dxil(d, len, &dxil, &dxil_size, &dxil_part) || dxil_size < 24) return 0;
    uint32_t nparts = rd32(d + 28);
    uint32_t bc_off = rd32(dxil + 16), bc_size = rd32(dxil + 20);
    if (8ull + bc_off + bc_size > dxil_size) return 0;

    LLVMContext ctx;
    ctx.setOpaquePointers(false);   /* DXIL is typed-pointer IR; keep it that way on the way out */
    auto mb = MemoryBuffer::getMemBuffer(StringRef((const char *)dxil + 8 + bc_off, bc_size), "dxil", false);
    auto mod = parseBitcodeFile(mb->getMemBufferRef(), ctx);
    if (!mod) {
        std::string err = toString(mod.takeError());
        snprintf(info->a64_note, sizeof info->a64_note, "bitcode reader: %s", err.c_str());
        return -1;
    }
    bool changed = false;
    if (opts->atomic64) {
        counts_t n;
        rewrite_module(**mod, n);
        if (n.typed_cmpxchg) {
            info->a64_refuse = 1;
            snprintf(info->a64_note, sizeof info->a64_note,
                     "%u 64-bit compare-exchange(s) on a texture or typed buffer read the old value; "
                     "the converter crashes on that, so the shader is refused unconverted", n.typed_cmpxchg);
            return 0;
        }
        if (n.other)
            snprintf(info->a64_note, sizeof info->a64_note,
                     "%u 64-bit atomic(s) on %s left as they were (the converter refuses them)",
                     n.other, n.other_why.c_str());
        info->a64_rewritten = n.rewritten;
        info->a64_left = n.other;
        changed |= n.rewritten != 0;
    }
    if (!changed) return 0;

    SmallVector<char, 0> nb;
    { raw_svector_ostream os(nb); WriteBitcodeToFile(**mod, os); }
    while (nb.size() & 3) nb.push_back(0);

    /* The container again with the new DXIL part; every other part as it was.
     * The digest no longer matches; the converter does not check it. */
    size_t total = 32 + 4ull * nparts;
    for (uint32_t i = 0; i < nparts; i++)
        total += 8 + (i == dxil_part ? 24 + nb.size() : rd32(d + rd32(d + 32 + 4 * i) + 4));
    uint8_t *o = (uint8_t *)malloc(total);
    if (!o) return 0;
    memcpy(o, d, 32);
    memset(o + 4, 0, 16);
    wr32(o + 24, (uint32_t)total);
    size_t w = 32 + 4ull * nparts;
    for (uint32_t i = 0; i < nparts; i++) {
        uint32_t off = rd32(d + 32 + 4 * i), sz = rd32(d + off + 4);
        wr32(o + 32 + 4 * i, (uint32_t)w);
        if (i == dxil_part) {
            /* program header: version, size in dwords, 'DXIL', DXIL version,
             * bitcode offset (from 'DXIL'), bitcode size */
            uint32_t psz = 24 + (uint32_t)nb.size();
            memcpy(o + w, d + off, 4);
            wr32(o + w + 4, psz);
            memcpy(o + w + 8, dxil, 16);
            wr32(o + w + 12, psz / 4);
            wr32(o + w + 24, 16);
            wr32(o + w + 28, (uint32_t)nb.size());
            memcpy(o + w + 32, nb.data(), nb.size());
            w += 8 + psz;
        } else {
            memcpy(o + w, d + off, 8 + (size_t)sz);
            w += 8 + (size_t)sz;
        }
    }
    *out = o; *out_len = total;
    return 1;
}
