#include "dragon/AST.h"
#include <functional>
#include <set>
#include <sstream>

namespace dragon {

//===----------------------------------------------------------------------===//
// AST utilities
//===----------------------------------------------------------------------===//

std::vector<std::string> instanceFieldOrder(const ClassDecl& cls) {
    std::vector<std::string> order;
    std::set<std::string> seen;
    auto add = [&](const std::string& n) {
        if (seen.insert(n).second) order.push_back(n);
    };
    // 1. Class-body annotations: `x: T` / `x: T = ...` (in source order).
    for (auto& s : cls.body) {
        if (auto* ann = dynamic_cast<AnnAssignStmt*>(s.get()))
            if (auto* nm = dynamic_cast<NameExpr*>(ann->target.get()))
                add(nm->name);
    }
    // 2. `self.X = ...` targets across method bodies (in source order),
    //  descending into nested blocks. A field first assigned in __init__ or
    //  any other method lands here.
    std::function<void(Stmt*)> walk = [&](Stmt* st) {
        if (!st) return;
        auto selfField = [&](Expr* e) {
            if (auto* attr = dynamic_cast<AttributeExpr*>(e))
                if (auto* obj = dynamic_cast<NameExpr*>(attr->object.get()))
                    if (obj->name == "self") add(attr->attribute);
        };
        if (auto* as = dynamic_cast<AssignStmt*>(st)) {
            for (auto& t : as->targets) selfField(t.get());
        } else if (auto* ann = dynamic_cast<AnnAssignStmt*>(st)) {
            selfField(ann->target.get());
        } else if (auto* aug = dynamic_cast<AugAssignStmt*>(st)) {
            selfField(aug->target.get());
        } else if (auto* ifs = dynamic_cast<IfStmt*>(st)) {
            for (auto& s2 : ifs->thenBody) walk(s2.get());
            for (auto& [_, body] : ifs->elifClauses)
                for (auto& s2 : body) walk(s2.get());
            for (auto& s2 : ifs->elseBody) walk(s2.get());
        } else if (auto* ws = dynamic_cast<WhileStmt*>(st)) {
            for (auto& s2 : ws->body) walk(s2.get());
        } else if (auto* fs = dynamic_cast<ForStmt*>(st)) {
            for (auto& s2 : fs->body) walk(s2.get());
        } else if (auto* with = dynamic_cast<WithStmt*>(st)) {
            for (auto& s2 : with->body) walk(s2.get());
        }
    };
    for (auto& s : cls.body) {
        if (auto* fd = dynamic_cast<FunctionDecl*>(s.get()))
            for (auto& bs : fd->body) walk(bs.get());
    }
    return order;
}

static bool stmtAlwaysTerminates(Stmt* s) {
    if (!s) return false;
    if (dynamic_cast<ReturnStmt*>(s) || dynamic_cast<RaiseStmt*>(s) ||
        dynamic_cast<BreakStmt*>(s) || dynamic_cast<ContinueStmt*>(s))
        return true;
    if (auto* ifs = dynamic_cast<IfStmt*>(s)) {
        // An `if` terminates only when control cannot fall through it: there is
        // an `else`, and the then-body, every elif-body, and the else-body all
        // terminate.
        if (ifs->elseBody.empty()) return false;
        if (!stmtsAlwaysTerminate(ifs->thenBody)) return false;
        for (auto& clause : ifs->elifClauses)
            if (!stmtsAlwaysTerminate(clause.second)) return false;
        return stmtsAlwaysTerminate(ifs->elseBody);
    }
    return false;
}

bool stmtsAlwaysTerminate(const std::vector<std::unique_ptr<Stmt>>& stmts) {
    if (stmts.empty()) return false;
    return stmtAlwaysTerminates(stmts.back().get());
}

//===----------------------------------------------------------------------===//
// Type Expression Visitors
//===----------------------------------------------------------------------===//

void NamedTypeExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void GenericTypeExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void OptionalTypeExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void UnionTypeExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void CallableTypeExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void TupleTypeExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }

//===----------------------------------------------------------------------===//
// Expression Visitors
//===----------------------------------------------------------------------===//

void IntegerLiteral::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void FloatLiteral::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void StringLiteral::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void TemplateExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void TemplateFileExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void BooleanLiteral::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void NoneLiteral::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void NameExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void BinaryExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void ChainedCompExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void WalrusExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void UnaryExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void CallExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void AttributeExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void SubscriptExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void SliceExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void ListExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void TupleExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void DictExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void SetExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void ListCompExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void DictCompExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void SetCompExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void GeneratorExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void LambdaExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void IfExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void AwaitExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void FireExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void YieldExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void StarredExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }

