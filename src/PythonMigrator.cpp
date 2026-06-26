#include "dragon/PythonMigrator.h"
#include "dragon/Lexer.h"
#include "dragon/Parser.h"
#include <fstream>
#include <sstream>

namespace dragon {

struct PythonMigrator::Impl {
    MigrationOptions options;
    std::vector<MigrationDiagnostic> diagnostics;
    std::vector<std::string> incompatibilities;
    TypeInference typeInference;
};

PythonMigrator::PythonMigrator(MigrationOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = options;
}

PythonMigrator::~PythonMigrator() = default;

bool PythonMigrator::migrate(const std::string& inputFile,
                             const std::string& outputFile) {
    std::ifstream in(inputFile);
    if (!in) {
        impl_->diagnostics.push_back({
            MigrationDiagnostic::Level::Error, {},
            "Cannot open file: " + inputFile
        });
        return false;
    }

    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string source = buffer.str();

    std::string result = migrateSource(source);
    if (hasErrors()) return false;

    std::ofstream out(outputFile);
    if (!out) {
        impl_->diagnostics.push_back({
            MigrationDiagnostic::Level::Error, {},
            "Cannot write file: " + outputFile
        });
        return false;
    }
    out << result;
    return true;
}

std::string PythonMigrator::migrateSource(const std::string& source) {
    // Parse as Python (indent mode)
    LexerOptions lexOpts;
    lexOpts.useBraceBlocks = false;
    Lexer lexer(source, lexOpts);
    auto tokens = lexer.tokenize();

    ParserOptions parseOpts;
    parseOpts.isDragonFile = false;
    parseOpts.requireTypes = false;
    Parser parser(std::move(tokens), parseOpts);
    auto module = parser.parseModule();

    if (parser.hasErrors()) {
        for (const auto& diag : parser.diagnostics()) {
            impl_->diagnostics.push_back({
                MigrationDiagnostic::Level::Error,
                diag.location, diag.message
            });
        }
        return "";
    }

    migrateModule(*module);

    return emitDragon(*module);
}

bool PythonMigrator::migrateModule(Module& module) {
    if (impl_->options.addTypes) {
        addTypeAnnotations(module);
    }
    validateDragonCompatibility(module);
    return !hasErrors();
}

const std::vector<MigrationDiagnostic>& PythonMigrator::diagnostics() const {
    return impl_->diagnostics;
}

bool PythonMigrator::hasErrors() const {
    for (const auto& d : impl_->diagnostics) {
        if (d.level == MigrationDiagnostic::Level::Error) return true;
    }
    return false;
}

std::vector<std::string> PythonMigrator::incompatibilities() const {
    return impl_->incompatibilities;
}

void PythonMigrator::addTypeAnnotations(Module& module) {
    impl_->typeInference.infer(module);
}

void PythonMigrator::convertBlocksToBraces(Module&) {
    // Block conversion is handled at emission time, not AST level
}

void PythonMigrator::validateDragonCompatibility(Module& module) {
    for (auto& stmt : module.body) {
        if (dynamic_cast<WithStmt*>(stmt.get())) {
            impl_->diagnostics.push_back({
                MigrationDiagnostic::Level::Warning,
                stmt->location(),
                "with statement: context managers may need manual adaptation"
            });
        }
    }
}

//===----------------------------------------------------------------------===//
// Dragon Code Emission
//===----------------------------------------------------------------------===//

static std::string ind(int level) {
    return std::string(level * 4, ' ');
}

std::string PythonMigrator::emitDragon(Module& module) {
    std::string out;
    for (size_t i = 0; i < module.body.size(); ++i) {
        out += emitStmt(module.body[i].get(), 0);
        if (i + 1 < module.body.size()) out += "\n";
    }
    return out;
}

std::string PythonMigrator::emitExpr(Expr* expr) {
    if (!expr) return "";

    if (auto* lit = dynamic_cast<IntegerLiteral*>(expr)) {
        return std::to_string(lit->value);
    }
    if (auto* lit = dynamic_cast<FloatLiteral*>(expr)) {
        std::ostringstream ss;
        ss << lit->value;
        std::string s = ss.str();
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
        return s;
    }
    if (auto* lit = dynamic_cast<StringLiteral*>(expr)) {
        std::string prefix;
        if (lit->isFString) prefix = "f";
        if (lit->isRaw) prefix = "r";
        if (lit->isBytes) prefix = "b";
        return prefix + "\"" + lit->value + "\"";
    }
    if (auto* lit = dynamic_cast<BooleanLiteral*>(expr)) {
        return lit->value ? "True" : "False";
    }
    if (dynamic_cast<NoneLiteral*>(expr)) {
        return "None";
    }
    if (auto* name = dynamic_cast<NameExpr*>(expr)) {
        return name->name;
    }
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        std::string op;
        switch (bin->op.type()) {
            case TokenType::PLUS: op = " + "; break;
            case TokenType::MINUS: op = " - "; break;
            case TokenType::STAR: op = " * "; break;
            case TokenType::SLASH: op = " / "; break;
            case TokenType::PERCENT: op = " % "; break;
            case TokenType::POWER: op = " ** "; break;
            case TokenType::DOUBLE_SLASH: op = " // "; break;
            case TokenType::EQUAL_EQUAL: op = " == "; break;
            case TokenType::NOT_EQUAL: op = " != "; break;
            case TokenType::LESS: op = " < "; break;
            case TokenType::LESS_EQUAL: op = " <= "; break;
            case TokenType::GREATER: op = " > "; break;
            case TokenType::GREATER_EQUAL: op = " >= "; break;
            case TokenType::AND: op = " and "; break;
            case TokenType::OR: op = " or "; break;
            case TokenType::AMPERSAND: op = " & "; break;
            case TokenType::PIPE: op = " | "; break;
            case TokenType::CARET: op = " ^ "; break;
            case TokenType::LEFT_SHIFT: op = " << "; break;
            case TokenType::RIGHT_SHIFT: op = " >> "; break;
            case TokenType::IN: op = " in "; break;
            case TokenType::IS: op = " is "; break;
            default: op = " " + bin->op.lexeme() + " "; break;
        }
        return emitExpr(bin->left.get()) + op + emitExpr(bin->right.get());
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
        if (unary->op.type() == TokenType::NOT) {
            return "not " + emitExpr(unary->operand.get());
        }
        return unary->op.lexeme() + emitExpr(unary->operand.get());
    }
    if (auto* call = dynamic_cast<CallExpr*>(expr)) {
        std::string out = emitExpr(call->callee.get()) + "(";
        for (size_t i = 0; i < call->args.size(); ++i) {
            if (i > 0) out += ", ";
            out += emitExpr(call->args[i].get());
        }
        for (size_t i = 0; i < call->kwArgs.size(); ++i) {
            if (i > 0 || !call->args.empty()) out += ", ";
            out += call->kwArgs[i].first + "=" + emitExpr(call->kwArgs[i].second.get());
        }
        out += ")";
        return out;
    }
    if (auto* attr = dynamic_cast<AttributeExpr*>(expr)) {
        return emitExpr(attr->object.get()) + "." + attr->attribute;
    }
    if (auto* sub = dynamic_cast<SubscriptExpr*>(expr)) {
        return emitExpr(sub->object.get()) + "[" + emitExpr(sub->index.get()) + "]";
    }
    if (auto* list = dynamic_cast<ListExpr*>(expr)) {
        std::string out = "[";
        for (size_t i = 0; i < list->elements.size(); ++i) {
            if (i > 0) out += ", ";
            out += emitExpr(list->elements[i].get());
        }
        return out + "]";
    }
    if (auto* dict = dynamic_cast<DictExpr*>(expr)) {
        std::string out = "{";
        for (size_t i = 0; i < dict->entries.size(); ++i) {
            if (i > 0) out += ", ";
            out += emitExpr(dict->entries[i].first.get()) + ": " +
                   emitExpr(dict->entries[i].second.get());
        }
        return out + "}";
    }
    if (auto* set = dynamic_cast<SetExpr*>(expr)) {
        std::string out = "{";
        for (size_t i = 0; i < set->elements.size(); ++i) {
            if (i > 0) out += ", ";
            out += emitExpr(set->elements[i].get());
        }
        return out + "}";
    }
    if (auto* tup = dynamic_cast<TupleExpr*>(expr)) {
        std::string out = "(";
        for (size_t i = 0; i < tup->elements.size(); ++i) {
            if (i > 0) out += ", ";
            out += emitExpr(tup->elements[i].get());
        }
        return out + ")";
    }
    if (auto* ifExpr = dynamic_cast<IfExpr*>(expr)) {
        return emitExpr(ifExpr->thenExpr.get()) + " if " +
               emitExpr(ifExpr->condition.get()) + " else " +
               emitExpr(ifExpr->elseExpr.get());
    }
    if (auto* starred = dynamic_cast<StarredExpr*>(expr)) {
        return (starred->isDoubleStar ? "**" : "*") + emitExpr(starred->value.get());
    }
    return "???";
}

