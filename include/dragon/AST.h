#ifndef DRAGON_AST_H
#define DRAGON_AST_H

#include "dragon/Token.h"
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <optional>

namespace dragon {

// Forward declarations
class ASTVisitor;
class Type;

//===----------------------------------------------------------------------===//
// Base Node Classes
//===----------------------------------------------------------------------===//

/// Base class for all AST nodes
class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual void accept(ASTVisitor& visitor) = 0;
    
    SourceLocation location() const { return location_; }
    void setLocation(SourceLocation loc) { location_ = loc; }

protected:
    SourceLocation location_;
};

/// Base class for expressions
class Expr : public ASTNode {
public:
    /// The inferred or declared type of this expression
    std::shared_ptr<Type> type;
};

/// Base class for statements
class Stmt : public ASTNode {};

/// Base class for type annotations
class TypeExpr : public ASTNode {};

//===----------------------------------------------------------------------===//
// Type Expressions (Type Annotations)
//===----------------------------------------------------------------------===//

/// Simple type name: int, str, MyClass
class NamedTypeExpr : public TypeExpr {
public:
    std::string name;
    void accept(ASTVisitor& visitor) override;
};

/// Generic type: list[int], dict[str, int]
class GenericTypeExpr : public TypeExpr {
public:
    std::unique_ptr<TypeExpr> base;
    std::vector<std::unique_ptr<TypeExpr>> typeArgs;
    void accept(ASTVisitor& visitor) override;
};

/// Optional type: int | None or Optional[int]
class OptionalTypeExpr : public TypeExpr {
public:
    std::unique_ptr<TypeExpr> inner;
    void accept(ASTVisitor& visitor) override;
};

/// Union type: int | str
class UnionTypeExpr : public TypeExpr {
public:
    std::vector<std::unique_ptr<TypeExpr>> types;
    void accept(ASTVisitor& visitor) override;
};

/// Callable type: Callable[[int, str], bool]
class CallableTypeExpr : public TypeExpr {
public:
    std::vector<std::unique_ptr<TypeExpr>> paramTypes;
    std::unique_ptr<TypeExpr> returnType;
    void accept(ASTVisitor& visitor) override;
};

/// Tuple type: tuple[int, str, bool]
class TupleTypeExpr : public TypeExpr {
public:
    std::vector<std::unique_ptr<TypeExpr>> elementTypes;
    void accept(ASTVisitor& visitor) override;
};

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

/// Integer literal: 42, 0x1F, 0b1010
class IntegerLiteral : public Expr {
public:
    int64_t value;
    void accept(ASTVisitor& visitor) override;
};

/// Float literal: 3.14, 1e-10
class FloatLiteral : public Expr {
public:
    double value;
    void accept(ASTVisitor& visitor) override;
};

/// One segment of an f-string: either a literal run of text, or an
/// interpolated expression (with optional !conversion and :format spec).
struct FStringPart {
    enum class Kind { Literal, Expression };
    Kind kind = Kind::Literal;
    std::string literal;             // populated when kind == Literal (escapes already resolved)
    std::unique_ptr<Expr> expr;      // populated when kind == Expression
    std::string formatSpec;          // optional :format_spec (e.g. ".2f", "x", "05d")
    char conversion = 0;             // 0 = none, 's' / 'r' / 'a' for !s / !r / !a
};

/// String literal: "hello", 'world', """multiline"""
class StringLiteral : public Expr {
public:
    std::string value;
    bool isRaw = false;
    bool isFString = false;
    bool isBytes = false;
    /// Parsed segments for f-strings - populated by Parser when isFString=true so
    /// Sema/TypeChecker/CodeGen all walk one shared AST instead of re-lexing the
    /// raw `value` at every stage. For non-f-strings this stays empty.
    std::vector<FStringPart> fstringParts;
    void accept(ASTVisitor& visitor) override;
};

/// One segment of a template body: a literal text run, a `!{expr}`
/// interpolation, or a `!{ ...statements... }` block interpolation. Parsed
/// once by the Parser into TemplateExpr::templateParts so Sema/TypeChecker/
/// CodeGen all walk one shared AST instead of re-lexing the raw `body` at
/// every stage (mirrors FStringPart). Without this the interpolations were
/// invisible to the TypeChecker, so a `!{p[0]}` tuple-subscript never got its
/// element type and lowered as a raw i64 pointer instead of the value.
struct TemplatePart {
    enum class Kind { Literal, Interpolation, Block };
    Kind kind = Kind::Literal;
    std::string literal;                          // Kind::Literal - raw text run (NOT escape-processed; template text is literal by design)
    std::unique_ptr<Expr> expr;                   // Kind::Interpolation - the parsed expression
    std::vector<std::unique_ptr<Stmt>> blockStmts;// Kind::Block - `for`/`if` block-interp statements
    std::string filterName;                       // optional `| filter` (raw; spread->join defaulting happens in CodeGen)
    bool isSpread = false;                         // `!{*expr}` spread sugar
    std::string exprText;                          // raw slot text (error messages / parse-failure fallback)
    size_t bangPos = 0;                            // byte offset of `!` in TemplateExpr::body (reactive/event-attr context in CodeGen)
    bool parseFailed = false;                      // sub-parse produced neither expr nor statements
};

