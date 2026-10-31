/* wine-lab port: RGB32 typed-buffer reads in the converter's AIR output. See
 * madeira_air_rewrite.h for the descriptor contract.
 *
 * MSC 4.0 reads a typed buffer as
 *     %t = load %struct._texture_buffer_1d_t addrspace(1)*, ... addrspace(2)* %A
 *     call { <4 x float>, i8 } @air.read_texture_buffer_1d.v4f32(%t, i32 %i, i32 0)
 * where %A addresses field 1 (textureViewID) of a 24-byte descriptor
 * { gpuVA, textureViewID, metadata } -- in the top-level argument buffer, a
 * descriptor table or the resource heap alike. The rewrite loads the metadata
 * (%A + 8) and, when MADEIRA_DESC_RGB32 is set, three i32 from gpuVA (%A - 8)
 * at %i * 3 instead of the texture read; GetDimensions (get_width) becomes
 * size / 12. A read whose texture does not come straight from such a field is
 * left alone and counted as unmatched.
 *
 * Texture-view offset (every texture-buffer access, SRV or UAV: read, write,
 * atomic, GetDimensions). A view starts on the device's linear-texture
 * alignment and metadata bits 32..39 hold the elements between that start and
 * the view's first element (IRBufferView textureViewOffsetInElements). MSC
 * writes that field but its code never reads it: typed loads, stores and
 * atomics use the coordinate as is, IncrementCounter uses 0, get_width the
 * texture's width. The rewrite adds the offset to the coordinate (argument 1
 * of every such call) and subtracts it from get_width. Raw descriptors keep
 * those bits 0; the RGB32 path above reads from gpuVA, which is exact, and
 * keeps its index.
 *
 * The metallib is then repacked around the new bitcode: header sizes, the
 * function's HASH (SHA-256 of the bitcode section) and MDSZ, and the offset of
 * any extended-header section placed after the bitcode (RLST). One function per
 * library, which is what MSC produces. */
#include "madeira_air_rewrite.h"

#include <CommonCrypto/CommonDigest.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace {

#pragma pack(push, 1)
struct MTLBHeader {
    uint32_t magic;
    uint16_t platform, ver_major, ver_minor;
    uint8_t type, os;
    uint16_t os_major, os_minor;
    uint64_t file_size, fl_off, fl_size, pub_off, pub_size, priv_off, priv_size, bc_off, bc_size;
};
#pragma pack(pop)
static_assert(sizeof(MTLBHeader) == 88, "metallib header");

const char *kRead = "air.read_texture_buffer_1d.";
const char *kWidth = "air.get_width_texture_buffer_1d";

bool has_typed_buffer_access(const void *lib, size_t len) {
    return memmem(lib, len, "texture_buffer_1d", 17) != nullptr;
}

/* v == 24 * n + k for some n, k constant. */
bool mult24(Value *v, int64_t &k) {
    while (isa<SExtInst>(v) || isa<ZExtInst>(v)) v = cast<Instruction>(v)->getOperand(0);
    if (auto *ci = dyn_cast<ConstantInt>(v)) { k = ci->getSExtValue(); return true; }
    if (auto *bo = dyn_cast<BinaryOperator>(v)) {
        if (bo->getOpcode() == Instruction::Mul)
            for (unsigned i = 0; i < 2; i++)
                if (auto *ci = dyn_cast<ConstantInt>(bo->getOperand(i)))
                    if (ci->getSExtValue() % 24 == 0) { k = 0; return true; }
        if (bo->getOpcode() == Instruction::Add) {
            int64_t a, b;
            if (mult24(bo->getOperand(0), a) && mult24(bo->getOperand(1), b)) { k = a + b; return true; }
        }
    }
    return false;
}
/* p == <the resource descriptor heap argument> + 24 * n + c, by i8 GEPs. */
bool heap_offset(Value *p, int64_t &c) {
    p = p->stripPointerCasts();
    if (auto *arg = dyn_cast<Argument>(p)) {
        auto *pt = dyn_cast<PointerType>(arg->getType());
        auto *st = pt && !pt->isOpaque() ? dyn_cast<StructType>(pt->getNonOpaquePointerElementType()) : nullptr;
        if (!st || !st->hasName() || !st->getName().contains("res_desc_heap")) return false;
        c = 0; return true;
    }
    auto *g = dyn_cast<GEPOperator>(p);
    if (!g || g->getNumIndices() != 1 || !g->getSourceElementType()->isIntegerTy(8)) return false;
    int64_t base, k;
    if (!heap_offset(g->getPointerOperand(), base) || !mult24(g->getOperand(1), k)) return false;
    c = base + k; return true;
}