std::string PythonMigrator::emitType(TypeExpr* type) {
    if (!type) return "";

    if (auto* named = dynamic_cast<NamedTypeExpr*>(type)) {
        return named->name;
    }
    if (auto* generic = dynamic_cast<GenericTypeExpr*>(type)) {
        std::string out = emitType(generic->base.get()) + "[";
        for (size_t i = 0; i < generic->typeArgs.size(); ++i) {
            if (i > 0) out += ", ";
            out += emitType(generic->typeArgs[i].get());
        }
        return out + "]";
    }
    if (auto* union_ = dynamic_cast<UnionTypeExpr*>(type)) {
        std::string out;
        for (size_t i = 0; i < union_->types.size(); ++i) {
            if (i > 0) out += " | ";
            out += emitType(union_->types[i].get());
        }
        return out;
    }
    if (auto* opt = dynamic_cast<OptionalTypeExpr*>(type)) {
        return emitType(opt->inner.get()) + " | None";
    }
    return "Any";
}

// Helper to emit a block of statements (with braces or indent)
static std::string emitBlockHelper(PythonMigrator* self, const std::vector<std::unique_ptr<Stmt>>& stmts,
                                   int level, bool useBraces) {
    std::string out;
    if (useBraces) {
        out += " {\n";
        for (auto& s : stmts) {
            out += self->emitStmt(s.get(), level + 1);
        }
        out += ind(level) + "}";
    } else {
        out += ":\n";
        for (auto& s : stmts) {
            out += self->emitStmt(s.get(), level + 1);
        }
    }
    return out;
}