/// Template expression: template { raw text with !{expr} interpolation }
/// Typed variant: template[HTML] { ... } - contentType = "HTML"
class TemplateExpr : public Expr {
public:
    std::string body;          // raw template text (between outer braces)
    std::string contentType;   // empty = untyped (str), non-empty = typed template

    /// Parsed segments of `body` - populated by the Parser (and, for file
    /// templates whose content the Parser never saw, lazily by CodeGen). One
    /// source of truth so the TypeChecker can walk each interpolation at its
    /// native type. Empty only for a genuinely empty template body.
    std::vector<TemplatePart> templateParts;

    // D017 Phase 4.B - true when this TemplateExpr was created from a
    // `:{ ... }` content alias (Parser detected TEMPLATE_CONTENT_OPEN), not
    // an explicit `template { ... }`. Drives two pieces of CodeGen behavior:
    //  1. contentType is inherited from the enclosing template[X] via the
    //  templateContextStack rather than being explicit.
    //  2. At statement position inside a `!{}` block, the rendered string
    //  appends to the block buffer (so `:{...}` fragments accumulate).
    //  Explicit `template { ... }` is not auto-appended.
    bool isContentAlias = false;
    void accept(ASTVisitor& visitor) override;
};

/// Compile-time file template: template("file.html")
/// Typed variant: template[HTML]("file.html") - contentType = "HTML"
class TemplateFileExpr : public Expr {
public:
    std::string filePath;     // path to template file (string literal)
    std::string contentType;  // empty = untyped (str), non-empty = typed template
    void accept(ASTVisitor& visitor) override;
};

/// Boolean literal: True, False
class BooleanLiteral : public Expr {
public:
    bool value;
    void accept(ASTVisitor& visitor) override;
};

/// None literal
class NoneLiteral : public Expr {
public:
    void accept(ASTVisitor& visitor) override;
};

/// Variable reference: x, my_var
class NameExpr : public Expr {
public:
    std::string name;
    /// `own x` at a consuming position (call argument, spawn argument, own-
    /// field-store RHS): the binding's +1 TRANSFERS to the consumer and the
    /// name is Moved afterwards (docs/002 ADR 2.4/2.8). Set by the parser
    /// only where the move grammar is legal; a move is always of a BINDING,
    /// so it lives on the name instead of a wrapper node.
    bool isMoveMarked = false;
    /// `dub x`: the ONLY second-owner path - a deep copy priced at this
    /// exact position (docs/002 ADR 2.7). str/bytes lower to an incref
    /// (immutable payloads are indistinguishable from copies); containers
    /// deep-copy; non-dubable types are E11 in the TypeChecker. The source
    /// binding is READ, never consumed.
    bool isDubMarked = false;
    void accept(ASTVisitor& visitor) override;
};

/// Binary operation: a + b, x and y
class BinaryExpr : public Expr {
public:
    std::unique_ptr<Expr> left;
    Token op;
    std::unique_ptr<Expr> right;
    void accept(ASTVisitor& visitor) override;
};

/// Chained comparison: a < b < c, 0 <= x < 100
class ChainedCompExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> operands;  // a, b, c, ...
    std::vector<Token> operators;                   // <, <, ...
    void accept(ASTVisitor& visitor) override;
};

/// Walrus operator: name := value
class WalrusExpr : public Expr {
public:
    std::string name;
    std::unique_ptr<Expr> value;
    void accept(ASTVisitor& visitor) override;
};

/// Unary operation: -x, not y
class UnaryExpr : public Expr {
public:
    Token op;
    std::unique_ptr<Expr> operand;
    void accept(ASTVisitor& visitor) override;
};

/// Function call: func(a, b, c=d)
class CallExpr : public Expr {
public:
    std::unique_ptr<Expr> callee;
    std::vector<std::unique_ptr<Expr>> args;
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> kwArgs;
    // Method-overload dispatch (ADR 010): when the callee resolves to an
    // OVERLOADED method (a class declares >1 method with this name), the
    // TypeChecker resolves the call to one overload by arity + parameter types
    // and records its index here (0-based, in class-body order). CodeGen appends
    // `__ovN` to the method symbol so the direct call targets the right monomorphic
    // function - the dispatch is fully compile-time, so there is no runtime cost.
    // -1 means "not an overloaded method" (the single-method fast path).
    int resolvedMethodOverload = -1;
    void accept(ASTVisitor& visitor) override;
};

/// Attribute access: obj.attr
class AttributeExpr : public Expr {
public:
    std::unique_ptr<Expr> object;
    std::string attribute;
    void accept(ASTVisitor& visitor) override;
};

