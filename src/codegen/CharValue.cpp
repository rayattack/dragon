#include "../CodeGenImpl.h"

#include <functional>

namespace dragon {

namespace {

bool decodeUtf8One(const std::string& s, size_t& i, uint32_t& cp) {
    unsigned char b0 = (unsigned char)s[i];
    size_t n = b0 < 0x80 ? 1
             : (b0 >> 5) == 6 ? 2
             : (b0 >> 4) == 14 ? 3
             : (b0 >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) return false;
    cp = n == 1 ? b0 : n == 2 ? (b0 & 0x1F) : n == 3 ? (b0 & 0x0F) : (b0 & 0x07);
    for (size_t k = 1; k < n; ++k) {
        unsigned char b = (unsigned char)s[i + k];
        if ((b & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (b & 0x3F);
    }
    i += n;
    return true;
}

bool decodeUtf8(const std::string& s, std::vector<uint32_t>& out) {
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        if (!decodeUtf8One(s, i, cp)) return false;
        out.push_back(cp);
    }
    return true;
}

const char* codePointPredicateRuntime(const std::string& method) {
    if (method == "isdigit" || method == "isdecimal" || method == "isnumeric")
        return "dragon_cp_isdigit";
    if (method == "isalpha") return "dragon_cp_isalpha";
    if (method == "isalnum") return "dragon_cp_isalnum";
    if (method == "isspace") return "dragon_cp_isspace";
    return nullptr;
}

}

std::optional<std::vector<uint32_t>> CodeGen::Impl::literalCodePoints(Expr* e) {
    auto* lit = dynamic_cast<StringLiteral*>(e);
    if (!lit || lit->isBytes || lit->isFString) return std::nullopt;
    std::vector<uint32_t> cps;
    if (!decodeUtf8(processEscapes(lit->value, lit->isRaw, false, lit->location()), cps))
        return std::nullopt;
    return cps;
}

std::optional<uint32_t> CodeGen::Impl::singleCodePointLiteral(Expr* e) {
    auto cps = literalCodePoints(e);
    if (!cps || cps->size() != 1) return std::nullopt;
    return (*cps)[0];
}

SubscriptExpr* CodeGen::Impl::asStrSubscript(Expr* e) {
    auto* sub = dynamic_cast<SubscriptExpr*>(e);
    if (!sub || !sub->object || !sub->object->type) return nullptr;
    if (sub->object->type->kind() != Type::Kind::Str) return nullptr;
    if (dynamic_cast<SliceExpr*>(sub->index.get())) return nullptr;
    return sub;
}

llvm::AllocaInst* CodeGen::Impl::lookupCharValueVar(const std::string& name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        if (it->vars.count(name)) return nullptr;
        auto found = it->charValueVars.find(name);
        if (found != it->charValueVars.end()) return found->second;
    }
    return nullptr;
}

bool CodeGen::Impl::isCharValueSource(Expr* e) {
    if (asStrSubscript(e)) return true;
    auto* name = dynamic_cast<NameExpr*>(e);
    return name && lookupCharValueVar(name->name) != nullptr;
}

bool CodeGen::Impl::calleeNameIsUserBound(const std::string& name) {
    std::string aliasSym = lookupImportedAlias(name);
    if (!aliasSym.empty() && module && module->getFunction(aliasSym)) return true;
    if (lookupVar(name)) return true;
    if (lookupModuleGlobal(name)) return true;
    if (module) {
        std::string mangled = mangleFunc(currentModuleName, name);
        if (module->getFunction(mangled) && !externFuncNames.count(mangled)) return true;
    }
    return false;
}

namespace {

struct CharLoopPredicates {
    std::function<bool(Expr*)> isCharOperand;
    std::function<bool(Expr*)> isStrLiteral;
    std::function<bool()> ordIsBuiltin;
};

class CharLoopUseScan : public DefaultASTVisitor {
public:
    CharLoopUseScan(const CharLoopPredicates& preds, const std::string& name)
        : preds_(preds), name_(name) {}

    bool escapes = false;

    void visit(NameExpr& node) override {
        if (node.name == name_) escapes = true;
    }

    void visit(WalrusExpr& node) override {
        if (node.name == name_) escapes = true;
        if (node.value) node.value->accept(*this);
    }