//===----------------------------------------------------------------------===//
// Statement Visitors
//===----------------------------------------------------------------------===//

void ExprStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void AssignStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void AugAssignStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void AnnAssignStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void IfStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void WhileStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void ForStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void TryStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void WithStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void ThreadStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void MatchStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void ReturnStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void RaiseStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void BreakStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void ContinueStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void PassStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void AssertStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void GlobalStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void NonlocalStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void DeleteStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void ImportStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void FromImportStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }

//===----------------------------------------------------------------------===//
// Declaration Visitors
//===----------------------------------------------------------------------===//

void FunctionDecl::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void ClassDecl::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void TypeAliasStmt::accept(ASTVisitor& visitor) { visitor.visit(*this); }
void Module::accept(ASTVisitor& visitor) { visitor.visit(*this); }

//===----------------------------------------------------------------------===//
// DefaultASTVisitor -- visits all children
//===----------------------------------------------------------------------===//

// Type expressions
void DefaultASTVisitor::visit(NamedTypeExpr&) {}
void DefaultASTVisitor::visit(GenericTypeExpr& node) {
    if (node.base) node.base->accept(*this);
    for (auto& arg : node.typeArgs) arg->accept(*this);
}
void DefaultASTVisitor::visit(OptionalTypeExpr& node) {
    if (node.inner) node.inner->accept(*this);
}
void DefaultASTVisitor::visit(UnionTypeExpr& node) {
    for (auto& t : node.types) t->accept(*this);
}
void DefaultASTVisitor::visit(CallableTypeExpr& node) {
    for (auto& p : node.paramTypes) p->accept(*this);
    if (node.returnType) node.returnType->accept(*this);
}
void DefaultASTVisitor::visit(TupleTypeExpr& node) {
    for (auto& e : node.elementTypes) e->accept(*this);
}

// Literals (no children)
void DefaultASTVisitor::visit(IntegerLiteral&) {}
void DefaultASTVisitor::visit(FloatLiteral&) {}
void DefaultASTVisitor::visit(StringLiteral&) {}
void DefaultASTVisitor::visit(TemplateExpr&) {}
void DefaultASTVisitor::visit(TemplateFileExpr&) {}
void DefaultASTVisitor::visit(BooleanLiteral&) {}
void DefaultASTVisitor::visit(NoneLiteral&) {}
void DefaultASTVisitor::visit(NameExpr&) {}

// Compound expressions
void DefaultASTVisitor::visit(BinaryExpr& node) {
    node.left->accept(*this);
    node.right->accept(*this);
}
void DefaultASTVisitor::visit(ChainedCompExpr& node) {
    for (auto& op : node.operands) op->accept(*this);
}
void DefaultASTVisitor::visit(WalrusExpr& node) {
    node.value->accept(*this);
}
void DefaultASTVisitor::visit(UnaryExpr& node) {
    node.operand->accept(*this);
}
void DefaultASTVisitor::visit(CallExpr& node) {
    node.callee->accept(*this);
    for (auto& arg : node.args) arg->accept(*this);
    for (auto& [name, val] : node.kwArgs) val->accept(*this);
}
void DefaultASTVisitor::visit(AttributeExpr& node) {
    node.object->accept(*this);
}
void DefaultASTVisitor::visit(SubscriptExpr& node) {
    node.object->accept(*this);
    node.index->accept(*this);
}
void DefaultASTVisitor::visit(SliceExpr& node) {
    if (node.lower) node.lower->accept(*this);
    if (node.upper) node.upper->accept(*this);
    if (node.step) node.step->accept(*this);
}
void DefaultASTVisitor::visit(ListExpr& node) {
    for (auto& e : node.elements) e->accept(*this);
}
void DefaultASTVisitor::visit(TupleExpr& node) {
    for (auto& e : node.elements) e->accept(*this);
}
void DefaultASTVisitor::visit(DictExpr& node) {
    for (auto& [key, val] : node.entries) {
        if (key) key->accept(*this);
        if (val) val->accept(*this);
    }
}
void DefaultASTVisitor::visit(SetExpr& node) {
    for (auto& e : node.elements) e->accept(*this);
}
void DefaultASTVisitor::visit(ListCompExpr& node) {
    node.element->accept(*this);
    node.iterable->accept(*this);
    if (node.condition) node.condition->accept(*this);
    for (auto& clause : node.extraClauses) {
        clause.iterable->accept(*this);
        if (clause.condition) clause.condition->accept(*this);
    }
}
void DefaultASTVisitor::visit(DictCompExpr& node) {
    node.key->accept(*this);
    node.value->accept(*this);
    node.iterable->accept(*this);
    if (node.condition) node.condition->accept(*this);
    for (auto& clause : node.extraClauses) {
        clause.iterable->accept(*this);
        if (clause.condition) clause.condition->accept(*this);
    }
}
void DefaultASTVisitor::visit(SetCompExpr& node) {
    node.element->accept(*this);
    node.iterable->accept(*this);
    if (node.condition) node.condition->accept(*this);
    for (auto& clause : node.extraClauses) {
        clause.iterable->accept(*this);
        if (clause.condition) clause.condition->accept(*this);
    }
}
void DefaultASTVisitor::visit(GeneratorExpr& node) {
    node.element->accept(*this);
    node.iterable->accept(*this);
    if (node.condition) node.condition->accept(*this);
    for (auto& clause : node.extraClauses) {
        clause.iterable->accept(*this);
        if (clause.condition) clause.condition->accept(*this);
    }
}
void DefaultASTVisitor::visit(LambdaExpr& node) {
    for (auto& p : node.params) {
        if (p.type) p.type->accept(*this);
        if (p.defaultValue) p.defaultValue->accept(*this);
    }
    if (node.returnType) node.returnType->accept(*this);
    if (node.body) node.body->accept(*this);
    for (auto& s : node.bodyStmts) s->accept(*this);
}
void DefaultASTVisitor::visit(IfExpr& node) {
    node.condition->accept(*this);
    node.thenExpr->accept(*this);
    node.elseExpr->accept(*this);
}
void DefaultASTVisitor::visit(AwaitExpr& node) {
    node.operand->accept(*this);
}
void DefaultASTVisitor::visit(FireExpr& node) {
    if (node.operand) node.operand->accept(*this);
    for (auto& s : node.bodyStmts) s->accept(*this);
}
void DefaultASTVisitor::visit(YieldExpr& node) {
    if (node.value) node.value->accept(*this);
}
void DefaultASTVisitor::visit(StarredExpr& node) {
    node.value->accept(*this);
}