/// Subscript: arr[idx], dict["key"]
class SubscriptExpr : public Expr {
public:
    std::unique_ptr<Expr> object;
    std::unique_ptr<Expr> index;
    void accept(ASTVisitor& visitor) override;
};

/// Slice: arr[1:5], arr[::2]
class SliceExpr : public Expr {
public:
    std::unique_ptr<Expr> lower;  // optional
    std::unique_ptr<Expr> upper;  // optional
    std::unique_ptr<Expr> step;   // optional
    void accept(ASTVisitor& visitor) override;
};

/// List literal: [1, 2, 3]
class ListExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> elements;
    void accept(ASTVisitor& visitor) override;
};

/// Tuple literal: (1, 2, 3)
class TupleExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> elements;
    void accept(ASTVisitor& visitor) override;
};

/// Dict literal: {"a": 1, "b": 2}
class DictExpr : public Expr {
public:
    std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>> entries;
    void accept(ASTVisitor& visitor) override;
};

/// Set literal: {1, 2, 3}
class SetExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> elements;
    void accept(ASTVisitor& visitor) override;
};

/// Extra comprehension clause: for var in iterable if condition
struct CompClause {
    std::vector<std::string> varNames;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;  // optional
};

/// List comprehension: [x*2 for x in items if x > 0]
class ListCompExpr : public Expr {
public:
    std::unique_ptr<Expr> element;
    std::string varName;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;  // optional
    std::vector<CompClause> extraClauses;  // for nested comprehensions
    void accept(ASTVisitor& visitor) override;
};

/// Dict comprehension: {k: v for k, v in items}
class DictCompExpr : public Expr {
public:
    std::unique_ptr<Expr> key;
    std::unique_ptr<Expr> value;
    std::vector<std::string> varNames;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;  // optional
    std::vector<CompClause> extraClauses;  // for nested comprehensions
    void accept(ASTVisitor& visitor) override;
};

/// Set comprehension: {x*2 for x in items if x > 0}
class SetCompExpr : public Expr {
public:
    std::unique_ptr<Expr> element;
    std::string varName;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;  // optional
    std::vector<CompClause> extraClauses;  // for nested comprehensions
    void accept(ASTVisitor& visitor) override;
};

/// Generator expression: (x*2 for x in items if x > 0)
/// Eagerly evaluated as a list for now
class GeneratorExpr : public Expr {
public:
    std::unique_ptr<Expr> element;
    std::string varName;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;  // optional
    std::vector<CompClause> extraClauses;  // for nested comprehensions
    void accept(ASTVisitor& visitor) override;
};

/// Lambda expression: lambda x, y: x + y
/// Dragon extended: lambda (x: int, y: int) : int { return x + y }
class LambdaExpr : public Expr {
public:
    struct Parameter {
        std::string name;
        std::unique_ptr<TypeExpr> type;  // required in Dragon
        std::unique_ptr<Expr> defaultValue;  // optional
    };
    std::vector<Parameter> params;
    std::unique_ptr<TypeExpr> returnType;  // required in Dragon
    std::unique_ptr<Expr> body;  // For simple lambdas
    std::vector<std::unique_ptr<Stmt>> bodyStmts;  // For Dragon block lambdas
    std::vector<std::string> capturedVars;  // D027: variables captured from enclosing scope
    std::vector<std::string> mutatedCapturedVars;  // D027.1: subset of capturedVars marked `nonlocal` (heap-cell promotion)
    void accept(ASTVisitor& visitor) override;
};

/// Ternary conditional: a if condition else b
class IfExpr : public Expr {
public:
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Expr> thenExpr;
    std::unique_ptr<Expr> elseExpr;
    void accept(ASTVisitor& visitor) override;
};

/// Await expression: await coro()
class AwaitExpr : public Expr {
public:
    std::unique_ptr<Expr> operand;
    void accept(ASTVisitor& visitor) override;
};

/// Fire expression: fire fn(args) or fire { block } - spawn green thread
class FireExpr : public Expr {
public:
    std::unique_ptr<Expr> operand;  // CallExpr for fire fn() form
    std::vector<std::unique_ptr<Stmt>> bodyStmts;  // for fire { block } form
    std::vector<std::string> capturedVars;  // D027: variables captured from enclosing scope
    std::vector<std::string> mutatedCapturedVars;  // D027.1: subset of capturedVars marked `nonlocal` (heap-cell promotion)
    void accept(ASTVisitor& visitor) override;
};

/// Yield expression: yield value
class YieldExpr : public Expr {
public:
    std::unique_ptr<Expr> value;  // optional
    bool isYieldFrom = false;
    void accept(ASTVisitor& visitor) override;
};

/// Starred expression: *args, **kwargs
class StarredExpr : public Expr {
public:
    std::unique_ptr<Expr> value;
    bool isDoubleStar = false;  // **kwargs vs *args
    void accept(ASTVisitor& visitor) override;
};

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

