#ifndef DRAGON_CODEGEN_BYTES_INLINE_H
#define DRAGON_CODEGEN_BYTES_INLINE_H

#include "../CodeGenImpl.h"

namespace dragon {

struct BytesInlineFields {
    llvm::Value* len;
    llvm::Value* data;
};

template <typename CodeGenImpl>
inline llvm::MDNode* bytesTbaaTag(CodeGenImpl& impl, const char* typeName) {
    auto* scalar = llvm::MDNode::get(*impl.context, {
        llvm::MDString::get(*impl.context, typeName), impl.tbaaRoot,
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(impl.i64Type, 0))});
    return llvm::MDNode::get(*impl.context, {scalar, scalar,
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(impl.i64Type, 0))});
}

template <typename CodeGenImpl>
inline llvm::Value* emitBytesHeaderLoad(CodeGenImpl& impl, llvm::Value* obj,
                                        int64_t slot, llvm::Type* fieldType,
                                        const char* name) {
    auto* gep = impl.builder->CreateGEP(impl.i64Type, obj,
        llvm::ConstantInt::get(impl.i64Type, slot), llvm::Twine(name) + ".gep");
    auto* load = impl.builder->CreateLoad(fieldType, gep, name);
    llvm::cast<llvm::Instruction>(load)->setMetadata(
        llvm::LLVMContext::MD_tbaa, bytesTbaaTag(impl, "bytes header"));
    return load;
}

template <typename CodeGenImpl>
inline llvm::Value* emitBytesLen(CodeGenImpl& impl, llvm::Value* obj) {
    return emitBytesHeaderLoad(impl, obj, 2, impl.i64Type, "bytes.len");
}

template <typename CodeGenImpl>
inline llvm::Value* emitBytesData(CodeGenImpl& impl, llvm::Value* obj) {
    return emitBytesHeaderLoad(impl, obj, 3, impl.i8PtrType, "bytes.data");
}

template <typename CodeGenImpl>
inline llvm::Value* emitBytesByte(CodeGenImpl& impl, llvm::Value* data,
                                  llvm::Value* index) {
    auto* i8Type = llvm::Type::getInt8Ty(*impl.context);
    auto* gep = impl.builder->CreateGEP(i8Type, data, index, "bytes.elem.gep");
    auto* load = impl.builder->CreateLoad(i8Type, gep, "bytes.elem");
    llvm::cast<llvm::Instruction>(load)->setMetadata(
        llvm::LLVMContext::MD_tbaa, bytesTbaaTag(impl, "bytes data"));
    return impl.builder->CreateZExt(load, impl.i64Type, "bytes.byte");
}

template <typename CodeGenImpl>
inline BytesInlineFields emitBytesFieldsOrEmpty(CodeGenImpl& impl,
                                                llvm::Value* obj, bool wantData) {
    auto* func = impl.currentFunction;
    auto* liveBB = llvm::BasicBlock::Create(*impl.context, "bytes.live", func);
    auto* emptyBB = llvm::BasicBlock::Create(*impl.context, "bytes.empty", func);
    auto* joinBB = llvm::BasicBlock::Create(*impl.context, "bytes.join", func);
    impl.builder->CreateCondBr(impl.builder->CreateIsNull(obj, "bytes.isnull"),
                               emptyBB, liveBB);

    impl.builder->SetInsertPoint(liveBB);
    llvm::Value* len = emitBytesLen(impl, obj);
    llvm::Value* data = wantData ? emitBytesData(impl, obj) : nullptr;
    impl.builder->CreateBr(joinBB);

    impl.builder->SetInsertPoint(emptyBB);
    impl.builder->CreateBr(joinBB);

    impl.builder->SetInsertPoint(joinBB);
    auto* lenPhi = impl.builder->CreatePHI(impl.i64Type, 2, "bytes.len.safe");
    lenPhi->addIncoming(len, liveBB);
    lenPhi->addIncoming(llvm::ConstantInt::get(impl.i64Type, 0), emptyBB);
    if (!wantData) return {lenPhi, nullptr};
    auto* dataPhi = impl.builder->CreatePHI(impl.i8PtrType, 2, "bytes.data.safe");
    dataPhi->addIncoming(data, liveBB);
    dataPhi->addIncoming(llvm::Constant::getNullValue(impl.i8PtrType), emptyBB);
    return {lenPhi, dataPhi};
}

template <typename CodeGenImpl>
inline llvm::Value* emitBytesIndexRead(CodeGenImpl& impl, llvm::Value* obj,
                                       llvm::Value* index, bool indexNonNeg) {
    auto* func = impl.currentFunction;
    auto* liveBB = llvm::BasicBlock::Create(*impl.context, "bytes.idx.live", func);
    auto* okBB = llvm::BasicBlock::Create(*impl.context, "bytes.idx.ok", func);
    auto* oobBB = llvm::BasicBlock::Create(*impl.context, "bytes.idx.oob", func);
    impl.builder->CreateCondBr(impl.builder->CreateIsNull(obj, "bytes.isnull"),
                               oobBB, liveBB);

    impl.builder->SetInsertPoint(liveBB);
    llvm::Value* len = emitBytesLen(impl, obj);
    llvm::Value* finalIdx = index;
    if (!indexNonNeg) {
        auto* isNeg = impl.builder->CreateICmpSLT(index,
            llvm::ConstantInt::get(impl.i64Type, 0), "bytes.idx.neg");
        auto* adjusted = impl.builder->CreateAdd(index, len, "bytes.idx.adj");
        finalIdx = impl.builder->CreateSelect(isNeg, adjusted, index, "bytes.idx.final");
    }
    impl.builder->CreateCondBr(
        impl.builder->CreateICmpULT(finalIdx, len, "bytes.idx.inbounds"), okBB, oobBB);

    impl.builder->SetInsertPoint(oobBB);
    impl.builder->CreateCall(impl.runtimeFuncs["dragon_bytes_index_error"], {});
    impl.builder->CreateUnreachable();

    impl.builder->SetInsertPoint(okBB);
    return emitBytesByte(impl, emitBytesData(impl, obj), finalIdx);
}

}

#endif
