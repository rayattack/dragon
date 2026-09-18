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
    std::function<bool(Expr*)> exprMentionsTarget;
    std::function<bool(Stmt*)> stmtMentionsTarget;
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

    void visit(LambdaExpr& node) override { escapeIfMentioned(&node); }
    void visit(FireExpr& node) override { escapeIfMentioned(&node); }
    void visit(GeneratorExpr& node) override { escapeIfMentioned(&node); }
    void visit(ThreadStmt& node) override { escapeIfMentioned(&node); }
    void visit(FunctionDecl& node) override { escapeIfMentioned(&node); }
    void visit(ClassDecl& node) override { escapeIfMentioned(&node); }

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

    void escapeIfMentioned(Expr* closure) {
        if (preds_.exprMentionsTarget(closure)) escapes = true;
    }

    void escapeIfMentioned(Stmt* closure) {
        if (preds_.stmtMentionsTarget(closure)) escapes = true;
    }

    const CharLoopPredicates& preds_;
    const std::string& name_;
};

}

namespace {

class AssignedNameCollector : public DefaultASTVisitor {
public:
    explicit AssignedNameCollector(std::unordered_set<std::string>& out) : out_(out) {}
    void visit(AssignStmt& node) override {
        for (auto& t : node.targets) noteTarget(t.get());
        DefaultASTVisitor::visit(node);
    }
    void visit(AugAssignStmt& node) override {
        noteTarget(node.target.get());
        DefaultASTVisitor::visit(node);
    }
    void visit(AnnAssignStmt& node) override {
        noteTarget(node.target.get());
        DefaultASTVisitor::visit(node);
    }
    void visit(WalrusExpr& node) override {
        out_.insert(node.name);
        DefaultASTVisitor::visit(node);
    }
    void visit(ForStmt& node) override {
        noteTarget(node.target.get());
        DefaultASTVisitor::visit(node);
    }
    void visit(DeleteStmt& node) override {
        for (auto& t : node.targets) noteTarget(t.get());
        DefaultASTVisitor::visit(node);
    }
    void visit(FunctionDecl& node) override { out_.insert(node.name); }
    void visit(ClassDecl& node) override { out_.insert(node.name); }

private:
    void noteTarget(Expr* t) {
        if (auto* n = dynamic_cast<NameExpr*>(t)) { out_.insert(n->name); return; }
        if (auto* tup = dynamic_cast<TupleExpr*>(t))
            for (auto& e : tup->elements) noteTarget(e.get());
        if (auto* lst = dynamic_cast<ListExpr*>(t))
            for (auto& e : lst->elements) noteTarget(e.get());
    }
    std::unordered_set<std::string>& out_;
};

}

void CodeGen::Impl::collectAssignedNames(const std::vector<std::unique_ptr<Stmt>>& body,
                                         std::unordered_set<std::string>& out) {
    AssignedNameCollector collector(out);
    for (auto& stmt : body) stmt->accept(collector);
}

llvm::Instruction* CodeGen::Impl::sequentialCacheResetFor(Expr* strExpr) {
    if (loopStack.empty() || !loopStack.top().preheaderBr) return nullptr;
    auto* name = dynamic_cast<NameExpr*>(strExpr);
    if (!name || !lookupVar(name->name) || isCellBacked(name->name)) return nullptr;
    if (lookupModuleGlobal(name->name)) return nullptr;
    if (loopStack.top().assignedNames.count(name->name)) return nullptr;
    return loopStack.top().preheaderBr;
}