/// Expression statement
class ExprStmt : public Stmt {
public:
    std::unique_ptr<Expr> expr;
    void accept(ASTVisitor& visitor) override;
};

/// Assignment: x = 5, x: int = 5
class AssignStmt : public Stmt {
public:
    std::vector<std::unique_ptr<Expr>> targets;
    std::unique_ptr<Expr> value;
    std::unique_ptr<TypeExpr> typeAnnotation;  // required in Dragon
    bool isConst = false;  // const multi-target unpack: const a: T, b: U = expr
    void accept(ASTVisitor& visitor) override;
};

/// Augmented assignment: x += 5
class AugAssignStmt : public Stmt {
public:
    std::unique_ptr<Expr> target;
    Token op;  // +=, -=, etc.
    std::unique_ptr<Expr> value;
    void accept(ASTVisitor& visitor) override;
};

/// Annotated assignment without value: x: int
class AnnAssignStmt : public Stmt {
public:
    std::unique_ptr<Expr> target;
    std::unique_ptr<TypeExpr> annotation;
    std::unique_ptr<Expr> value;  // optional
    bool isConst = false;   // true if declared with 'const' (.dr mode)
    bool isStatic = false;  // true if declared with 'static' (.dr mode)
    /// `own f: T` field marker (docs/001-memory.md): the field is the value's
    /// SOLE owner - death of the instance releases it (raw handles via the
    /// intrinsic releaser registry, heap values via the normal dealloc
    /// decref), and stores must transfer ownership (a borrow is a compile
    /// error). Set by the parser (.dr class bodies), enforced by
    /// OwnershipCheck, consumed by the dealloc emission in codegen.
    bool isOwn = false;
    void accept(ASTVisitor& visitor) override;
};

/// If statement
class IfStmt : public Stmt {
public:
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Stmt>> thenBody;
    std::vector<std::pair<std::unique_ptr<Expr>, std::vector<std::unique_ptr<Stmt>>>> elifClauses;
    std::vector<std::unique_ptr<Stmt>> elseBody;
    void accept(ASTVisitor& visitor) override;
};

/// While loop
class WhileStmt : public Stmt {
public:
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Stmt>> elseBody;  // else clause
    void accept(ASTVisitor& visitor) override;
};

/// For loop
class ForStmt : public Stmt {
public:
    std::unique_ptr<Expr> target;
    std::unique_ptr<Expr> iterable;
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Stmt>> elseBody;  // else clause
    void accept(ASTVisitor& visitor) override;
};

/// Try statement (supports both 'except' and Dragon's 'catch')
class TryStmt : public Stmt {
public:
    struct ExceptHandler {
        std::unique_ptr<TypeExpr> type;  // optional (first type for tuple form)
        std::vector<std::string> altTypeNames;  // `except (A, B, ...)` extras
        std::string name;  // optional
        std::vector<std::unique_ptr<Stmt>> body;
        bool isStar = false;  // except* (PEP 654)
    };
    std::vector<std::unique_ptr<Stmt>> tryBody;
    std::vector<ExceptHandler> handlers;
    std::vector<std::unique_ptr<Stmt>> elseBody;
    std::vector<std::unique_ptr<Stmt>> finallyBody;
    void accept(ASTVisitor& visitor) override;
};

/// With statement
class WithStmt : public Stmt {
public:
    struct WithItem {
        std::unique_ptr<Expr> contextExpr;
        std::unique_ptr<Expr> optionalVars;  // as target
    };
    std::vector<WithItem> items;
    std::vector<std::unique_ptr<Stmt>> body;
    void accept(ASTVisitor& visitor) override;
};

/// Thread statement: thread { block } - scoped OS thread with auto-join
class ThreadStmt : public Stmt {
public:
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::string> capturedVars;  // D027: variables captured from enclosing scope
    std::vector<std::string> mutatedCapturedVars;  // D027.1: subset of capturedVars marked `nonlocal` (heap-cell promotion)
    void accept(ASTVisitor& visitor) override;
};

/// Match pattern for match/case (PEP 634)
struct MatchPattern {
    enum class Kind {
        Literal,    // 42, "hello", True, None
        Capture,    // name (binds value)
        Wildcard,   // _
        Sequence,   // [p1, p2, ...] or (p1, p2, ...)
        Or,         // p1 | p2
        Class,      // ClassName(p1, p2) - future
        Value,      // dotted name like Color.RED
    };
    Kind kind;
    std::unique_ptr<Expr> literal;                  // for Literal, Value
    std::string name;                               // for Capture
    std::vector<MatchPattern> subPatterns;           // for Sequence, Or
    std::unique_ptr<Expr> guard;                    // optional guard (if expr)
};

/// Match/case statement (PEP 634)
class MatchStmt : public Stmt {
public:
    struct MatchCase {
        MatchPattern pattern;
        std::unique_ptr<Expr> guard;  // optional: case pattern if guard
        std::vector<std::unique_ptr<Stmt>> body;
    };
    std::unique_ptr<Expr> subject;
    std::vector<MatchCase> cases;
    void accept(ASTVisitor& visitor) override;
};