// Statements
void DefaultASTVisitor::visit(ExprStmt& node) {
    node.expr->accept(*this);
}
void DefaultASTVisitor::visit(AssignStmt& node) {
    for (auto& t : node.targets) t->accept(*this);
    if (node.typeAnnotation) node.typeAnnotation->accept(*this);
    node.value->accept(*this);
}
void DefaultASTVisitor::visit(AugAssignStmt& node) {
    node.target->accept(*this);
    node.value->accept(*this);
}
void DefaultASTVisitor::visit(AnnAssignStmt& node) {
    node.target->accept(*this);
    if (node.annotation) node.annotation->accept(*this);
    if (node.value) node.value->accept(*this);
}
void DefaultASTVisitor::visit(IfStmt& node) {
    node.condition->accept(*this);
    for (auto& s : node.thenBody) s->accept(*this);
    for (auto& [cond, body] : node.elifClauses) {
        cond->accept(*this);
        for (auto& s : body) s->accept(*this);
    }
    for (auto& s : node.elseBody) s->accept(*this);
}
void DefaultASTVisitor::visit(WhileStmt& node) {
    node.condition->accept(*this);
    for (auto& s : node.body) s->accept(*this);
    for (auto& s : node.elseBody) s->accept(*this);
}
void DefaultASTVisitor::visit(ForStmt& node) {
    node.target->accept(*this);
    node.iterable->accept(*this);
    for (auto& s : node.body) s->accept(*this);
    for (auto& s : node.elseBody) s->accept(*this);
}
void DefaultASTVisitor::visit(TryStmt& node) {
    for (auto& s : node.tryBody) s->accept(*this);
    for (auto& handler : node.handlers) {
        if (handler.type) handler.type->accept(*this);
        for (auto& s : handler.body) s->accept(*this);
    }
    for (auto& s : node.elseBody) s->accept(*this);
    for (auto& s : node.finallyBody) s->accept(*this);
}
void DefaultASTVisitor::visit(WithStmt& node) {
    for (auto& item : node.items) {
        item.contextExpr->accept(*this);
        if (item.optionalVars) item.optionalVars->accept(*this);
    }
    for (auto& s : node.body) s->accept(*this);
}
void DefaultASTVisitor::visit(ThreadStmt& node) {
    for (auto& s : node.body) s->accept(*this);
}
void DefaultASTVisitor::visit(MatchStmt& node) {
    node.subject->accept(*this);
    // Recursively visit all expressions inside a pattern tree
    std::function<void(MatchPattern&)> visitPattern = [&](MatchPattern& pat) {
        if (pat.literal) pat.literal->accept(*this);
        if (pat.guard) pat.guard->accept(*this);
        for (auto& sub : pat.subPatterns) visitPattern(sub);
    };
    for (auto& c : node.cases) {
        visitPattern(c.pattern);
        if (c.guard) c.guard->accept(*this);
        for (auto& s : c.body) s->accept(*this);
    }
}
void DefaultASTVisitor::visit(ReturnStmt& node) {
    if (node.value) node.value->accept(*this);
}
void DefaultASTVisitor::visit(RaiseStmt& node) {
    if (node.exception) node.exception->accept(*this);
    if (node.cause) node.cause->accept(*this);
}
void DefaultASTVisitor::visit(BreakStmt&) {}
void DefaultASTVisitor::visit(ContinueStmt&) {}
void DefaultASTVisitor::visit(PassStmt&) {}
void DefaultASTVisitor::visit(AssertStmt& node) {
    node.test->accept(*this);
    if (node.msg) node.msg->accept(*this);
}
void DefaultASTVisitor::visit(GlobalStmt&) {}
void DefaultASTVisitor::visit(NonlocalStmt&) {}
void DefaultASTVisitor::visit(DeleteStmt& node) {
    for (auto& t : node.targets) t->accept(*this);
}
void DefaultASTVisitor::visit(ImportStmt&) {}
void DefaultASTVisitor::visit(FromImportStmt&) {}