/* %A when %t is loaded from field 1 (textureViewID) of a 24-byte descriptor:
 * `load (gep <{ ptr, ptr, i64 }> ..., i32 1)` (argument buffer, tables), or
 * an i8 offset of 24 * n + 8 from the resource heap (SM 6.6); else null. */
Value *desc_field1(Value *t) {
    auto *ld = dyn_cast<LoadInst>(t->stripPointerCasts());
    if (!ld) return nullptr;
    {
        int64_t c;
        if (heap_offset(ld->getPointerOperand(), c)) return ((c % 24) + 24) % 24 == 8 ? ld->getPointerOperand() : nullptr;
    }
    auto *gep = dyn_cast<GEPOperator>(ld->getPointerOperand()->stripPointerCasts());
    if (!gep || gep->getNumIndices() < 2) return nullptr;
    SmallVector<Value *, 4> idx(gep->idx_begin(), gep->idx_end() - 1);   /* all but the field index */
    Type *into = GetElementPtrInst::getIndexedType(gep->getSourceElementType(), idx);
    auto *st = dyn_cast_or_null<StructType>(into);
    auto *ci = dyn_cast<ConstantInt>(gep->getOperand(gep->getNumOperands() - 1));
    if (!st || !ci || ci->getZExtValue() != 1 || st->getNumElements() != 3) return nullptr;
    if (!st->getElementType(0)->isPointerTy() || !st->getElementType(1)->isPointerTy() || !st->getElementType(2)->isIntegerTy(64))
        return nullptr;
    return ld->getPointerOperand();
}

struct DescParts { Value *md, *flag, *n; };

/* Before `at`: the metadata word, the RGB32 test and the element count. */
DescParts load_desc(IRBuilder<> &b, Value *A) {
    LLVMContext &c = b.getContext();
    unsigned as = A->getType()->getPointerAddressSpace();
    Value *p8 = b.CreateBitCast(A, Type::getInt8PtrTy(c, as));
    Value *mdp = b.CreateBitCast(b.CreateConstGEP1_64(Type::getInt8Ty(c), p8, 8), Type::getInt64PtrTy(c, as));
    LoadInst *md = b.CreateAlignedLoad(Type::getInt64Ty(c), mdp, Align(8), "rgb32.md");
    Value *flag = b.CreateICmpNE(b.CreateAnd(md, b.getInt64(MADEIRA_DESC_RGB32)), b.getInt64(0), "rgb32.is");
    Value *n = b.CreateUDiv(b.CreateTrunc(md, b.getInt32Ty()), b.getInt32(12), "rgb32.n");
    return { md, flag, n };
}