    void visit(BinaryExpr& node) override {
        auto op = node.op.type();
        Expr* left = node.left.get();
        Expr* right = node.right.get();
        if (op == TokenType::EQUAL_EQUAL || op == TokenType::NOT_EQUAL) {
            bool l = isTarget(left);
            bool r = isTarget(right);
            if (l || r) {
                Expr* other = l ? right : left;
                if (!isTarget(other) && !preds_.isCharOperand(other)) {
                    escapes = true;
                    return;
                }
                if (!isTarget(other)) other->accept(*this);
                return;
            }
        }
        if (op == TokenType::IN || op == TokenType::NOT_IN) {
            if (isTarget(left)) {
                if (!preds_.isStrLiteral(right)) escapes = true;
                return;
            }
        }
        DefaultASTVisitor::visit(node);
    }

    void visit(CallExpr& node) override {
        if (auto* callee = dynamic_cast<NameExpr*>(node.callee.get())) {
            if (callee->name == "ord" && node.args.size() == 1 && node.kwArgs.empty() &&
                isTarget(node.args[0].get()) && preds_.ordIsBuiltin())
                return;
        }
        if (auto* attr = dynamic_cast<AttributeExpr*>(node.callee.get())) {
            if (isTarget(attr->object.get()) && node.args.empty() && node.kwArgs.empty() &&
                codePointPredicateRuntime(attr->attribute))
                return;
        }
        DefaultASTVisitor::visit(node);
    }

private:
    bool isTarget(Expr* e) {
        auto* n = dynamic_cast<NameExpr*>(e);
        return n && n->name == name_;
    }