// Declarations
void DefaultASTVisitor::visit(FunctionDecl& node) {
    for (auto& dec : node.decorators) dec->accept(*this);
    for (auto& p : node.params) {
        if (p.type) p.type->accept(*this);
        if (p.defaultValue) p.defaultValue->accept(*this);
    }
    if (node.returnType) node.returnType->accept(*this);
    for (auto& s : node.body) s->accept(*this);
}
void DefaultASTVisitor::visit(ClassDecl& node) {
    for (auto& dec : node.decorators) dec->accept(*this);
    for (auto& b : node.bases) b->accept(*this);
    for (auto& [name, val] : node.keywords) val->accept(*this);
    for (auto& s : node.body) s->accept(*this);
}
void DefaultASTVisitor::visit(TypeAliasStmt& node) {
    if (node.value) node.value->accept(*this);
}
void DefaultASTVisitor::visit(Module& node) {
    for (auto& s : node.body) s->accept(*this);
}

//===----------------------------------------------------------------------===//
// AST Printer
//===----------------------------------------------------------------------===//

void ASTPrinter::write(const std::string& text) {
    output_ += text;
}

void ASTPrinter::writeLine(const std::string& text) {
    output_ += indentStr() + text + "\n";
}

void ASTPrinter::increaseIndent() { indent_ += 2; }
void ASTPrinter::decreaseIndent() { indent_ -= 2; }

std::string ASTPrinter::indentStr() const {
    return std::string(indent_, ' ');
}

void ASTPrinter::printPattern(MatchPattern& pat) {
    switch (pat.kind) {
        case MatchPattern::Kind::Literal:
            writeLine("(literal");
            increaseIndent();
            if (pat.literal) pat.literal->accept(*this);
            decreaseIndent();
            writeLine(")");
            break;
        case MatchPattern::Kind::Capture:
            writeLine("(capture " + pat.name + ")");
            break;
        case MatchPattern::Kind::Wildcard:
            writeLine("(wildcard)");
            break;
        case MatchPattern::Kind::Sequence:
            writeLine("(sequence");
            increaseIndent();
            for (auto& sub : pat.subPatterns) printPattern(sub);
            decreaseIndent();
            writeLine(")");
            break;
        case MatchPattern::Kind::Or:
            writeLine("(or");
            increaseIndent();
            for (auto& sub : pat.subPatterns) printPattern(sub);
            decreaseIndent();
            writeLine(")");
            break;
        case MatchPattern::Kind::Value:
            writeLine("(value");
            increaseIndent();
            if (pat.literal) pat.literal->accept(*this);
            decreaseIndent();
            writeLine(")");
            break;
        default:
            writeLine("(pattern)");
            break;
    }
}

std::string ASTPrinter::print(ASTNode& node) {
    output_.clear();
    indent_ = 0;
    node.accept(*this);
    return output_;
}

// --- Type Expressions ---

void ASTPrinter::visit(NamedTypeExpr& node) {
    write(node.name);
}

void ASTPrinter::visit(GenericTypeExpr& node) {
    node.base->accept(*this);
    write("[");
    for (size_t i = 0; i < node.typeArgs.size(); i++) {
        if (i > 0) write(", ");
        node.typeArgs[i]->accept(*this);
    }
    write("]");
}

void ASTPrinter::visit(OptionalTypeExpr& node) {
    node.inner->accept(*this);
    write("?");
}

void ASTPrinter::visit(UnionTypeExpr& node) {
    for (size_t i = 0; i < node.types.size(); i++) {
        if (i > 0) write(" | ");
        node.types[i]->accept(*this);
    }
}