bool rewrite_read(CallInst *call, Value *A) {
    LLVMContext &c = call->getContext();
    auto *rt = dyn_cast<StructType>(call->getType());
    if (!rt || rt->getNumElements() != 2) return false;
    auto *vt = dyn_cast<FixedVectorType>(rt->getElementType(0));
    if (!vt || vt->getNumElements() != 4 || vt->getElementType()->getPrimitiveSizeInBits() != 32) return false;
    Type *et = vt->getElementType();
    bool is_float = et->isFloatTy();
    if (!is_float && !et->isIntegerTy(32)) return false;
    Type *st1 = rt->getElementType(1);
    if (!st1->isIntegerTy()) return false;
    unsigned as = A->getType()->getPointerAddressSpace();

    IRBuilder<> b(call);
    DescParts d = load_desc(b, A);
    Instruction *thenT = nullptr, *elseT = nullptr;
    SplitBlockAndInsertIfThenElse(d.flag, call, &thenT, &elseT);
    BasicBlock *tail = call->getParent();
    call->moveBefore(elseT);

    b.SetInsertPoint(thenT);
    Value *idx = call->getArgOperand(1);
    Value *inb = b.CreateICmpULT(idx, d.n);
    Value *i2 = b.CreateSelect(inb, idx, b.getInt32(0));
    Type *i32p = PointerType::get(Type::getInt32Ty(c), 1);
    Value *p8 = b.CreateBitCast(A, Type::getInt8PtrTy(c, as));
    Value *vap = b.CreateBitCast(b.CreateConstGEP1_64(Type::getInt8Ty(c), p8, -8), PointerType::get(i32p, as));
    Value *va = b.CreateAlignedLoad(i32p, vap, Align(8), "rgb32.va");
    Value *e0 = b.CreateMul(b.CreateZExt(i2, b.getInt64Ty()), b.getInt64(3));
    Value *vec = UndefValue::get(vt);
    for (unsigned k = 0; k < 3; k++) {
        Value *ptr = b.CreateGEP(Type::getInt32Ty(c), va, b.CreateAdd(e0, b.getInt64(k)));
        Value *v = b.CreateAlignedLoad(Type::getInt32Ty(c), ptr, Align(4));
        if (is_float) v = b.CreateBitCast(v, et);
        vec = b.CreateInsertElement(vec, v, (uint64_t)k);
    }
    vec = b.CreateInsertElement(vec, is_float ? ConstantFP::get(et, 1.0) : ConstantInt::get(et, 1), (uint64_t)3);
    vec = b.CreateSelect(inb, vec, Constant::getNullValue(vt));   /* out of range reads 0, as D3D's */
    Value *res = b.CreateInsertValue(UndefValue::get(rt), vec, 0);
    res = b.CreateInsertValue(res, ConstantInt::get(st1, 1), 1);

    b.SetInsertPoint(&*tail->getFirstInsertionPt());
    PHINode *phi = b.CreatePHI(rt, 2, "rgb32.read");
    call->replaceAllUsesWith(phi);
    phi->addIncoming(res, thenT->getParent());
    phi->addIncoming(call, elseT->getParent());
    return true;
}

/* The texture-view offset in elements: metadata bits 32..39. */
Value *view_offset(IRBuilder<> &b, Value *md) {
    return b.CreateTrunc(b.CreateAnd(b.CreateLShr(md, b.getInt64(32)), b.getInt64(0xff)), b.getInt32Ty(), "tbuf.off");
}

/* Coordinate (argument 1) + offset, before the call; past 2^32 stays out of range. */
bool offset_coord(CallInst *call, Value *A) {
    if (call->arg_size() < 2 || !call->getArgOperand(1)->getType()->isIntegerTy(32)) return false;
    IRBuilder<> b(call);
    Value *i = call->getArgOperand(1);
    Value *j = b.CreateAdd(i, view_offset(b, load_desc(b, A).md), "tbuf.coord");
    call->setArgOperand(1, b.CreateSelect(b.CreateICmpULT(j, i), b.getInt32(~0u), j));
    return true;
}

/* GetDimensions: the texture's width less the offset (with the RGB32 count
 * when `rgb32` and the view is one). */
bool rewrite_width(CallInst *call, Value *A, bool rgb32) {
    if (!call->getType()->isIntegerTy(32)) return false;
    IRBuilder<> b(call->getNextNode());
    DescParts d = load_desc(b, A);
    Value *off = view_offset(b, d.md);
    Value *w = b.CreateSelect(b.CreateICmpUGT(call, off), b.CreateSub(call, off), b.getInt32(0), "tbuf.width");
    if (rgb32) w = b.CreateSelect(d.flag, d.n, w, "rgb32.width");
    Instruction *last = cast<Instruction>(w);
    call->replaceUsesWithIf(w, [&](Use &u) { Instruction *ui = dyn_cast<Instruction>(u.getUser()); return !ui || ui->getParent() != last->getParent() || last->comesBefore(ui); });
    return true;
}

