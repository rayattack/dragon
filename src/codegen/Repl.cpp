#include "../CodeGenImpl.h"

namespace dragon {

namespace {

bool isEchoableKind(Type::Kind kind) {
    switch (kind) {
        case Type::Kind::None_:
        case Type::Kind::Never:
        case Type::Kind::Unknown:
            return false;
        default:
            return true;
    }
}

bool returnsNothing(Expr* expr) {
    auto* call = dynamic_cast<CallExpr*>(expr);
    if (!call || !call->type) return false;
    return !isEchoableKind(call->type->kind());
}

}

bool CodeGen::emitReplEcho(ExprStmt& node) {
    if (!impl_->options.replMode || impl_->options.replTeardown) return false;
    if (impl_->currentFunction != impl_->mainFunction) return false;
    if (impl_->scopes.size() > impl_->moduleBodyScopeDepth) return false;

    Expr* expr = node.expr.get();
    if (!expr || !expr->type) return false;
    if (dynamic_cast<NoneLiteral*>(expr)) return false;
    if (dynamic_cast<FireExpr*>(expr)) return false;
    if (dynamic_cast<AwaitExpr*>(expr)) return false;
    if (!isEchoableKind(expr->type->kind())) return false;
    if (returnsNothing(expr)) return false;

    const Type::Kind kind = expr->type->kind();
    if (kind == Type::Kind::Str || kind == Type::Kind::Bytes) {
        expr->accept(*this);
        llvm::Value* value = impl_->lastValue;
        if (!value) return false;

        auto* quoted = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_repr_str"], {value}, "echo.repr");
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_print_str_raw"], {quoted});
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_decref_str"], {quoted});

        if (impl_->options.gcMode == GCMode::RC && impl_->isOwnedStrResult(value))
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_decref_str"], {value});
    } else {
        emitPrintArgRaw(expr);
    }

    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_print_newline"], {});
    return true;
}

}