/// Return statement
class ReturnStmt : public Stmt {
public:
    std::unique_ptr<Expr> value;  // optional
    void accept(ASTVisitor& visitor) override;
};

/// Raise statement
class RaiseStmt : public Stmt {
public:
    std::unique_ptr<Expr> exception;  // optional
    std::unique_ptr<Expr> cause;  // from clause
    void accept(ASTVisitor& visitor) override;
};

/// Break statement
class BreakStmt : public Stmt {
public:
    void accept(ASTVisitor& visitor) override;
};

/// Continue statement
class ContinueStmt : public Stmt {
public:
    void accept(ASTVisitor& visitor) override;
};

/// Pass statement
class PassStmt : public Stmt {
public:
    void accept(ASTVisitor& visitor) override;
};

/// Assert statement
class AssertStmt : public Stmt {
public:
    std::unique_ptr<Expr> test;
    std::unique_ptr<Expr> msg;  // optional
    void accept(ASTVisitor& visitor) override;
};

/// Global statement
class GlobalStmt : public Stmt {
public:
    std::vector<std::string> names;
    void accept(ASTVisitor& visitor) override;
};

/// Nonlocal statement
class NonlocalStmt : public Stmt {
public:
    std::vector<std::string> names;
    void accept(ASTVisitor& visitor) override;
};

/// Delete statement
class DeleteStmt : public Stmt {
public:
    std::vector<std::unique_ptr<Expr>> targets;
    /// Parallel to `targets`, set by OwnershipCheck (docs/002 ADR): 1 when the
    /// target is a compiler-PROVEN sole-owner local (`Owned`, no escape fact).
    /// Codegen emits the debug rc==1 assert only for proven targets; an
    /// unproven `del` keeps the plain scope-exit-identical release. Empty when
    /// the pass has not run (e.g. partial-analysis tools).
    std::vector<uint8_t> provenUnique;
    void accept(ASTVisitor& visitor) override;
};

/// Import statement: import x, y as z
class ImportStmt : public Stmt {
public:
    struct Alias {
        std::string name;
        std::string asName;  // optional
    };
    std::vector<Alias> names;
    void accept(ASTVisitor& visitor) override;
};

/// From-import statement: from x import y, z
class FromImportStmt : public Stmt {
public:
    std::string module;
    int level = 0;  // relative import level
    std::vector<ImportStmt::Alias> names;  // empty means import *
    void accept(ASTVisitor& visitor) override;
};

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

/// Function parameter
struct Parameter {
    std::string name;
    std::unique_ptr<TypeExpr> type;  // required in Dragon
    std::unique_ptr<Expr> defaultValue;  // optional
    bool isVarArg = false;   // *args
    bool isKwArg = false;    // **kwargs
    /// `def f(own p: T)`: the callee OWNS p (docs/002 ADR 2.8) - the caller
    /// moved its +1 in, the body may consume it, and the callee's scope exit
    /// releases it if nothing did. Both ends of a move must say own (E13/E14).
    bool isOwn = false;
};

/// Generic type parameter (PEP 695): the `T` in `class Foo[T]` / `def f[T]()`.
/// Per Decision 044, `bound` is reserved for D046 (bounded `T: Base`) and is
/// always nullptr in v1 - its presence in the AST keeps the surface stable so
/// D046 doesn't churn it. A decl whose `typeParams` is non-empty is a
/// *generic template*: it is parsed and abstractly type-checked once, but never
/// lowered to LLVM directly (it has free type vars) - only the monomorphizer's
/// stamped instantiations reach CodeGen.
struct TypeParam {
    std::string name;
    std::unique_ptr<TypeExpr> bound;  // nullptr in v1 (D046)
};