/* A typed UAV texture: some use of it is a write, an atomic, a fence, or a read
 * with read_write access (the third argument of air.read_texture_buffer_1d is
 * the access: 0 for an SRV). D3D12 has no RGB32 UAVs, so these are never an
 * RGB32 view. */
bool is_uav_texture(Value *t) {
    for (User *u : t->users())
        if (auto *ci = dyn_cast<CallInst>(u))
            if (Function *f = ci->getCalledFunction()) {
                StringRef n = f->getName();
                if (!n.startswith("air.") || !n.contains("texture_buffer_1d")) continue;
                if (n.startswith("air.write_") || n.contains("atomic") || n.startswith("air.fence_")) return true;
                if (n.startswith(kRead) && ci->arg_size() >= 3)
                    if (auto *acc = dyn_cast<ConstantInt>(ci->getArgOperand(2)))
                        if (!acc->isZero()) return true;
            }
    return false;
}

/* The descriptor field pointer for texture t, also through phis and selects of
 * textures that each have one (a phi or select of the field pointers is built
 * next to the original). */
Value *field_ptr(Value *t, DenseMap<Value *, Value *> &memo, bool &via) {
    t = t->stripPointerCasts();
    auto it = memo.find(t);
    if (it != memo.end()) return it->second;
    if (Value *A = desc_field1(t)) { memo[t] = A; return A; }
    if (auto *phi = dyn_cast<PHINode>(t)) {
        memo[t] = nullptr;   /* a cycle through itself resolves below */
        SmallVector<Value *, 4> in;
        Type *ty = nullptr;
        for (Value *v : phi->incoming_values()) {
            if (v == phi) { in.push_back(nullptr); continue; }
            Value *A = field_ptr(v, memo, via);
            if (!A) return nullptr;
            if (!ty) ty = A->getType();
            in.push_back(A);
        }
        if (!ty) return nullptr;
        PHINode *np = PHINode::Create(ty, phi->getNumIncomingValues(), "rgb32.field", phi);
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
            Value *A = in[i] ? in[i] : np;
            if (A->getType() != ty) {
                IRBuilder<> b(phi->getIncomingBlock(i)->getTerminator());
                A = b.CreateBitCast(A, ty);
            }
            np->addIncoming(A, phi->getIncomingBlock(i));
        }
        via = true;
        memo[t] = np;
        return np;
    }
    if (auto *sel = dyn_cast<SelectInst>(t)) {
        Value *a = field_ptr(sel->getTrueValue(), memo, via), *b2 = field_ptr(sel->getFalseValue(), memo, via);
        if (!a || !b2) return nullptr;
        IRBuilder<> b(sel);
        if (b2->getType() != a->getType()) b2 = b.CreateBitCast(b2, a->getType());
        Value *r = b.CreateSelect(sel->getCondition(), a, b2, "rgb32.field");
        via = true;
        memo[t] = r;
        return r;
    }
    return nullptr;
}