bool CodeGen::Impl::charLoopTargetStaysValue(ForStmt& node, const std::string& name) {
    CharLoopPredicates preds{
        [this](Expr* e) { return asStrSubscript(e) != nullptr || singleCodePointLiteral(e).has_value(); },
        [this](Expr* e) { return literalCodePoints(e).has_value(); },
        [this]() { return !calleeNameIsUserBound("ord"); },
        [this, &name](Expr* e) { return nodeMentionsName(e, name); },
        [this, &name](Stmt* s) { return nodeMentionsName(s, name); },
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
                                              llvm::Value* kind, llvm::Value* index,
                                              llvm::Instruction* cacheReset) {
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
    return emitCodePointLoadInBounds(str, kind, finalIdx, cacheReset);
}

llvm::Value* CodeGen::Impl::emitCodePointAtOffset(llvm::Value* str, llvm::Value* offset,
                                                  llvm::Value** advanceOut) {
    auto* func = currentFunction;
    auto* i8Ty = llvm::Type::getInt8Ty(*context);
    auto* asciiBB = llvm::BasicBlock::Create(*context, "cp.ascii", func);
    auto* multiBB = llvm::BasicBlock::Create(*context, "cp.multi", func);
    auto* joinBB = llvm::BasicBlock::Create(*context, "cp.join", func);
    auto* gep = builder->CreateGEP(i8Ty, str, offset, "cp.byte.gep");
    auto* byte = builder->CreateZExt(builder->CreateLoad(i8Ty, gep, "cp.byte"), i64Type,
                                     "cp.byte.i64");
    builder->CreateCondBr(
        builder->CreateICmpULT(byte, llvm::ConstantInt::get(i64Type, 0x80), "cp.is.ascii"),
        asciiBB, multiBB);

    builder->SetInsertPoint(asciiBB);
    builder->CreateBr(joinBB);

    builder->SetInsertPoint(multiBB);
    auto* packed = builder->CreateCall(runtimeFuncs["dragon_str_decode_at"], {str, offset},
                                       "cp.packed");
    auto* multiCp = builder->CreateAnd(packed, llvm::ConstantInt::get(i64Type, 0x1FFFFF),
                                       "cp.multi.cp");
    auto* multiAdv = builder->CreateLShr(packed, llvm::ConstantInt::get(i64Type, 32),
                                         "cp.multi.adv");
    builder->CreateBr(joinBB);

    builder->SetInsertPoint(joinBB);
    auto* cp = builder->CreatePHI(i64Type, 2, "cp.value");
    cp->addIncoming(byte, asciiBB);
    cp->addIncoming(multiCp, multiBB);
    auto* adv = builder->CreatePHI(i64Type, 2, "cp.advance");
    adv->addIncoming(llvm::ConstantInt::get(i64Type, 1), asciiBB);
    adv->addIncoming(multiAdv, multiBB);
    if (advanceOut) *advanceOut = adv;
    return cp;
}

llvm::Value* CodeGen::Impl::emitCodePointLoadInBounds(llvm::Value* str, llvm::Value* kind,
                                                      llvm::Value* finalIdx,
                                                      llvm::Instruction* cacheReset) {
    auto* func = currentFunction;
    auto* narrowBB = llvm::BasicBlock::Create(*context, "ch.narrow", func);
    auto* wideBB = llvm::BasicBlock::Create(*context, "ch.wide", func);
    auto* joinBB = llvm::BasicBlock::Create(*context, "ch.join", func);
    builder->CreateCondBr(
        builder->CreateICmpEQ(kind, llvm::ConstantInt::get(i64Type, 1), "ch.narrowkind"),
        narrowBB, wideBB);

    auto* i8Ty = llvm::Type::getInt8Ty(*context);

    builder->SetInsertPoint(narrowBB);
    auto* narrowGep = builder->CreateGEP(i8Ty, str, finalIdx, "ch.byte.gep");
    auto* narrow = builder->CreateZExt(
        builder->CreateLoad(i8Ty, narrowGep, "ch.byte"), i64Type, "ch.narrow.cp");
    builder->CreateBr(joinBB);

    builder->SetInsertPoint(wideBB);
    llvm::Value* wide = nullptr;
    llvm::BasicBlock* wideEndBB = nullptr;
    if (!cacheReset) {
        wide = builder->CreateCall(runtimeFuncs["dragon_str_cp_at_index"], {str, finalIdx},
                                   "ch.wide.cp");
        wideEndBB = builder->GetInsertBlock();
    } else {
        auto* cacheIdx = createEntryAllocaInit(func, "ch.cache.idx",
                                               llvm::ConstantInt::get(i64Type, -2));
        auto* cacheNext = createEntryAllocaInit(func, "ch.cache.next",
                                                llvm::ConstantInt::get(i64Type, 0));
        llvm::IRBuilder<> reset(cacheReset);
        reset.CreateStore(llvm::ConstantInt::get(i64Type, -2), cacheIdx);
        auto* nextIdx = builder->CreateAdd(builder->CreateLoad(i64Type, cacheIdx, "ch.cache.idx.v"),
                                           llvm::ConstantInt::get(i64Type, 1), "ch.cache.idx.next");
        auto* hit = builder->CreateICmpEQ(finalIdx, nextIdx, "ch.cache.hit");
        auto* hitBB = llvm::BasicBlock::Create(*context, "ch.cache.hitbb", func);
        auto* missBB = llvm::BasicBlock::Create(*context, "ch.cache.miss", func);
        auto* offBB = llvm::BasicBlock::Create(*context, "ch.cache.off", func);
        builder->CreateCondBr(hit, hitBB, missBB);

        builder->SetInsertPoint(hitBB);
        auto* hitOff = builder->CreateLoad(i64Type, cacheNext, "ch.cache.next.v");
        builder->CreateBr(offBB);

        builder->SetInsertPoint(missBB);
        auto* missOff = builder->CreateCall(runtimeFuncs["dragon_str_cp_byte_offset"],
                                            {str, finalIdx}, "ch.cache.lookup");
        builder->CreateBr(offBB);

        builder->SetInsertPoint(offBB);
        auto* off = builder->CreatePHI(i64Type, 2, "ch.off");
        off->addIncoming(hitOff, hitBB);
        off->addIncoming(missOff, missBB);
        llvm::Value* adv = nullptr;
        wide = emitCodePointAtOffset(str, off, &adv);
        builder->CreateStore(finalIdx, cacheIdx);
        builder->CreateStore(builder->CreateAdd(off, adv, "ch.cache.next.new"), cacheNext);
        wideEndBB = builder->GetInsertBlock();
    }
    builder->CreateBr(joinBB);

    builder->SetInsertPoint(joinBB);
    auto* cp = builder->CreatePHI(i64Type, 2, "ch.cp");
    cp->addIncoming(narrow, narrowBB);
    cp->addIncoming(wide, wideEndBB);
    return cp;
}

llvm::Value* CodeGen::Impl::emitCodePointStep(llvm::Value* str, llvm::AllocaInst* cursor) {
    auto* off = builder->CreateLoad(i64Type, cursor, "ch.step.off");
    llvm::Value* adv = nullptr;
    llvm::Value* cp = emitCodePointAtOffset(str, off, &adv);
    builder->CreateStore(builder->CreateAdd(off, adv, "ch.step.next"), cursor);
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
    auto* kind = builder->CreateCall(runtimeFuncs["dragon_str_is_ascii"], {str}, "ch.ascii");
    llvm::Value* cp = emitCodePointLoad(str, len, kind, index,
                                        sequentialCacheResetFor(sub->object.get()));
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