    const CharLoopPredicates& preds_;
    const std::string& name_;
};

}

bool CodeGen::Impl::charLoopTargetStaysValue(ForStmt& node, const std::string& name) {
    CharLoopPredicates preds{
        [this](Expr* e) { return asStrSubscript(e) != nullptr || singleCodePointLiteral(e).has_value(); },
        [this](Expr* e) { return literalCodePoints(e).has_value(); },
        [this]() { return !calleeNameIsUserBound("ord"); },
    };
    CharLoopUseScan scan(preds, name);
    for (auto& stmt : node.body) {
        stmt->accept(scan);
        if (scan.escapes) return false;
    }
    for (auto& stmt : node.elseBody) {
        stmt->accept(scan);
        if (scan.escapes) return false;
    }
    return true;
}

llvm::Value* CodeGen::Impl::emitCodePointLoad(llvm::Value* str, llvm::Value* len,
                                              llvm::Value* kind, llvm::Value* index) {
    auto* func = currentFunction;
    auto* zero = llvm::ConstantInt::get(i64Type, 0);
    auto* isNeg = builder->CreateICmpSLT(index, zero, "ch.idx.neg");
    auto* adjusted = builder->CreateAdd(index, len, "ch.idx.adj");
    auto* finalIdx = builder->CreateSelect(isNeg, adjusted, index, "ch.idx");

    auto* okBB = llvm::BasicBlock::Create(*context, "ch.ok", func);
    auto* oobBB = llvm::BasicBlock::Create(*context, "ch.oob", func);

    builder->CreateCondBr(builder->CreateICmpULT(finalIdx, len, "ch.inbounds"),
                          okBB, oobBB);

    builder->SetInsertPoint(oobBB);
    builder->CreateCall(runtimeFuncs["dragon_str_index_error"], {});
    builder->CreateUnreachable();

    builder->SetInsertPoint(okBB);
    return emitCodePointLoadInBounds(str, kind, finalIdx);
}

llvm::Value* CodeGen::Impl::emitCodePointLoadInBounds(llvm::Value* str, llvm::Value* kind,
                                                      llvm::Value* finalIdx) {
    auto* func = currentFunction;
    auto* narrowBB = llvm::BasicBlock::Create(*context, "ch.narrow", func);
    auto* wideBB = llvm::BasicBlock::Create(*context, "ch.wide", func);
    auto* joinBB = llvm::BasicBlock::Create(*context, "ch.join", func);
    builder->CreateCondBr(
        builder->CreateICmpEQ(kind, llvm::ConstantInt::get(i64Type, 1), "ch.narrowkind"),
        narrowBB, wideBB);

    auto* i8Ty = llvm::Type::getInt8Ty(*context);
    auto* i32Ty = llvm::Type::getInt32Ty(*context);

    builder->SetInsertPoint(narrowBB);
    auto* narrowGep = builder->CreateGEP(i8Ty, str, finalIdx, "ch.byte.gep");
    auto* narrow = builder->CreateZExt(
        builder->CreateLoad(i8Ty, narrowGep, "ch.byte"), i64Type, "ch.narrow.cp");
    builder->CreateBr(joinBB);

    builder->SetInsertPoint(wideBB);
    auto* wideGep = builder->CreateGEP(i32Ty, str, finalIdx, "ch.ucs4.gep");
    auto* wide = builder->CreateZExt(
        builder->CreateLoad(i32Ty, wideGep, "ch.ucs4"), i64Type, "ch.wide.cp");
    builder->CreateBr(joinBB);

    builder->SetInsertPoint(joinBB);
    auto* cp = builder->CreatePHI(i64Type, 2, "ch.cp");
    cp->addIncoming(narrow, narrowBB);
    cp->addIncoming(wide, wideBB);
    return cp;
}

llvm::Value* CodeGen::Impl::emitCharValue(CodeGen& cg, Expr* e) {
    if (auto* name = dynamic_cast<NameExpr*>(e)) {
        auto* slot = lookupCharValueVar(name->name);
        return builder->CreateLoad(i64Type, slot, name->name + ".cp");
    }
    auto* sub = asStrSubscript(e);
    sub->object->accept(cg);
    llvm::Value* str = lastValue;
    if (!str->getType()->isPointerTy()) {
        addError("internal error: str subscript receiver is not a pointer",
                 sub->location());
        return llvm::ConstantInt::get(i64Type, 0);
    }
    VarKind drain = ownedTempDrainKind(sub->object.get(), str);
    std::vector<llvm::Value*> bases;
    pushTempCleanupByKind(str, drain, bases);
    sub->index->accept(cg);
    llvm::Value* index = lastValue;
    if (index->getType() != i64Type)
        index = builder->CreateZExtOrTrunc(index, i64Type, "ch.idx.i64");
    auto* len = builder->CreateCall(runtimeFuncs["dragon_str_len"], {str}, "ch.len");
    auto* kind = builder->CreateCall(runtimeFuncs["dragon_str_kind"], {str}, "ch.kind");
    llvm::Value* cp = emitCodePointLoad(str, len, kind, index);
    popArgTempCleanups(bases);
    if (drain != VarKind::Other) emitDecrefByKind(str, drain);
    return cp;
}

bool CodeGen::Impl::tryEmitCharCompare(CodeGen& cg, BinaryExpr& node) {
    auto op = node.op.type();
    Expr* left = node.left.get();
    Expr* right = node.right.get();
    if (op == TokenType::EQUAL_EQUAL || op == TokenType::NOT_EQUAL) {
        bool leftIsChar = isCharValueSource(left);
        bool rightIsChar = isCharValueSource(right);
        auto leftLit = singleCodePointLiteral(left);
        auto rightLit = singleCodePointLiteral(right);
        bool lowerable = (leftIsChar && (rightIsChar || rightLit)) ||
                         (rightIsChar && leftLit);
        if (!lowerable) return false;
        llvm::Value* a = leftIsChar ? emitCharValue(cg, left)
                                    : llvm::ConstantInt::get(i64Type, *leftLit);
        llvm::Value* b = rightIsChar ? emitCharValue(cg, right)
                                     : llvm::ConstantInt::get(i64Type, *rightLit);
        lastValue = op == TokenType::EQUAL_EQUAL ? builder->CreateICmpEQ(a, b, "ch.eq")
                                                 : builder->CreateICmpNE(a, b, "ch.ne");
        return true;
    }
    if (op == TokenType::IN) {
        if (!isCharValueSource(left)) return false;
        auto cps = literalCodePoints(right);
        if (!cps) return false;
        llvm::Value* c = emitCharValue(cg, left);
        llvm::Value* found = llvm::ConstantInt::getFalse(*context);
        for (uint32_t cp : *cps) {
            found = builder->CreateOr(
                found,
                builder->CreateICmpEQ(c, llvm::ConstantInt::get(i64Type, cp), "ch.in.eq"),
                "ch.in");
        }
        lastValue = found;
        return true;
    }
    return false;
}

bool CodeGen::Impl::tryEmitCharPredicate(CodeGen& cg, CallExpr& node, AttributeExpr& attr) {
    if (!node.args.empty()) return false;
    const char* runtimeName = codePointPredicateRuntime(attr.attribute);
    if (!runtimeName || !isCharValueSource(attr.object.get())) return false;
    llvm::Value* cp = emitCharValue(cg, attr.object.get());
    auto* call = builder->CreateCall(runtimeFuncs[runtimeName], {cp}, attr.attribute);
    lastValue = builder->CreateICmpNE(call, llvm::ConstantInt::get(i64Type, 0),
                                      attr.attribute + ".b");
    return true;
}

}