int rewrite_module(Module &m, madeira_air_rgb32_info *info, unsigned what) {
    std::vector<CallInst *> reads, widths, others;
    DenseMap<Value *, Value *> memo;
    const bool rgb32 = what & MADEIRA_AIR_RGB32, offs = what & MADEIRA_AIR_VIEW_OFFSET;
    for (Function &f : m) {
        if (!f.isDeclaration() && !info->name[0]) snprintf(info->name, sizeof info->name, "%s", f.getName().str().c_str());
        for (BasicBlock &bb : f)
            for (Instruction &i : bb)
                if (auto *ci = dyn_cast<CallInst>(&i))
                    if (Function *cf = ci->getCalledFunction()) {
                        StringRef n = cf->getName();
                        if (n.startswith(kRead)) reads.push_back(ci);
                        else if (n == kWidth) widths.push_back(ci);
                        else if ((n.startswith("air.write_") || n.startswith("air.atomic_")) && n.contains("texture_buffer_1d"))
                            others.push_back(ci);
                    }
    }
    int changed = 0;
    for (CallInst *ci : reads) {
        auto *acc = ci->arg_size() >= 3 ? dyn_cast<ConstantInt>(ci->getArgOperand(2)) : nullptr;
        bool srv = acc && acc->isZero() && !is_uav_texture(ci->getArgOperand(0)), via = false;
        if (!srv) info->uav++;
        if (!srv && !offs) continue;
        Value *A = field_ptr(ci->getArgOperand(0), memo, via);
        if (!A) { if (srv && rgb32) info->unmatched++; else info->off_unmatched++; continue; }
        if (srv && rgb32 && rewrite_read(ci, A)) { info->reads++; info->traced += via; changed = 1; }   /* ci now runs only for other views */
        else if (srv && rgb32) info->unmatched++;
        if (offs && offset_coord(ci, A)) { info->offsets++; changed = 1; }
    }
    for (CallInst *ci : widths) {
        bool srv = !is_uav_texture(ci->getArgOperand(0)), via = false;
        if (!srv) info->uav++;
        if (!srv && !offs) continue;
        Value *A = field_ptr(ci->getArgOperand(0), memo, via);
        if (!A) { if (srv && rgb32) info->unmatched++; else info->off_unmatched++; continue; }
        if (offs || (srv && rgb32)) {
            if (!rewrite_width(ci, A, srv && rgb32)) { if (srv && rgb32) info->unmatched++; continue; }
            if (srv && rgb32) { info->widths++; info->traced += via; }
            if (offs) info->offsets++;
            changed = 1;
        }
    }
    if (offs)
        for (CallInst *ci : others) {
            bool via = false;
            Value *A = field_ptr(ci->getArgOperand(0), memo, via);
            if (A && offset_coord(ci, A)) { info->offsets++; changed = 1; }
            else info->off_unmatched++;
        }
    return changed;
}

/* Tag walk: returns the offset after ENDT, or 0. */
size_t tags_end(const unsigned char *b, size_t o, size_t lim) {
    while (o + 4 <= lim) {
        if (!memcmp(b + o, "ENDT", 4)) return o + 4;
        if (o + 6 > lim) return 0;
        uint16_t sz; memcpy(&sz, b + o + 4, 2);
        o += 6 + sz;
    }
    return 0;
}

} // namespace