void ASTPrinter::visit(CallableTypeExpr& node) {
    write("Callable[[");
    for (size_t i = 0; i < node.paramTypes.size(); i++) {
        if (i > 0) write(", ");
        node.paramTypes[i]->accept(*this);
    }
    write("], ");
    if (node.returnType) node.returnType->accept(*this);
    else write("None");
    write("]");
}

void ASTPrinter::visit(TupleTypeExpr& node) {
    write("tuple[");
    for (size_t i = 0; i < node.elementTypes.size(); i++) {
        if (i > 0) write(", ");
        node.elementTypes[i]->accept(*this);
    }
    write("]");
}

// --- Expressions ---

void ASTPrinter::visit(IntegerLiteral& node) {
    writeLine("(int " + std::to_string(node.value) + ")");
}

void ASTPrinter::visit(FloatLiteral& node) {
    writeLine("(float " + std::to_string(node.value) + ")");
}

void ASTPrinter::visit(StringLiteral& node) {
    std::string prefix;
    if (node.isFString) prefix += "f";
    if (node.isRaw) prefix += "r";
    if (node.isBytes) prefix += "b";
    writeLine("(str " + prefix + "\"" + node.value + "\")");
}

void ASTPrinter::visit(TemplateExpr& node) {
    writeLine("(template {" + node.body + "})");
}

void ASTPrinter::visit(TemplateFileExpr& node) {
    writeLine("(template-file \"" + node.filePath + "\")");
}

void ASTPrinter::visit(BooleanLiteral& node) {
    writeLine(node.value ? "(bool True)" : "(bool False)");
}

void ASTPrinter::visit(NoneLiteral&) {
    writeLine("(None)");
}

void ASTPrinter::visit(NameExpr& node) {
    writeLine("(name " + node.name + ")");
}

void ASTPrinter::visit(BinaryExpr& node) {
    writeLine("(binary " + std::string(node.op.lexeme()) + ")");
    increaseIndent();
    node.left->accept(*this);
    node.right->accept(*this);
    decreaseIndent();
}

void ASTPrinter::visit(ChainedCompExpr& node) {
    std::string ops;
    for (size_t i = 0; i < node.operators.size(); i++) {
        if (i > 0) ops += " ";
        ops += std::string(node.operators[i].lexeme());
    }
    writeLine("(chainedcomp " + ops + ")");
    increaseIndent();
    for (auto& op : node.operands) op->accept(*this);
    decreaseIndent();
}

void ASTPrinter::visit(WalrusExpr& node) {
    writeLine("(walrus " + node.name + ")");
    increaseIndent();
    node.value->accept(*this);
    decreaseIndent();
}

void ASTPrinter::visit(UnaryExpr& node) {
    writeLine("(unary " + std::string(node.op.lexeme()) + ")");
    increaseIndent();
    node.operand->accept(*this);
    decreaseIndent();
}