/// Function definition
class FunctionDecl : public Stmt {
public:
    std::string name;
    std::vector<TypeParam> typeParams;     // D044 - PEP 695 `def f[T](...)`; empty = non-generic
    std::vector<Parameter> params;
    std::unique_ptr<TypeExpr> returnType;  // required in Dragon
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Expr>> decorators;
    bool isAsync = false;
    bool isMethod = false;         // true if defined inside a class body
    bool hasImplicitSelf = false;  // true if self is implicit (.dr mode)
    bool isStatic = false;         // true if static or @staticmethod
    bool isClassMethod = false;    // true if @classmethod (skip cls param, callable on class)
    bool isConstructor = false;    // true if parsed from self() syntax (.dr mode)
    bool isExtern = false;         // true if extern "C" def (C FFI, no body)
    std::string externLib;         // library hint from extern "C" from "lib" { }
    /// C-side linker symbol for an `extern "C" def CSYM(...) as DRAGON_NAME`
    /// declaration where CSYM collides with a Dragon keyword (e.g. `raise`).
    /// When non-empty, `name` holds the Dragon-visible identifier (the alias)
    /// and `externSymbol` holds the C symbol the linker should resolve to.
    /// Empty for the plain `extern "C" def fn(...)` shape, where `name` is
    /// both the C symbol and the Dragon-visible identifier.
    std::string externSymbol;
    bool isProperty = false;       // true if @property - accessed without parens; in vtable
    std::string propertySetterFor; // non-empty if @<name>.setter - name of the property it sets
    int constructorIndex = -1;     // overload index (0, 1, 2, ...) for __init__ methods
    // ADR 010 method overloading: when a class declares >1 method with this
    // name, each gets a 0-based index in class-body order and the shared count.
    // CodeGen mangles the LLVM symbol `Class_method__ovN` only when count > 1, so
    // a non-overloaded method (count == 1) keeps its bare name and identical
    // codegen. Resolution is compile-time; the emitted call is a direct call.
    int methodOverloadIndex = -1;  // -1 = not overloaded
    int methodOverloadCount = 1;   // number of same-name overloads (1 = unique)
    int posOnlyEnd = -1;           // index after last positional-only param (/ separator)
    int kwOnlyStart = -1;          // index of first keyword-only param (bare * separator)
    std::vector<std::string> capturedVars;  // populated by Sema for nested defs (closure semantics)
    std::vector<std::string> mutatedCapturedVars;  // D027.1: subset of capturedVars marked `nonlocal` in this fn body
    std::optional<std::string> docstring;  // first ExprStmt(StringLiteral) lifted out of body; powers __doc__
    /// D044 cross-module generics: when this is a stamped generic instantiation
    /// of a template defined in another module, holds that DEFINING module's
    /// name. CodeGen resolves the body's bare names (sibling functions, module
    /// globals) against it, so a `Signal[int]` stamped at a use site can still
    /// reach its `stdlib/ui` helpers. Empty for normal (non-stamped) decls.
    std::string genericHomeModule;
    void accept(ASTVisitor& visitor) override;
};

/// Class definition
class ClassDecl : public Stmt {
public:
    std::string name;
    std::vector<TypeParam> typeParams;  // D044 - PEP 695 `class Foo[T]`; empty = non-generic
    std::vector<std::unique_ptr<Expr>> bases;
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> keywords;  // metaclass etc.
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Expr>> decorators;
    std::optional<std::string> docstring;  // first ExprStmt(StringLiteral) lifted out of body; powers __doc__
    /// D044 cross-module generics: defining module of the template this is a
    /// stamped instantiation of (empty for normal decls). See FunctionDecl.
    std::string genericHomeModule;
    void accept(ASTVisitor& visitor) override;
};

/// Canonical positional order of a class's own instance fields, as declared:
/// class-body annotations first (in source order), then `self.X = ...` targets
/// across method bodies (in source order), deduplicated. This is the single
/// source of truth for positional `match` class-pattern destructuring
/// (`case Point(x, y)`) - BOTH the TypeChecker (capture types) and CodeGen
/// (field loads) call this on the SAME ClassDecl, so position->field-name is
/// guaranteed identical across stages. Parent fields are NOT included; callers
/// prepend the parent's order for inheritance.
std::vector<std::string> instanceFieldOrder(const ClassDecl& cls);

/// Type alias statement (PEP 695): type Point = tuple[int, int]
class TypeAliasStmt : public Stmt {
public:
    std::string name;
    std::unique_ptr<TypeExpr> value;
    void accept(ASTVisitor& visitor) override;
};

//===----------------------------------------------------------------------===//
// Module
//===----------------------------------------------------------------------===//

/// A complete Dragon/Python module
class Module : public ASTNode {
public:
    std::string filename;
    std::string moduleName;  // set by ModuleResolver for imported modules
    bool isDragonFile = true;  // .dr mode (braces) vs .py mode (indentation)
    std::vector<std::unique_ptr<Stmt>> body;
    std::optional<std::string> docstring;  // first ExprStmt(StringLiteral) lifted out of body; powers __doc__
    void accept(ASTVisitor& visitor) override;
};

//===----------------------------------------------------------------------===//
// AST utilities
//===----------------------------------------------------------------------===//

/// True if executing `stmts` ALWAYS leaves the current block on every path
/// (the last statement is a return/raise/break/continue, or an `if` with an
/// `else` where every branch terminates). A conservative under-approximation:
/// it may answer `false` for a body that does terminate (e.g. via a `while
/// true` with no break), never `true` for one that can fall through. Shared by
/// the TypeChecker and CodeGen so their flow-sensitive narrowing of the code
/// after a terminating `if` stays in lockstep.
bool stmtsAlwaysTerminate(const std::vector<std::unique_ptr<Stmt>>& stmts);

//===----------------------------------------------------------------------===//
// Visitor Pattern
//===----------------------------------------------------------------------===//