extern "C" int madeira_air_rgb32_rewrite(const void *lib, size_t len, void **out, size_t *out_len,
                                         struct madeira_air_rgb32_info *info, unsigned what) {
    const unsigned char *b = (const unsigned char *)lib;
    MTLBHeader h;
    *out = nullptr; *out_len = 0;
    memset(info, 0, sizeof *info);
    if (!lib || len < sizeof h || !has_typed_buffer_access(lib, len)) return 0;
#define FAIL(...) do { snprintf(info->note, sizeof info->note, __VA_ARGS__); if (!info->unmatched) info->unmatched = 1; return -1; } while (0)
    memcpy(&h, b, sizeof h);
    if (h.magic != 0x424c544du || h.file_size != len || h.bc_off + h.bc_size > len || h.fl_off + 4 > len)
        FAIL("not a metallib this rewrite knows");
    uint32_t nfn; memcpy(&nfn, b + h.fl_off, 4);
    if (nfn != 1) FAIL("%u functions in the library", nfn);
    /* the function entry: u32 size, then tags to ENDT */
    size_t ent = h.fl_off + 4, ent_end = tags_end(b, ent + 4, len);
    if (!ent_end) FAIL("function entry unreadable");
    size_t hash_at = 0, mdsz_at = 0;
    for (size_t o = ent + 4; o + 6 <= ent_end - 4;) {
        uint16_t sz; memcpy(&sz, b + o + 4, 2);
        if (!memcmp(b + o, "HASH", 4) && sz == 32) hash_at = o + 6;
        if (!memcmp(b + o, "MDSZ", 4) && sz == 8) mdsz_at = o + 6;
        if (!memcmp(b + o, "OFFT", 4) && sz == 24) {
            uint64_t bco; memcpy(&bco, b + o + 6 + 16, 8);
            if (bco != 0) FAIL("bitcode not at the start of its section");
        }
        o += 6 + sz;
    }
    if (!hash_at || !mdsz_at) FAIL("function entry without HASH or MDSZ");
    /* extended header (after the function list, before the public metadata):
     * tags whose payload is an (offset, size) pair past the bitcode move with it */
    std::vector<size_t> moved;
    {
        size_t o = ent_end, lim = h.pub_off;
        while (o + 4 <= lim && memcmp(b + o, "ENDT", 4)) {
            uint16_t sz; memcpy(&sz, b + o + 4, 2);
            if (!memcmp(b + o, "RLST", 4) || !memcmp(b + o, "SARC", 4) || !memcmp(b + o, "SRCS", 4) || !memcmp(b + o, "EMBD", 4)) {
                if (sz != 16) FAIL("extended-header section tag of %u bytes", sz);
                uint64_t off; memcpy(&off, b + o + 6, 8);
                if (off >= h.bc_off + h.bc_size) moved.push_back(o + 6);
                else if (off >= h.bc_off) FAIL("a section inside the bitcode");
            } else if (memcmp(b + o, "UUID", 4)) {
                char t[5] = { (char)b[o], (char)b[o + 1], (char)b[o + 2], (char)b[o + 3], 0 };
                FAIL("unknown extended-header tag %s", t);
            }
            o += 6 + sz;
        }
    }
    /* the bitcode, without its wrapper */
    const unsigned char *bc = b + h.bc_off; size_t bclen = h.bc_size;
    if (bclen >= 20) {
        uint32_t magic; memcpy(&magic, bc, 4);
        if (magic == 0x0b17c0deu) {
            uint32_t off, sz; memcpy(&off, bc + 8, 4); memcpy(&sz, bc + 12, 4);
            if ((size_t)off + sz > bclen) FAIL("bad bitcode wrapper");
            bc += off; bclen = sz;
        }
    }
    LLVMContext ctx;
    ctx.setOpaquePointers(false);
    auto mod = parseBitcodeFile(MemoryBufferRef(StringRef((const char *)bc, bclen), "air"), ctx);
    if (!mod) { std::string e = toString(mod.takeError()); FAIL("bitcode: %s", e.c_str()); }
    if (!rewrite_module(**mod, info, what)) {
        if (info->unmatched) snprintf(info->note, sizeof info->note, "%u typed SRV accesses without a descriptor field", info->unmatched);
        return info->unmatched ? -1 : 0;
    }
    SmallVector<char, 0> nbc;
    {
        raw_svector_ostream os(nbc);
        WriteBitcodeToFile(**mod, os);
    }
    size_t nlen = len - h.bc_size + nbc.size();
    unsigned char *o = (unsigned char *)malloc(nlen);
    if (!o) FAIL("out of memory");
    memcpy(o, b, h.bc_off);
    memcpy(o + h.bc_off, nbc.data(), nbc.size());
    memcpy(o + h.bc_off + nbc.size(), b + h.bc_off + h.bc_size, len - h.bc_off - h.bc_size);
    int64_t delta = (int64_t)nbc.size() - (int64_t)h.bc_size;
    h.file_size = nlen; h.bc_size = nbc.size();
    memcpy(o, &h, sizeof h);
    uint64_t sz64 = nbc.size(); memcpy(o + mdsz_at, &sz64, 8);
    CC_SHA256(nbc.data(), (CC_LONG)nbc.size(), o + hash_at);
    for (size_t at : moved) { uint64_t off; memcpy(&off, o + at, 8); off += delta; memcpy(o + at, &off, 8); }
    *out = o; *out_len = nlen;
    if (info->unmatched) snprintf(info->note, sizeof info->note, "%u typed SRV accesses without a descriptor field", info->unmatched);
    return 1;
#undef FAIL
}