void ASTPrinter::visit(CallExpr& node) {
    writeLine("(call");
    increaseIndent();
    node.callee->accept(*this);
    for (auto& arg : node.args) {
        arg->accept(*this);
    }
    for (auto& [name, val] : node.kwArgs) {
        writeLine("(kwarg " + name + ")");
        increaseIndent();
        val->accept(*this);
        decreaseIndent();
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(AttributeExpr& node) {
    writeLine("(attribute ." + node.attribute + ")");
    increaseIndent();
    node.object->accept(*this);
    decreaseIndent();
}

void ASTPrinter::visit(SubscriptExpr& node) {
    writeLine("(subscript");
    increaseIndent();
    node.object->accept(*this);
    node.index->accept(*this);
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(SliceExpr& node) {
    writeLine("(slice");
    increaseIndent();
    if (node.lower) node.lower->accept(*this);
    else writeLine("(none)");
    if (node.upper) node.upper->accept(*this);
    else writeLine("(none)");
    if (node.step) node.step->accept(*this);
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(ListExpr& node) {
    writeLine("(list");
    increaseIndent();
    for (auto& e : node.elements) e->accept(*this);
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(TupleExpr& node) {
    writeLine("(tuple");
    increaseIndent();
    for (auto& e : node.elements) e->accept(*this);
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(DictExpr& node) {
    writeLine("(dict");
    increaseIndent();
    for (auto& [key, val] : node.entries) {
        if (!key) {
            writeLine("(spread");
            increaseIndent();
            if (val) val->accept(*this);
            decreaseIndent();
            writeLine(")");
        } else {
            writeLine("(entry");
            increaseIndent();
            key->accept(*this);
            val->accept(*this);
            decreaseIndent();
            writeLine(")");
        }
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(SetExpr& node) {
    writeLine("(set");
    increaseIndent();
    for (auto& e : node.elements) e->accept(*this);
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(ListCompExpr& node) {
    writeLine("(listcomp");
    increaseIndent();
    writeLine("(element");
    increaseIndent();
    node.element->accept(*this);
    decreaseIndent();
    writeLine(")");
    writeLine("(for " + node.varName + " in");
    increaseIndent();
    node.iterable->accept(*this);
    decreaseIndent();
    writeLine(")");
    if (node.condition) {
        writeLine("(if");
        increaseIndent();
        node.condition->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(DictCompExpr& node) {
    writeLine("(dictcomp");
    increaseIndent();
    node.key->accept(*this);
    node.value->accept(*this);
    std::string vars;
    for (size_t i = 0; i < node.varNames.size(); i++) {
        if (i > 0) vars += ", ";
        vars += node.varNames[i];
    }
    writeLine("(for " + vars + " in");
    increaseIndent();
    node.iterable->accept(*this);
    decreaseIndent();
    writeLine(")");
    if (node.condition) {
        writeLine("(if");
        increaseIndent();
        node.condition->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(SetCompExpr& node) {
    writeLine("(setcomp");
    increaseIndent();
    writeLine("(element");
    increaseIndent();
    node.element->accept(*this);
    decreaseIndent();
    writeLine(")");
    writeLine("(for " + node.varName + " in");
    increaseIndent();
    node.iterable->accept(*this);
    decreaseIndent();
    writeLine(")");
    if (node.condition) {
        writeLine("(if");
        increaseIndent();
        node.condition->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(GeneratorExpr& node) {
    writeLine("(genexpr");
    increaseIndent();
    writeLine("(element");
    increaseIndent();
    node.element->accept(*this);
    decreaseIndent();
    writeLine(")");
    writeLine("(for " + node.varName + " in");
    increaseIndent();
    node.iterable->accept(*this);
    decreaseIndent();
    writeLine(")");
    if (node.condition) {
        writeLine("(if");
        increaseIndent();
        node.condition->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(LambdaExpr& node) {
    writeLine("(lambda");
    increaseIndent();
    writeLine("(params");
    increaseIndent();
    for (auto& p : node.params) {
        std::string param = p.name;
        if (p.type) {
            param += ": ";
            // Inline type printing
            std::string savedOutput = output_;
            int savedIndent = indent_;
            output_.clear();
            indent_ = 0;
            p.type->accept(*this);
            std::string typeStr = output_;
            // Remove any trailing whitespace/newline
            while (!typeStr.empty() && (typeStr.back() == '\n' || typeStr.back() == ' '))
                typeStr.pop_back();
            output_ = savedOutput;
            indent_ = savedIndent;
            param += typeStr;
        }
        writeLine(param);
    }
    decreaseIndent();
    writeLine(")");
    if (node.body) {
        node.body->accept(*this);
    }
    for (auto& s : node.bodyStmts) {
        s->accept(*this);
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(IfExpr& node) {
    writeLine("(if-expr");
    increaseIndent();
    writeLine("(then");
    increaseIndent();
    node.thenExpr->accept(*this);
    decreaseIndent();
    writeLine(")");
    writeLine("(cond");
    increaseIndent();
    node.condition->accept(*this);
    decreaseIndent();
    writeLine(")");
    writeLine("(else");
    increaseIndent();
    node.elseExpr->accept(*this);
    decreaseIndent();
    writeLine(")");
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(AwaitExpr& node) {
    writeLine("(await");
    increaseIndent();
    node.operand->accept(*this);
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(FireExpr& node) {
    writeLine("(fire");
    increaseIndent();
    if (node.operand) {
        node.operand->accept(*this);
    }
    for (auto& s : node.bodyStmts) s->accept(*this);
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(YieldExpr& node) {
    if (node.isYieldFrom)
        writeLine("(yield-from");
    else
        writeLine("(yield");
    if (node.value) {
        increaseIndent();
        node.value->accept(*this);
        decreaseIndent();
    }
    writeLine(")");
}

void ASTPrinter::visit(StarredExpr& node) {
    writeLine(node.isDoubleStar ? "(starred **" : "(starred *");
    increaseIndent();
    node.value->accept(*this);
    decreaseIndent();
    writeLine(")");
}

// --- Statements ---

void ASTPrinter::visit(ExprStmt& node) {
    writeLine("(expr-stmt");
    increaseIndent();
    node.expr->accept(*this);
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(AssignStmt& node) {
    writeLine("(assign");
    increaseIndent();
    writeLine("(targets");
    increaseIndent();
    for (auto& t : node.targets) t->accept(*this);
    decreaseIndent();
    writeLine(")");
    if (node.typeAnnotation) {
        writeLine("(type ");
        increaseIndent();
        node.typeAnnotation->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    writeLine("(value");
    increaseIndent();
    node.value->accept(*this);
    decreaseIndent();
    writeLine(")");
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(AugAssignStmt& node) {
    writeLine("(aug-assign " + std::string(node.op.lexeme()) + ")");
    increaseIndent();
    node.target->accept(*this);
    node.value->accept(*this);
    decreaseIndent();
}

void ASTPrinter::visit(AnnAssignStmt& node) {
    writeLine("(ann-assign");
    increaseIndent();
    node.target->accept(*this);
    if (node.annotation) {
        write(indentStr() + "(type: ");
        node.annotation->accept(*this);
        write(")\n");
    }
    if (node.value) {
        writeLine("(value");
        increaseIndent();
        node.value->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(IfStmt& node) {
    writeLine("(if");
    increaseIndent();
    writeLine("(cond");
    increaseIndent();
    node.condition->accept(*this);
    decreaseIndent();
    writeLine(")");
    writeLine("(then");
    increaseIndent();
    for (auto& s : node.thenBody) s->accept(*this);
    decreaseIndent();
    writeLine(")");
    for (auto& [cond, body] : node.elifClauses) {
        writeLine("(elif");
        increaseIndent();
        writeLine("(cond");
        increaseIndent();
        cond->accept(*this);
        decreaseIndent();
        writeLine(")");
        writeLine("(then");
        increaseIndent();
        for (auto& s : body) s->accept(*this);
        decreaseIndent();
        writeLine(")");
        decreaseIndent();
        writeLine(")");
    }
    if (!node.elseBody.empty()) {
        writeLine("(else");
        increaseIndent();
        for (auto& s : node.elseBody) s->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(WhileStmt& node) {
    writeLine("(while");
    increaseIndent();
    writeLine("(cond");
    increaseIndent();
    node.condition->accept(*this);
    decreaseIndent();
    writeLine(")");
    writeLine("(body");
    increaseIndent();
    for (auto& s : node.body) s->accept(*this);
    decreaseIndent();
    writeLine(")");
    if (!node.elseBody.empty()) {
        writeLine("(else");
        increaseIndent();
        for (auto& s : node.elseBody) s->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(ForStmt& node) {
    writeLine("(for");
    increaseIndent();
    writeLine("(target");
    increaseIndent();
    node.target->accept(*this);
    decreaseIndent();
    writeLine(")");
    writeLine("(iter");
    increaseIndent();
    node.iterable->accept(*this);
    decreaseIndent();
    writeLine(")");
    writeLine("(body");
    increaseIndent();
    for (auto& s : node.body) s->accept(*this);
    decreaseIndent();
    writeLine(")");
    if (!node.elseBody.empty()) {
        writeLine("(else");
        increaseIndent();
        for (auto& s : node.elseBody) s->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(TryStmt& node) {
    writeLine("(try");
    increaseIndent();
    writeLine("(body");
    increaseIndent();
    for (auto& s : node.tryBody) s->accept(*this);
    decreaseIndent();
    writeLine(")");
    for (auto& handler : node.handlers) {
        std::string label = "(catch";
        if (handler.type) {
            // Inline type to string
            label += " ";
        }
        if (!handler.name.empty()) {
            label += " as " + handler.name;
        }
        writeLine(label);
        increaseIndent();
        if (handler.type) {
            handler.type->accept(*this);
        }
        for (auto& s : handler.body) s->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    if (!node.elseBody.empty()) {
        writeLine("(else");
        increaseIndent();
        for (auto& s : node.elseBody) s->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    if (!node.finallyBody.empty()) {
        writeLine("(finally");
        increaseIndent();
        for (auto& s : node.finallyBody) s->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(ThreadStmt& node) {
    writeLine("(thread");
    increaseIndent();
    for (auto& s : node.body) s->accept(*this);
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(WithStmt& node) {
    writeLine("(with");
    increaseIndent();
    for (auto& item : node.items) {
        writeLine("(item");
        increaseIndent();
        item.contextExpr->accept(*this);
        if (item.optionalVars) {
            writeLine("(as");
            increaseIndent();
            item.optionalVars->accept(*this);
            decreaseIndent();
            writeLine(")");
        }
        decreaseIndent();
        writeLine(")");
    }
    writeLine("(body");
    increaseIndent();
    for (auto& s : node.body) s->accept(*this);
    decreaseIndent();
    writeLine(")");
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(MatchStmt& node) {
    writeLine("(match");
    increaseIndent();
    node.subject->accept(*this);
    for (auto& c : node.cases) {
        writeLine("(case");
        increaseIndent();
        printPattern(c.pattern);
        if (c.guard) {
            writeLine("(guard");
            increaseIndent();
            c.guard->accept(*this);
            decreaseIndent();
            writeLine(")");
        }
        writeLine("(body");
        increaseIndent();
        for (auto& s : c.body) s->accept(*this);
        decreaseIndent();
        writeLine(")");
        decreaseIndent();
        writeLine(")");
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(ReturnStmt& node) {
    if (node.value) {
        writeLine("(return");
        increaseIndent();
        node.value->accept(*this);
        decreaseIndent();
        writeLine(")");
    } else {
        writeLine("(return)");
    }
}

void ASTPrinter::visit(RaiseStmt& node) {
    if (node.exception) {
        writeLine("(raise");
        increaseIndent();
        node.exception->accept(*this);
        if (node.cause) {
            writeLine("(from");
            increaseIndent();
            node.cause->accept(*this);
            decreaseIndent();
            writeLine(")");
        }
        decreaseIndent();
        writeLine(")");
    } else {
        writeLine("(raise)");
    }
}

void ASTPrinter::visit(BreakStmt&) { writeLine("(break)"); }
void ASTPrinter::visit(ContinueStmt&) { writeLine("(continue)"); }
void ASTPrinter::visit(PassStmt&) { writeLine("(pass)"); }

void ASTPrinter::visit(AssertStmt& node) {
    writeLine("(assert");
    increaseIndent();
    node.test->accept(*this);
    if (node.msg) {
        writeLine("(msg");
        increaseIndent();
        node.msg->accept(*this);
        decreaseIndent();
        writeLine(")");
    }
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(GlobalStmt& node) {
    std::string names;
    for (size_t i = 0; i < node.names.size(); i++) {
        if (i > 0) names += ", ";
        names += node.names[i];
    }
    writeLine("(global " + names + ")");
}

void ASTPrinter::visit(NonlocalStmt& node) {
    std::string names;
    for (size_t i = 0; i < node.names.size(); i++) {
        if (i > 0) names += ", ";
        names += node.names[i];
    }
    writeLine("(nonlocal " + names + ")");
}

void ASTPrinter::visit(DeleteStmt& node) {
    writeLine("(del");
    increaseIndent();
    for (auto& t : node.targets) t->accept(*this);
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(ImportStmt& node) {
    std::string line = "(import";
    for (auto& alias : node.names) {
        line += " " + alias.name;
        if (!alias.asName.empty()) line += " as " + alias.asName;
    }
    line += ")";
    writeLine(line);
}

void ASTPrinter::visit(FromImportStmt& node) {
    std::string line = "(from " + node.module + " import";
    for (size_t i = 0; i < node.names.size(); i++) {
        line += " " + node.names[i].name;
        if (!node.names[i].asName.empty()) line += " as " + node.names[i].asName;
        if (i + 1 < node.names.size()) line += ",";
    }
    line += ")";
    writeLine(line);
}

// --- Declarations ---

void ASTPrinter::visit(FunctionDecl& node) {
    std::string header = "(def ";
    if (node.isAsync) header += "async ";
    header += node.name;
    writeLine(header);
    increaseIndent();

    if (!node.decorators.empty()) {
        writeLine("(decorators");
        increaseIndent();
        for (auto& dec : node.decorators) dec->accept(*this);
        decreaseIndent();
        writeLine(")");
    }

    writeLine("(params");
    increaseIndent();
    for (auto& p : node.params) {
        std::string param;
        if (p.isVarArg) param += "*";
        if (p.isKwArg) param += "**";
        param += p.name;
        writeLine(param);
    }
    decreaseIndent();
    writeLine(")");

    if (node.returnType) {
        write(indentStr() + "(returns: ");
        node.returnType->accept(*this);
        write(")\n");
    }

    if (node.docstring)
        writeLine("(doc " + *node.docstring + ")");

    writeLine("(body");
    increaseIndent();
    for (auto& s : node.body) s->accept(*this);
    decreaseIndent();
    writeLine(")");

    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(ClassDecl& node) {
    writeLine("(class " + node.name);
    increaseIndent();

    if (!node.decorators.empty()) {
        writeLine("(decorators");
        increaseIndent();
        for (auto& dec : node.decorators) dec->accept(*this);
        decreaseIndent();
        writeLine(")");
    }

    if (!node.bases.empty()) {
        writeLine("(bases");
        increaseIndent();
        for (auto& b : node.bases) b->accept(*this);
        decreaseIndent();
        writeLine(")");
    }

    if (node.docstring)
        writeLine("(doc " + *node.docstring + ")");

    writeLine("(body");
    increaseIndent();
    for (auto& s : node.body) s->accept(*this);
    decreaseIndent();
    writeLine(")");

    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(TypeAliasStmt& node) {
    writeLine("(type_alias " + node.name);
    increaseIndent();
    if (node.value) node.value->accept(*this);
    decreaseIndent();
    writeLine(")");
}

void ASTPrinter::visit(Module& node) {
    writeLine("(module " + node.filename);
    increaseIndent();
    if (node.docstring)
        writeLine("(doc " + *node.docstring + ")");
    for (auto& s : node.body) s->accept(*this);
    decreaseIndent();
    writeLine(")");
}

} // namespace dragon