/// Visitor base class for AST traversal
class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    // Type expressions
    virtual void visit(NamedTypeExpr& node) = 0;
    virtual void visit(GenericTypeExpr& node) = 0;
    virtual void visit(OptionalTypeExpr& node) = 0;
    virtual void visit(UnionTypeExpr& node) = 0;
    virtual void visit(CallableTypeExpr& node) = 0;
    virtual void visit(TupleTypeExpr& node) = 0;

    // Expressions
    virtual void visit(IntegerLiteral& node) = 0;
    virtual void visit(FloatLiteral& node) = 0;
    virtual void visit(StringLiteral& node) = 0;
    virtual void visit(BooleanLiteral& node) = 0;
    virtual void visit(NoneLiteral& node) = 0;
    virtual void visit(NameExpr& node) = 0;
    virtual void visit(BinaryExpr& node) = 0;
    virtual void visit(ChainedCompExpr& node) = 0;
    virtual void visit(WalrusExpr& node) = 0;
    virtual void visit(UnaryExpr& node) = 0;
    virtual void visit(CallExpr& node) = 0;
    virtual void visit(AttributeExpr& node) = 0;
    virtual void visit(SubscriptExpr& node) = 0;
    virtual void visit(SliceExpr& node) = 0;
    virtual void visit(ListExpr& node) = 0;
    virtual void visit(TupleExpr& node) = 0;
    virtual void visit(DictExpr& node) = 0;
    virtual void visit(SetExpr& node) = 0;
    virtual void visit(ListCompExpr& node) = 0;
    virtual void visit(DictCompExpr& node) = 0;
    virtual void visit(SetCompExpr& node) = 0;
    virtual void visit(GeneratorExpr& node) = 0;
    virtual void visit(LambdaExpr& node) = 0;
    virtual void visit(IfExpr& node) = 0;
    virtual void visit(AwaitExpr& node) = 0;
    virtual void visit(FireExpr& node) = 0;
    virtual void visit(YieldExpr& node) = 0;
    virtual void visit(StarredExpr& node) = 0;
    virtual void visit(TemplateExpr& node) = 0;
    virtual void visit(TemplateFileExpr& node) = 0;

    // Statements
    virtual void visit(ExprStmt& node) = 0;
    virtual void visit(AssignStmt& node) = 0;
    virtual void visit(AugAssignStmt& node) = 0;
    virtual void visit(AnnAssignStmt& node) = 0;
    virtual void visit(IfStmt& node) = 0;
    virtual void visit(WhileStmt& node) = 0;
    virtual void visit(ForStmt& node) = 0;
    virtual void visit(TryStmt& node) = 0;
    virtual void visit(WithStmt& node) = 0;
    virtual void visit(ThreadStmt& node) = 0;
    virtual void visit(MatchStmt& node) = 0;
    virtual void visit(ReturnStmt& node) = 0;
    virtual void visit(RaiseStmt& node) = 0;
    virtual void visit(BreakStmt& node) = 0;
    virtual void visit(ContinueStmt& node) = 0;
    virtual void visit(PassStmt& node) = 0;
    virtual void visit(AssertStmt& node) = 0;
    virtual void visit(GlobalStmt& node) = 0;
    virtual void visit(NonlocalStmt& node) = 0;
    virtual void visit(DeleteStmt& node) = 0;
    virtual void visit(ImportStmt& node) = 0;
    virtual void visit(FromImportStmt& node) = 0;

    // Declarations
    virtual void visit(FunctionDecl& node) = 0;
    virtual void visit(ClassDecl& node) = 0;
    virtual void visit(TypeAliasStmt& node) = 0;

    // Module
    virtual void visit(Module& node) = 0;
};

/// Default visitor that recursively visits all children
class DefaultASTVisitor : public ASTVisitor {
public:
    // Type expressions
    void visit(NamedTypeExpr& node) override;
    void visit(GenericTypeExpr& node) override;
    void visit(OptionalTypeExpr& node) override;
    void visit(UnionTypeExpr& node) override;
    void visit(CallableTypeExpr& node) override;
    void visit(TupleTypeExpr& node) override;

    // Expressions
    void visit(IntegerLiteral& node) override;
    void visit(FloatLiteral& node) override;
    void visit(StringLiteral& node) override;
    void visit(BooleanLiteral& node) override;
    void visit(NoneLiteral& node) override;
    void visit(NameExpr& node) override;
    void visit(BinaryExpr& node) override;
    void visit(ChainedCompExpr& node) override;
    void visit(WalrusExpr& node) override;
    void visit(UnaryExpr& node) override;
    void visit(CallExpr& node) override;
    void visit(AttributeExpr& node) override;
    void visit(SubscriptExpr& node) override;
    void visit(SliceExpr& node) override;
    void visit(ListExpr& node) override;
    void visit(TupleExpr& node) override;
    void visit(DictExpr& node) override;
    void visit(SetExpr& node) override;
    void visit(ListCompExpr& node) override;
    void visit(DictCompExpr& node) override;
    void visit(SetCompExpr& node) override;
    void visit(GeneratorExpr& node) override;
    void visit(LambdaExpr& node) override;
    void visit(IfExpr& node) override;
    void visit(AwaitExpr& node) override;
    void visit(FireExpr& node) override;
    void visit(YieldExpr& node) override;
    void visit(StarredExpr& node) override;
    void visit(TemplateExpr& node) override;
    void visit(TemplateFileExpr& node) override;

