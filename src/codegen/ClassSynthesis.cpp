/// Dragon CodeGen - @dataclass / NamedTuple / Enum compile-time synthesis
/// Split from Classes.cpp (file-size policy): pure code motion.
#include "../CodeGenImpl.h"
#include "ClassesShared.h"

namespace dragon {

//===----------------------------------------------------------------------===//
// 6.18 - @dataclass / NamedTuple compile-time synthesis
//===----------------------------------------------------------------------===//

// Helper: build a binary-op expression a <op> b (string concat / and-chain
// for synthesized __eq__ and __repr__).
static std::unique_ptr<Expr> makeBinaryOp(std::unique_ptr<Expr> lhs,
                                          TokenType op,
                                          const std::string& lexeme,
                                          std::unique_ptr<Expr> rhs,
                                          SourceLocation loc) {
    auto bin = std::make_unique<BinaryExpr>();
    bin->op = Token(op, lexeme, loc);
    bin->left = std::move(lhs);
    bin->right = std::move(rhs);
    bin->setLocation(loc);
    return bin;
}

// Helper: build `self.<field>` AttributeExpr.
static std::unique_ptr<Expr> makeSelfDot(const std::string& field,
                                         SourceLocation loc) {
    auto attr = std::make_unique<AttributeExpr>();
    auto self = std::make_unique<NameExpr>();
    self->name = "self";
    self->setLocation(loc);
    attr->object = std::move(self);
    attr->attribute = field;
    attr->setLocation(loc);
    return attr;
}

// Helper: build `str(<expr>)` call.
static std::unique_ptr<Expr> makeStrCall(std::unique_ptr<Expr> arg,
                                         SourceLocation loc) {
    auto call = std::make_unique<CallExpr>();
    auto callee = std::make_unique<NameExpr>();
    callee->name = "str";
    callee->setLocation(loc);
    call->callee = std::move(callee);
    call->args.push_back(std::move(arg));
    call->setLocation(loc);
    return call;
}

// Helper: build a string literal expression.
static std::unique_ptr<Expr> makeStrLiteral(const std::string& s,
                                            SourceLocation loc) {
    auto sl = std::make_unique<StringLiteral>();
    sl->value = s;
    sl->setLocation(loc);
    return sl;
}

// Helper: build a NamedTypeExpr.
static std::unique_ptr<TypeExpr> makeNamedType(const std::string& name,
                                               SourceLocation loc) {
    auto t = std::make_unique<NamedTypeExpr>();
    t->name = name;
    t->setLocation(loc);
    return t;
}

void CodeGen::Impl::synthesizeDataclassMethods(ClassDecl& node) {
    // Detect markers: @dataclass decorator OR NamedTuple base class.
    bool isDataclass = false, isNamedTuple = false;
    for (auto& dec : node.decorators) {
        if (auto* ne = dynamic_cast<NameExpr*>(dec.get())) {
            if (ne->name == "dataclass") isDataclass = true;
        }
    }
    for (auto& base : node.bases) {
        if (auto* bn = dynamic_cast<NameExpr*>(base.get())) {
            if (bn->name == "NamedTuple") isNamedTuple = true;
        }
    }
    if (!isDataclass && !isNamedTuple) return;

    // What does the user already define?
    bool hasInit = false, hasEq = false, hasRepr = false;
    for (auto& stmt : node.body) {
        if (auto* fd = dynamic_cast<FunctionDecl*>(stmt.get())) {
            if (fd->name == "__init__") hasInit = true;
            else if (fd->name == "__eq__") hasEq = true;
            else if (fd->name == "__repr__") hasRepr = true;
        }
    }

    // Collect fields: bare-name AnnAssignStmts (not static). Default values
    // are MOVED out of the AnnAssignStmt into the synthesized parameter.
    struct DCField {
        std::string name;
        std::unique_ptr<TypeExpr> type;
        std::unique_ptr<Expr> defaultValue;  // nullable
        SourceLocation loc;
    };
    std::vector<DCField> fields;
    for (auto& stmt : node.body) {
        auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get());
        if (!ann || ann->isStatic) continue;
        auto* tgt = dynamic_cast<NameExpr*>(ann->target.get());
        if (!tgt) continue;
        DCField f;
        f.name = tgt->name;
        f.type = cloneTypeExpr(ann->annotation.get());
        f.defaultValue = std::move(ann->value);
        f.loc = ann->location();
        fields.push_back(std::move(f));
    }