std::string PythonMigrator::emitStmt(Stmt* stmt, int level) {
    if (!stmt) return "";
    bool useBraces = impl_->options.useBraces;

    if (auto* expr = dynamic_cast<ExprStmt*>(stmt)) {
        return ind(level) + emitExpr(expr->expr.get()) + "\n";
    }

    if (auto* assign = dynamic_cast<AssignStmt*>(stmt)) {
        std::string out = ind(level);
        for (size_t i = 0; i < assign->targets.size(); ++i) {
            if (i > 0) out += " = ";
            out += emitExpr(assign->targets[i].get());
        }
        if (assign->typeAnnotation) {
            out += ": " + emitType(assign->typeAnnotation.get());
        }
        out += " = " + emitExpr(assign->value.get()) + "\n";
        return out;
    }

    if (auto* aug = dynamic_cast<AugAssignStmt*>(stmt)) {
        return ind(level) + emitExpr(aug->target.get()) + " " +
               aug->op.lexeme() + " " + emitExpr(aug->value.get()) + "\n";
    }

    if (auto* ann = dynamic_cast<AnnAssignStmt*>(stmt)) {
        std::string out = ind(level) + emitExpr(ann->target.get());
        out += ": " + emitType(ann->annotation.get());
        if (ann->value) out += " = " + emitExpr(ann->value.get());
        out += "\n";
        return out;
    }

    if (auto* ifStmt = dynamic_cast<IfStmt*>(stmt)) {
        std::string out = ind(level) + "if " + emitExpr(ifStmt->condition.get());
        out += emitBlockHelper(this, ifStmt->thenBody, level, useBraces);
        for (auto& [cond, body] : ifStmt->elifClauses) {
            out += " elif " + emitExpr(cond.get());
            out += emitBlockHelper(this, body, level, useBraces);
        }
        if (!ifStmt->elseBody.empty()) {
            out += " else";
            out += emitBlockHelper(this, ifStmt->elseBody, level, useBraces);
        }
        out += "\n";
        return out;
    }

    if (auto* whileStmt = dynamic_cast<WhileStmt*>(stmt)) {
        std::string out = ind(level) + "while " + emitExpr(whileStmt->condition.get());
        out += emitBlockHelper(this, whileStmt->body, level, useBraces);
        out += "\n";
        return out;
    }

    if (auto* forStmt = dynamic_cast<ForStmt*>(stmt)) {
        std::string out = ind(level) + "for " + emitExpr(forStmt->target.get()) +
                          " in " + emitExpr(forStmt->iterable.get());
        out += emitBlockHelper(this, forStmt->body, level, useBraces);
        out += "\n";
        return out;
    }

    if (auto* tryStmt = dynamic_cast<TryStmt*>(stmt)) {
        std::string out = ind(level) + "try";
        out += emitBlockHelper(this, tryStmt->tryBody, level, useBraces);
        for (auto& handler : tryStmt->handlers) {
            out += " catch";
            if (handler.type) out += " " + emitType(handler.type.get());
            if (!handler.name.empty()) out += " as " + handler.name;
            out += emitBlockHelper(this, handler.body, level, useBraces);
        }
        if (!tryStmt->finallyBody.empty()) {
            out += " finally";
            out += emitBlockHelper(this, tryStmt->finallyBody, level, useBraces);
        }
        out += "\n";
        return out;
    }

    if (auto* withStmt = dynamic_cast<WithStmt*>(stmt)) {
        std::string out = ind(level) + "with ";
        for (size_t i = 0; i < withStmt->items.size(); ++i) {
            if (i > 0) out += ", ";
            out += emitExpr(withStmt->items[i].contextExpr.get());
            if (withStmt->items[i].optionalVars)
                out += " as " + emitExpr(withStmt->items[i].optionalVars.get());
        }
        out += emitBlockHelper(this, withStmt->body, level, useBraces);
        out += "\n";
        return out;
    }

    if (auto* func = dynamic_cast<FunctionDecl*>(stmt)) {
        std::string out;
        for (auto& dec : func->decorators) {
            out += ind(level) + "@" + emitExpr(dec.get()) + "\n";
        }
        out += ind(level);
        if (func->isAsync) out += "async ";
        out += "def " + func->name + "(";
        for (size_t i = 0; i < func->params.size(); ++i) {
            if (i > 0) out += ", ";
            auto& p = func->params[i];
            if (p.isVarArg) out += "*";
            if (p.isKwArg) out += "**";
            out += p.name;
            if (p.type) out += ": " + emitType(p.type.get());
            if (p.defaultValue) out += " = " + emitExpr(p.defaultValue.get());
        }
        out += ")";
        if (func->returnType) out += " : " + emitType(func->returnType.get());
        out += emitBlockHelper(this, func->body, level, useBraces);
        out += "\n";
        return out;
    }

    if (auto* cls = dynamic_cast<ClassDecl*>(stmt)) {
        std::string out;
        for (auto& dec : cls->decorators) {
            out += ind(level) + "@" + emitExpr(dec.get()) + "\n";
        }
        out += ind(level) + "class " + cls->name;
        if (!cls->bases.empty()) {
            out += "(";
            for (size_t i = 0; i < cls->bases.size(); ++i) {
                if (i > 0) out += ", ";
                out += emitExpr(cls->bases[i].get());
            }
            out += ")";
        }
        out += emitBlockHelper(this, cls->body, level, useBraces);
        out += "\n";
        return out;
    }

    if (auto* ret = dynamic_cast<ReturnStmt*>(stmt)) {
        std::string out = ind(level) + "return";
        if (ret->value) out += " " + emitExpr(ret->value.get());
        out += "\n";
        return out;
    }

    if (dynamic_cast<PassStmt*>(stmt)) return ind(level) + "pass\n";
    if (dynamic_cast<BreakStmt*>(stmt)) return ind(level) + "break\n";
    if (dynamic_cast<ContinueStmt*>(stmt)) return ind(level) + "continue\n";

    if (auto* raise = dynamic_cast<RaiseStmt*>(stmt)) {
        std::string out = ind(level) + "raise";
        if (raise->exception) out += " " + emitExpr(raise->exception.get());
        out += "\n";
        return out;
    }

    if (auto* assert_ = dynamic_cast<AssertStmt*>(stmt)) {
        std::string out = ind(level) + "assert " + emitExpr(assert_->test.get());
        if (assert_->msg) out += ", " + emitExpr(assert_->msg.get());
        out += "\n";
        return out;
    }

    if (auto* global = dynamic_cast<GlobalStmt*>(stmt)) {
        std::string out = ind(level) + "global ";
        for (size_t i = 0; i < global->names.size(); ++i) {
            if (i > 0) out += ", ";
            out += global->names[i];
        }
        return out + "\n";
    }

    if (auto* nonlocal = dynamic_cast<NonlocalStmt*>(stmt)) {
        std::string out = ind(level) + "nonlocal ";
        for (size_t i = 0; i < nonlocal->names.size(); ++i) {
            if (i > 0) out += ", ";
            out += nonlocal->names[i];
        }
        return out + "\n";
    }

    if (auto* del = dynamic_cast<DeleteStmt*>(stmt)) {
        std::string out = ind(level) + "del ";
        for (size_t i = 0; i < del->targets.size(); ++i) {
            if (i > 0) out += ", ";
            out += emitExpr(del->targets[i].get());
        }
        return out + "\n";
    }

    if (auto* imp = dynamic_cast<ImportStmt*>(stmt)) {
        std::string out = ind(level) + "import ";
        for (size_t i = 0; i < imp->names.size(); ++i) {
            if (i > 0) out += ", ";
            out += imp->names[i].name;
            if (!imp->names[i].asName.empty()) out += " as " + imp->names[i].asName;
        }
        return out + "\n";
    }

    if (auto* fromImp = dynamic_cast<FromImportStmt*>(stmt)) {
        std::string out = ind(level) + "from " + fromImp->module + " import ";
        for (size_t i = 0; i < fromImp->names.size(); ++i) {
            if (i > 0) out += ", ";
            out += fromImp->names[i].name;
            if (!fromImp->names[i].asName.empty()) out += " as " + fromImp->names[i].asName;
        }
        return out + "\n";
    }

    return ind(level) + "# <unknown statement>\n";
}

} // namespace dragon