    // Statements
    void visit(ExprStmt& node) override;
    void visit(AssignStmt& node) override;
    void visit(AugAssignStmt& node) override;
    void visit(AnnAssignStmt& node) override;
    void visit(IfStmt& node) override;
    void visit(WhileStmt& node) override;
    void visit(ForStmt& node) override;
    void visit(TryStmt& node) override;
    void visit(WithStmt& node) override;
    void visit(ThreadStmt& node) override;
    void visit(MatchStmt& node) override;
    void visit(ReturnStmt& node) override;
    void visit(RaiseStmt& node) override;
    void visit(BreakStmt& node) override;
    void visit(ContinueStmt& node) override;
    void visit(PassStmt& node) override;
    void visit(AssertStmt& node) override;
    void visit(GlobalStmt& node) override;
    void visit(NonlocalStmt& node) override;
    void visit(DeleteStmt& node) override;
    void visit(ImportStmt& node) override;
    void visit(FromImportStmt& node) override;

    // Declarations
    void visit(FunctionDecl& node) override;
    void visit(ClassDecl& node) override;
    void visit(TypeAliasStmt& node) override;

    // Module
    void visit(Module& node) override;
};

/// AST printer for debugging
class ASTPrinter : public ASTVisitor {
public:
    std::string print(ASTNode& node);

    // Type expressions
    void visit(NamedTypeExpr& node) override;
    void visit(GenericTypeExpr& node) override;
    void visit(OptionalTypeExpr& node) override;
    void visit(UnionTypeExpr& node) override;
    void visit(CallableTypeExpr& node) override;
    void visit(TupleTypeExpr& node) override;

    // Expressions
    void visit(IntegerLiteral& node) override;
    void visit(FloatLiteral& node) override;
    void visit(StringLiteral& node) override;
    void visit(BooleanLiteral& node) override;
    void visit(NoneLiteral& node) override;
    void visit(NameExpr& node) override;
    void visit(BinaryExpr& node) override;
    void visit(ChainedCompExpr& node) override;
    void visit(WalrusExpr& node) override;
    void visit(UnaryExpr& node) override;
    void visit(CallExpr& node) override;
    void visit(AttributeExpr& node) override;
    void visit(SubscriptExpr& node) override;
    void visit(SliceExpr& node) override;
    void visit(ListExpr& node) override;
    void visit(TupleExpr& node) override;
    void visit(DictExpr& node) override;
    void visit(SetExpr& node) override;
    void visit(ListCompExpr& node) override;
    void visit(DictCompExpr& node) override;
    void visit(SetCompExpr& node) override;
    void visit(GeneratorExpr& node) override;
    void visit(LambdaExpr& node) override;
    void visit(IfExpr& node) override;
    void visit(AwaitExpr& node) override;
    void visit(FireExpr& node) override;
    void visit(YieldExpr& node) override;
    void visit(StarredExpr& node) override;
    void visit(TemplateExpr& node) override;
    void visit(TemplateFileExpr& node) override;

    // Statements
    void visit(ExprStmt& node) override;
    void visit(AssignStmt& node) override;
    void visit(AugAssignStmt& node) override;
    void visit(AnnAssignStmt& node) override;
    void visit(IfStmt& node) override;
    void visit(WhileStmt& node) override;
    void visit(ForStmt& node) override;
    void visit(TryStmt& node) override;
    void visit(WithStmt& node) override;
    void visit(ThreadStmt& node) override;
    void visit(MatchStmt& node) override;
    void visit(ReturnStmt& node) override;
    void visit(RaiseStmt& node) override;
    void visit(BreakStmt& node) override;
    void visit(ContinueStmt& node) override;
    void visit(PassStmt& node) override;
    void visit(AssertStmt& node) override;
    void visit(GlobalStmt& node) override;
    void visit(NonlocalStmt& node) override;
    void visit(DeleteStmt& node) override;
    void visit(ImportStmt& node) override;
    void visit(FromImportStmt& node) override;

    // Declarations
    void visit(FunctionDecl& node) override;
    void visit(ClassDecl& node) override;
    void visit(TypeAliasStmt& node) override;

    // Module
    void visit(Module& node) override;

private:
    std::string output_;
    int indent_ = 0;

    void write(const std::string& text);
    void writeLine(const std::string& text);
    void increaseIndent();
    void decreaseIndent();
    std::string indentStr() const;
    void printPattern(MatchPattern& pat);
};

} // namespace dragon

#endif // DRAGON_AST_H