    // For NamedTuple, drop the marker base.
    if (isNamedTuple) {
        std::vector<std::unique_ptr<Expr>> filtered;
        for (auto& base : node.bases) {
            if (auto* bn = dynamic_cast<NameExpr*>(base.get())) {
                if (bn->name == "NamedTuple") continue;
            }
            filtered.push_back(std::move(base));
        }
        node.bases = std::move(filtered);
    }

    SourceLocation loc = node.location();

    // ─── Synthesize __init__ ────────────────────────────────────────────
    if (!hasInit && !fields.empty()) {
        auto init = std::make_unique<FunctionDecl>();
        init->name = "__init__";
        init->isMethod = true;
        init->isConstructor = true;
        init->hasImplicitSelf = true;
        init->setLocation(loc);
        for (auto& f : fields) {
            Parameter p;
            p.name = f.name;
            p.type = cloneTypeExpr(f.type.get());
            if (f.defaultValue) p.defaultValue = std::move(f.defaultValue);
            init->params.push_back(std::move(p));
        }
        for (auto& f : fields) {
            init->body.push_back(makeSelfAssign(f.name, f.loc));
        }
        node.body.insert(node.body.begin(), std::move(init));
    }

    // ─── Synthesize __eq__ ──────────────────────────────────────────────
    // def __eq__(other: ClassName) -> bool {
    //  return self.f1 == other.f1 and self.f2 == other.f2
    // }
    if (!hasEq) {
        auto eq = std::make_unique<FunctionDecl>();
        eq->name = "__eq__";
        eq->isMethod = true;
        eq->hasImplicitSelf = true;
        eq->returnType = makeNamedType("bool", loc);
        eq->setLocation(loc);
        // single param: other: ClassName
        Parameter p;
        p.name = "other";
        p.type = makeNamedType(node.name, loc);
        eq->params.push_back(std::move(p));

        std::unique_ptr<Expr> condition;
        if (fields.empty()) {
            auto t = std::make_unique<BooleanLiteral>();
            t->value = true;
            t->setLocation(loc);
            condition = std::move(t);
        } else {
            for (auto& f : fields) {
                auto lhs = makeSelfDot(f.name, f.loc);
                auto otherAttr = std::make_unique<AttributeExpr>();
                auto otherName = std::make_unique<NameExpr>();
                otherName->name = "other";
                otherName->setLocation(f.loc);
                otherAttr->object = std::move(otherName);
                otherAttr->attribute = f.name;
                otherAttr->setLocation(f.loc);
                auto cmp = makeBinaryOp(std::move(lhs), TokenType::EQUAL_EQUAL, "==",
                                        std::move(otherAttr), f.loc);
                if (!condition) condition = std::move(cmp);
                else condition = makeBinaryOp(std::move(condition),
                                              TokenType::AND, "and",
                                              std::move(cmp), f.loc);
            }
        }
        auto ret = std::make_unique<ReturnStmt>();
        ret->value = std::move(condition);
        ret->setLocation(loc);
        eq->body.push_back(std::move(ret));
        node.body.push_back(std::move(eq));
    }

    // ─── Synthesize __repr__ ────────────────────────────────────────────
    // def __repr__() -> str {
    //  return "ClassName(f1=" + str(self.f1) + ", f2=" + str(self.f2) + ")"
    // }
    if (!hasRepr) {
        auto repr = std::make_unique<FunctionDecl>();
        repr->name = "__repr__";
        repr->isMethod = true;
        repr->hasImplicitSelf = true;
        repr->returnType = makeNamedType("str", loc);
        repr->setLocation(loc);

        std::unique_ptr<Expr> chain;
        for (size_t i = 0; i < fields.size(); i++) {
            auto& f = fields[i];
            std::string prefix = (i == 0)
                ? (node.name + "(" + f.name + "=")
                : (", " + f.name + "=");
            auto prefixLit = makeStrLiteral(prefix, f.loc);
            auto valueStr = makeStrCall(makeSelfDot(f.name, f.loc), f.loc);
            auto piece = makeBinaryOp(std::move(prefixLit), TokenType::PLUS, "+",
                                      std::move(valueStr), f.loc);
            if (!chain) chain = std::move(piece);
            else chain = makeBinaryOp(std::move(chain), TokenType::PLUS, "+",
                                      std::move(piece), f.loc);
        }
        auto closing = makeStrLiteral(fields.empty() ? (node.name + "()") : ")", loc);
        if (chain) {
            chain = makeBinaryOp(std::move(chain), TokenType::PLUS, "+",
                                 std::move(closing), loc);
        } else {
            chain = std::move(closing);
        }
        auto ret = std::make_unique<ReturnStmt>();
        ret->value = std::move(chain);
        ret->setLocation(loc);
        repr->body.push_back(std::move(ret));
        node.body.push_back(std::move(repr));
    }

    // Track for later passes (TypeChecker registration, etc.)
    std::vector<std::string> fieldNames;
    for (auto& f : fields) fieldNames.push_back(f.name);
    dataclassFieldNames[node.name] = std::move(fieldNames);
    dataclassClassNames.insert(node.name);
}

// Helper: is this expression a call to the enum `auto()` sentinel?
static bool isEnumAutoCall(Expr* e) {
    auto* call = dynamic_cast<CallExpr*>(e);
    if (!call || !call->args.empty()) return false;
    auto* callee = dynamic_cast<NameExpr*>(call->callee.get());
    return callee && callee->name == "auto";
}

// Helper: build `list[ElemName]` type expression.
static std::unique_ptr<TypeExpr> makeListType(const std::string& elemName,
                                              SourceLocation loc) {
    auto g = std::make_unique<GenericTypeExpr>();
    g->base = makeNamedType("list", loc);
    g->typeArgs.push_back(makeNamedType(elemName, loc));
    g->setLocation(loc);
    return g;
}

// AST-level synthesis for class-based enums: `class C(Enum) { RED: int = 1 }`.
// Rewrites the class so each member is a singleton instance carrying .name and
// .value, with __init__/__str__/__repr__ and a __members__ list. Value-lookup
// (`C(v)`), iteration (`for x in C`), and IntEnum/StrEnum value-comparison are
// wired in CallExpr/ForLoop/Expressions keyed on enumKind/enumMemberNames.
// Mirrors the proven synthesizeDataclassMethods pathway. See stdlib/enum.dr.
void CodeGen::Impl::synthesizeEnumMethods(ClassDecl& node) {
    EnumKind kind = EnumKind::Plain;
    bool isEnum = false;
    for (auto& base : node.bases) {
        auto* bn = dynamic_cast<NameExpr*>(base.get());
        if (!bn) continue;
        if (bn->name == "Enum")         { kind = EnumKind::Plain; isEnum = true; }
        else if (bn->name == "IntEnum") { kind = EnumKind::Int;   isEnum = true; }
        else if (bn->name == "StrEnum") { kind = EnumKind::Str;   isEnum = true; }
    }
    if (!isEnum) return;

    SourceLocation loc = node.location();
    bool isStr = (kind == EnumKind::Str);
    std::string valueTy = isStr ? "str" : "int";

    // Collect member declarations: non-static `NAME: T = <literal|auto()>`.
    struct EMember { std::string name; int64_t ival = 0; std::string sval; SourceLocation loc; };
    std::vector<EMember> members;
    int64_t running = 0;  // int auto() counter: next auto == running + 1
    for (auto& stmt : node.body) {
        auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get());
        if (!ann || ann->isStatic || !ann->value) continue;
        auto* tgt = dynamic_cast<NameExpr*>(ann->target.get());
        if (!tgt) continue;
        EMember m; m.name = tgt->name; m.loc = ann->location();
        if (isStr) {
            if (auto* sl = dynamic_cast<StringLiteral*>(ann->value.get())) {
                m.sval = sl->value;
            } else if (isEnumAutoCall(ann->value.get())) {
                m.sval = m.name;  // StrEnum auto() -> lowercased member name
                for (auto& c : m.sval)
                    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            } else {
                continue;  // not a synthesizable member
            }
        } else {
            if (auto* il = dynamic_cast<IntegerLiteral*>(ann->value.get())) {
                m.ival = il->value; running = m.ival;
            } else if (auto* un = dynamic_cast<UnaryExpr*>(ann->value.get())) {
                auto* il2 = dynamic_cast<IntegerLiteral*>(un->operand.get());
                if (un->op.type() == TokenType::MINUS && il2) {
                    m.ival = -il2->value; running = m.ival;
                } else continue;
            } else if (isEnumAutoCall(ann->value.get())) {
                m.ival = running + 1; running = m.ival;
            } else {
                continue;
            }
        }
        members.push_back(std::move(m));
    }
    if (members.empty()) return;

    std::unordered_set<std::string> memberSet;
    for (auto& m : members) memberSet.insert(m.name);

    // Drop the member declarations (they become static singletons, not instance
    // fields) and the marker base.
    std::vector<std::unique_ptr<Stmt>> kept;
    for (auto& stmt : node.body) {
        if (auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get())) {
            if (!ann->isStatic) {
                if (auto* tgt = dynamic_cast<NameExpr*>(ann->target.get()))
                    if (memberSet.count(tgt->name)) continue;
            }
        }
        kept.push_back(std::move(stmt));
    }
    node.body = std::move(kept);
    {
        std::vector<std::unique_ptr<Expr>> filtered;
        for (auto& base : node.bases) {
            if (auto* bn = dynamic_cast<NameExpr*>(base.get()))
                if (bn->name == "Enum" || bn->name == "IntEnum" || bn->name == "StrEnum")
                    continue;
            filtered.push_back(std::move(base));
        }
        node.bases = std::move(filtered);
    }

    // Instance fields: value: <T> ; name: str (insert at front; name ends up first).
    auto declField = [&](const std::string& fn, std::unique_ptr<TypeExpr> ty) {
        auto f = std::make_unique<AnnAssignStmt>();
        auto t = std::make_unique<NameExpr>(); t->name = fn; t->setLocation(loc);
        f->target = std::move(t);
        f->annotation = std::move(ty);
        f->setLocation(loc);
        node.body.insert(node.body.begin(), std::move(f));
    };
    declField("value", makeNamedType(valueTy, loc));
    declField("name", makeNamedType("str", loc));

    // __init__(self, name: str, value: <T>) { self.name = name; self.value = value }
    {
        auto init = std::make_unique<FunctionDecl>();
        init->name = "__init__";
        init->isMethod = true;
        init->isConstructor = true;
        init->hasImplicitSelf = true;
        init->setLocation(loc);
        { Parameter p; p.name = "name"; p.type = makeNamedType("str", loc); init->params.push_back(std::move(p)); }
        { Parameter p; p.name = "value"; p.type = makeNamedType(valueTy, loc); init->params.push_back(std::move(p)); }
        init->body.push_back(makeSelfAssign("name", loc));
        init->body.push_back(makeSelfAssign("value", loc));
        node.body.push_back(std::move(init));
    }

    // __str__(self) -> str { return "ClassName." + self.name }
    {
        auto m = std::make_unique<FunctionDecl>();
        m->name = "__str__"; m->isMethod = true; m->hasImplicitSelf = true;
        m->returnType = makeNamedType("str", loc); m->setLocation(loc);
        auto e = makeBinaryOp(makeStrLiteral(node.name + ".", loc),
                              TokenType::PLUS, "+", makeSelfDot("name", loc), loc);
        auto ret = std::make_unique<ReturnStmt>(); ret->value = std::move(e); ret->setLocation(loc);
        m->body.push_back(std::move(ret));
        node.body.push_back(std::move(m));
    }

    // __repr__(self) -> str { return "<ClassName." + self.name + ": " + str(self.value) + ">" }
    {
        auto m = std::make_unique<FunctionDecl>();
        m->name = "__repr__"; m->isMethod = true; m->hasImplicitSelf = true;
        m->returnType = makeNamedType("str", loc); m->setLocation(loc);
        auto e = makeBinaryOp(makeStrLiteral("<" + node.name + ".", loc),
                              TokenType::PLUS, "+", makeSelfDot("name", loc), loc);
        e = makeBinaryOp(std::move(e), TokenType::PLUS, "+", makeStrLiteral(": ", loc), loc);
        e = makeBinaryOp(std::move(e), TokenType::PLUS, "+",
                         makeStrCall(makeSelfDot("value", loc), loc), loc);
        e = makeBinaryOp(std::move(e), TokenType::PLUS, "+", makeStrLiteral(">", loc), loc);
        auto ret = std::make_unique<ReturnStmt>(); ret->value = std::move(e); ret->setLocation(loc);
        m->body.push_back(std::move(ret));
        node.body.push_back(std::move(m));
    }

    // Static singleton members: static RED: ClassName = ClassName("RED", <value>).
    // Non-literal static inits -> built once in main's preamble.
    for (auto& m : members) {
        auto ctor = std::make_unique<CallExpr>();
        auto callee = std::make_unique<NameExpr>(); callee->name = node.name; callee->setLocation(m.loc);
        ctor->callee = std::move(callee);
        ctor->args.push_back(makeStrLiteral(m.name, m.loc));
        if (isStr) {
            ctor->args.push_back(makeStrLiteral(m.sval, m.loc));
        } else {
            auto iv = std::make_unique<IntegerLiteral>(); iv->value = m.ival; iv->setLocation(m.loc);
            ctor->args.push_back(std::move(iv));
        }
        ctor->setLocation(m.loc);

        auto sf = std::make_unique<AnnAssignStmt>();
        auto t = std::make_unique<NameExpr>(); t->name = m.name; t->setLocation(m.loc);
        sf->target = std::move(t);
        sf->annotation = makeNamedType(node.name, m.loc);
        sf->value = std::move(ctor);
        sf->isStatic = true;
        sf->setLocation(m.loc);
        node.body.push_back(std::move(sf));
    }

    // Static member list (definition order): static __members__: list[ClassName] = [ClassName.RED, ...].
    {
        auto listE = std::make_unique<ListExpr>();
        for (auto& m : members) {
            auto attr = std::make_unique<AttributeExpr>();
            auto cn = std::make_unique<NameExpr>(); cn->name = node.name; cn->setLocation(m.loc);
            attr->object = std::move(cn);
            attr->attribute = m.name;
            attr->setLocation(m.loc);
            listE->elements.push_back(std::move(attr));
        }
        listE->setLocation(loc);
        auto sf = std::make_unique<AnnAssignStmt>();
        auto t = std::make_unique<NameExpr>(); t->name = "__members__"; t->setLocation(loc);
        sf->target = std::move(t);
        sf->annotation = makeListType(node.name, loc);
        sf->value = std::move(listE);
        sf->isStatic = true;
        sf->setLocation(loc);
        node.body.push_back(std::move(sf));
    }

    // Static value-lookup helper: `static _lookup(value) -> ClassName` scans the
    // members and returns the matching singleton, or raises ValueError. CallExpr
    // redirects `ClassName(v)` here. Written in Dragon AST (reuses normal codegen
    // + the `for m in ClassName` rewrite) - no hand-rolled field-offset loop.
    {
        auto fn = std::make_unique<FunctionDecl>();
        fn->name = "_lookup";
        fn->isMethod = true;
        fn->isStatic = true;
        fn->hasImplicitSelf = false;
        fn->returnType = makeNamedType(node.name, loc);
        fn->setLocation(loc);
        { Parameter p; p.name = "value"; p.type = makeNamedType(valueTy, loc); fn->params.push_back(std::move(p)); }

        // for m in ClassName { if m.value == value { return m } }
        auto forStmt = std::make_unique<ForStmt>();
        { auto t = std::make_unique<NameExpr>(); t->name = "m"; t->setLocation(loc); forStmt->target = std::move(t); }
        { auto it = std::make_unique<NameExpr>(); it->name = node.name; it->setLocation(loc); forStmt->iterable = std::move(it); }
        forStmt->setLocation(loc);

        auto ifStmt = std::make_unique<IfStmt>();
        { // m.value == value
            auto mval = std::make_unique<AttributeExpr>();
            auto mn = std::make_unique<NameExpr>(); mn->name = "m"; mn->setLocation(loc);
            mval->object = std::move(mn); mval->attribute = "value"; mval->setLocation(loc);
            auto vn = std::make_unique<NameExpr>(); vn->name = "value"; vn->setLocation(loc);
            ifStmt->condition = makeBinaryOp(std::move(mval), TokenType::EQUAL_EQUAL, "==",
                                             std::move(vn), loc);
        }
        ifStmt->setLocation(loc);
        { auto ret = std::make_unique<ReturnStmt>();
          auto mn = std::make_unique<NameExpr>(); mn->name = "m"; mn->setLocation(loc);
          ret->value = std::move(mn); ret->setLocation(loc);
          ifStmt->thenBody.push_back(std::move(ret)); }
        forStmt->body.push_back(std::move(ifStmt));
        fn->body.push_back(std::move(forStmt));

        // raise ValueError("<value> is not a valid ClassName")
        auto raise = std::make_unique<RaiseStmt>();
        auto exc = std::make_unique<CallExpr>();
        auto vn = std::make_unique<NameExpr>(); vn->name = "ValueError"; vn->setLocation(loc);
        exc->callee = std::move(vn);
        exc->args.push_back(makeStrLiteral("invalid " + node.name + " value", loc));
        exc->setLocation(loc);
        raise->exception = std::move(exc);
        raise->setLocation(loc);
        fn->body.push_back(std::move(raise));

        node.body.push_back(std::move(fn));
    }

    // Record for CallExpr/ForLoop/Expressions wiring.
    enumKind[node.name] = kind;
    std::vector<std::string> names;
    for (auto& m : members) names.push_back(m.name);
    enumMemberNames[node.name] = std::move(names);
}

} // namespace dragon
