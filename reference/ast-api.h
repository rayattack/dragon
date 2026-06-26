/**
 * Dragon AST API Reference
 * ========================
 * Source: include/dragon/AST.h
 *
 * Defines ~52 AST node types organized in a class hierarchy:
 *  ASTNode (base) -> Expr, Stmt, TypeExpr
 *
 * All nodes carry a SourceLocation for error reporting.
 * All nodes implement accept(ASTVisitor&) for the visitor pattern.
 * Ownership is via std::unique_ptr<>; types use std::shared_ptr<Type>.
 *
 * The ASTVisitor base class defines 56 pure virtual visit() methods.
 * DefaultASTVisitor provides recursive traversal; ASTPrinter provides debug output.
 */

#pragma once
#include <string>
#include <vector>
#include <memory>
#include <optional>

// Forward declarations
struct SourceLocation;
class Type;
class Token;
class ASTVisitor;

// ============================================================================
// 1. BASE CLASSES
// ============================================================================

/**
 * Root of the AST hierarchy. All nodes inherit from this.
 * Provides source location tracking and visitor dispatch.
 */
class ASTNode {
public:
    SourceLocation location_;  ///< Where in source this node was parsed

    virtual void accept(ASTVisitor& visitor) = 0;
    virtual ~ASTNode() = default;
};

/**
 * Base class for all expression nodes.
 * Carries an inferred/declared type after type checking.
 */
class Expr : public ASTNode {
public:
    std::shared_ptr<Type> type;  ///< Inferred or declared type (set by TypeChecker)
};

/** Base class for all statement nodes. */
class Stmt : public ASTNode {};

/** Base class for all type annotation expression nodes. */
class TypeExpr : public ASTNode {};


// ============================================================================
// 2. TYPE EXPRESSIONS (6 types)
// ============================================================================

/** Simple type name: int, str, float, bool, MyClass, etc. */
class NamedTypeExpr : public TypeExpr {
public:
    std::string name;  ///< Type name text
};

/** Parameterized generic type: list[int], dict[str, int], Optional[str]. */
class GenericTypeExpr : public TypeExpr {
public:
    std::unique_ptr<TypeExpr> base;                        ///< Base type (e.g., "list")
    std::vector<std::unique_ptr<TypeExpr>> typeArgs;       ///< Type parameters (e.g., [int])
};

/** Optional type: int? or Optional[int] (sugar for int | None). */
class OptionalTypeExpr : public TypeExpr {
public:
    std::unique_ptr<TypeExpr> inner;  ///< The wrapped type
};

/** Union type: int | str | None. */
class UnionTypeExpr : public TypeExpr {
public:
    std::vector<std::unique_ptr<TypeExpr>> types;  ///< Union members
};

/** Callable/function type: Callable[[int, str], bool]. */
class CallableTypeExpr : public TypeExpr {
public:
    std::vector<std::unique_ptr<TypeExpr>> paramTypes;  ///< Parameter types
    std::unique_ptr<TypeExpr> returnType;                ///< Return type
};

/** Tuple type: tuple[int, str, bool]. */
class TupleTypeExpr : public TypeExpr {
public:
    std::vector<std::unique_ptr<TypeExpr>> elementTypes;  ///< Element types (positional)
};


// ============================================================================
// 3. LITERAL EXPRESSIONS (6 types)
// ============================================================================

/** Integer literal: 42, 0xff, 0b1010, 0o77. */
class IntegerLiteral : public Expr {
public:
    int64_t value;
};

/** Float literal: 3.14, 1e-10, .5. */
class FloatLiteral : public Expr {
public:
    double value;
};

/**
 * String literal: "hello", r"raw\n", f"value={x}", b"bytes".
 * NOTE: The value includes surrounding quotes in the lexeme - strip before use.
 */
class StringLiteral : public Expr {
public:
    std::string value;  ///< String content (includes quotes from lexer)
    bool isRaw;         ///< r-prefix: no escape processing
    bool isFString;     ///< f-prefix: contains {expr} interpolation
    bool isBytes;       ///< b-prefix: bytes literal
};

/** Boolean literal: True, False. */
class BooleanLiteral : public Expr {
public:
    bool value;
};

/** None literal. */
class NoneLiteral : public Expr {};

/** Template string expression with !{expr} interpolation (Dragon-specific). */
class TemplateExpr : public Expr {
public:
    std::string body;  ///< Template body with !{...} markers
};


// ============================================================================
// 4. REFERENCE & OPERATOR EXPRESSIONS (7 types)
// ============================================================================

/** Variable/name reference: x, my_var, print. */
class NameExpr : public Expr {
public:
    std::string name;
};

/** Binary operation: a + b, x and y, a == b. */
class BinaryExpr : public Expr {
public:
    std::unique_ptr<Expr> left;
    Token op;                     ///< Operator token (PLUS, MINUS, AND, EQUAL, etc.)
    std::unique_ptr<Expr> right;
};

/** Chained comparison: a < b < c, 0 <= x < 100. Desugars to (a<b) and (b<c). */
class ChainedCompExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> operands;  ///< N+1 operands
    std::vector<Token> operators;                  ///< N operators between operands
};

/** Walrus/assignment expression: name := value. */
class WalrusExpr : public Expr {
public:
    std::string name;
    std::unique_ptr<Expr> value;
};

/** Unary operation: -x, not y, ~z. */
class UnaryExpr : public Expr {
public:
    Token op;                       ///< Operator token (MINUS, NOT, TILDE)
    std::unique_ptr<Expr> operand;
};

/** Starred expression: *args (single star), **kwargs (double star). */
class StarredExpr : public Expr {
public:
    std::unique_ptr<Expr> value;
    bool isDoubleStar;  ///< true for **kwargs, false for *args
};

/** Yield expression: yield value, yield from iterable. */
class YieldExpr : public Expr {
public:
    std::unique_ptr<Expr> value;  ///< Value to yield (may be nullptr)
    bool isYieldFrom;             ///< true for "yield from"
};


// ============================================================================
// 5. CALL & MEMBER ACCESS EXPRESSIONS (3 types)
// ============================================================================

/** Function/method call: func(a, b, key=val). */
class CallExpr : public Expr {
public:
    std::unique_ptr<Expr> callee;                                                ///< Function/method being called
    std::vector<std::unique_ptr<Expr>> args;                                     ///< Positional arguments
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> kwArgs;           ///< Keyword arguments
};

/** Attribute access: obj.attr, module.func. */
class AttributeExpr : public Expr {
public:
    std::unique_ptr<Expr> object;    ///< Object being accessed
    std::string attribute;           ///< Attribute name
};

/** Subscript/indexing: arr[idx], dict["key"]. */
class SubscriptExpr : public Expr {
public:
    std::unique_ptr<Expr> object;  ///< Container being indexed
    std::unique_ptr<Expr> index;   ///< Index expression
};


// ============================================================================
// 6. CONTAINER & SLICE EXPRESSIONS (5 types)
// ============================================================================

/** List literal: [1, 2, 3]. */
class ListExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> elements;
};

/** Tuple literal: (1, 2, 3). */
class TupleExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> elements;
};

/** Dict literal: {"a": 1, "b": 2}. */
class DictExpr : public Expr {
public:
    std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>> entries;  ///< (key, value) pairs
};

/** Set literal: {1, 2, 3}. */
class SetExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> elements;
};

/** Slice expression: arr[start:stop:step]. All fields optional (nullptr = omitted). */
class SliceExpr : public Expr {
public:
    std::unique_ptr<Expr> lower;  ///< Start index (optional)
    std::unique_ptr<Expr> upper;  ///< Stop index (optional)
    std::unique_ptr<Expr> step;   ///< Step size (optional)
};


// ============================================================================
// 7. COMPREHENSION EXPRESSIONS (4 types)
// ============================================================================

/**
 * Supporting struct for multi-clause comprehensions.
 * Represents additional "for x in iter if cond" clauses.
 */
struct CompClause {
    std::vector<std::string> varNames;    ///< Iteration variable names (supports tuple unpacking)
    std::unique_ptr<Expr> iterable;       ///< Iterable expression
    std::unique_ptr<Expr> condition;      ///< Optional filter condition
};

/** List comprehension: [x*2 for x in items if x > 0]. */
class ListCompExpr : public Expr {
public:
    std::unique_ptr<Expr> element;              ///< Output expression
    std::string varName;                        ///< Primary iteration variable
    std::unique_ptr<Expr> iterable;             ///< Primary iterable
    std::unique_ptr<Expr> condition;            ///< Primary filter (optional)
    std::vector<CompClause> extraClauses;       ///< Additional for/if clauses
};

/** Dict comprehension: {k: v for k, v in items}. */
class DictCompExpr : public Expr {
public:
    std::unique_ptr<Expr> key;                  ///< Key output expression
    std::unique_ptr<Expr> value;                ///< Value output expression
    std::vector<std::string> varNames;          ///< Iteration variable names
    std::unique_ptr<Expr> iterable;             ///< Iterable expression
    std::unique_ptr<Expr> condition;            ///< Filter (optional)
    std::vector<CompClause> extraClauses;       ///< Additional clauses
};

/** Set comprehension: {x*2 for x in items if x > 0}. */
class SetCompExpr : public Expr {
public:
    std::unique_ptr<Expr> element;
    std::string varName;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;
    std::vector<CompClause> extraClauses;
};

/** Generator expression: (x*2 for x in items). Currently evaluated eagerly as list. */
class GeneratorExpr : public Expr {
public:
    std::unique_ptr<Expr> element;
    std::string varName;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;
    std::vector<CompClause> extraClauses;
};


// ============================================================================
// 8. LAMBDA, CONTROL FLOW, ASYNC EXPRESSIONS (4 types)
// ============================================================================

/** Lambda expression: lambda x, y: x + y (Dragon: with types and block body). */
class LambdaExpr : public Expr {
public:
    struct Parameter {
        std::string name;
        std::unique_ptr<TypeExpr> type;          ///< Type annotation (optional in .py)
        std::unique_ptr<Expr> defaultValue;      ///< Default value (optional)
    };

    std::vector<Parameter> params;
    std::unique_ptr<TypeExpr> returnType;         ///< Return type (optional)
    std::unique_ptr<Expr> body;                   ///< Single-expression body
    std::vector<std::unique_ptr<Stmt>> bodyStmts; ///< Block body (Dragon extended lambda)
};

/** Ternary conditional: value_if_true if condition else value_if_false. */
class IfExpr : public Expr {
public:
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Expr> thenExpr;
    std::unique_ptr<Expr> elseExpr;
};

/** Await expression: await coroutine(). Works in any def (no function coloring). */
class AwaitExpr : public Expr {
public:
    std::unique_ptr<Expr> operand;
};

/**
 * Fire expression: fire fn(args) or fire { block }.
 * Spawns a green thread (minicoro coroutine, M:N scheduled).
 */
class FireExpr : public Expr {
public:
    std::unique_ptr<Expr> operand;                  ///< Function call to spawn
    std::vector<std::unique_ptr<Stmt>> bodyStmts;   ///< Block body (fire { ... })
};


// ============================================================================
// 9. STATEMENTS - ASSIGNMENT (4 types)
// ============================================================================

/** Expression statement: func_call(), x + 1 (discarded). */
class ExprStmt : public Stmt {
public:
    std::unique_ptr<Expr> expr;
};

/** Assignment: x = 5, a, b = 1, 2, x: int = 5. */
class AssignStmt : public Stmt {
public:
    std::vector<std::unique_ptr<Expr>> targets;      ///< Assignment targets (supports multiple: a = b = 5)
    std::unique_ptr<Expr> value;                     ///< Right-hand side value
    std::unique_ptr<TypeExpr> typeAnnotation;        ///< Optional type annotation
};

/** Augmented assignment: x += 5, y *= 2. */
class AugAssignStmt : public Stmt {
public:
    std::unique_ptr<Expr> target;    ///< LHS target
    Token op;                        ///< Operator (PLUS_ASSIGN, MINUS_ASSIGN, etc.)
    std::unique_ptr<Expr> value;     ///< RHS value
};

/**
 * Annotated assignment: x: int = 5, x: int (declaration only).
 * Supports Dragon const/static modifiers.
 */
class AnnAssignStmt : public Stmt {
public:
    std::unique_ptr<Expr> target;            ///< LHS target
    std::unique_ptr<TypeExpr> annotation;    ///< Type annotation
    std::unique_ptr<Expr> value;             ///< RHS value (optional - declaration only if null)
    bool isConst;                            ///< Dragon const binding (.dr mode)
    bool isStatic;                           ///< Dragon static member (.dr mode)
};


// ============================================================================
// 10. STATEMENTS - CONTROL FLOW (4 types)
// ============================================================================

/** If/elif/else statement. */
class IfStmt : public Stmt {
public:
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Stmt>> thenBody;
    std::vector<std::pair<std::unique_ptr<Expr>,
                          std::vector<std::unique_ptr<Stmt>>>> elifClauses;  ///< (condition, body) pairs
    std::vector<std::unique_ptr<Stmt>> elseBody;                             ///< Optional else block
};

/** While loop with optional else clause. */
class WhileStmt : public Stmt {
public:
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Stmt>> elseBody;  ///< Runs if loop completes without break
};

/** For loop with optional else clause. */
class ForStmt : public Stmt {
public:
    std::unique_ptr<Expr> target;     ///< Iteration variable(s)
    std::unique_ptr<Expr> iterable;   ///< Iterable expression
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Stmt>> elseBody;
};

/** Match/case statement (PEP 634 structural pattern matching). */
class MatchStmt : public Stmt {
public:
    /** A single pattern in a match arm. */
    struct MatchPattern {
        enum class Kind { Literal, Capture, Wildcard, Sequence, Or, Class, Value };
        Kind kind;
        std::unique_ptr<Expr> literal;              ///< For Literal patterns
        std::string name;                            ///< For Capture patterns
        std::vector<MatchPattern> subPatterns;       ///< For Sequence/Or patterns
        std::unique_ptr<Expr> guard;                 ///< Optional guard expression
    };

    /** A single case arm: case pattern [if guard]: body. */
    struct MatchCase {
        MatchPattern pattern;
        std::unique_ptr<Expr> guard;                 ///< Optional guard
        std::vector<std::unique_ptr<Stmt>> body;
    };

    std::unique_ptr<Expr> subject;           ///< Value being matched
    std::vector<MatchCase> cases;            ///< Case arms
};


// ============================================================================
// 11. STATEMENTS - EXCEPTION & CONTEXT MANAGEMENT (2 types)
// ============================================================================

/** Try/except/else/finally statement. */
class TryStmt : public Stmt {
public:
    /** A single except handler: except Type as name: body. */
    struct ExceptHandler {
        std::unique_ptr<Expr> type;                  ///< Exception type (optional for bare except)
        std::string name;                            ///< Binding name (optional)
        std::vector<std::unique_ptr<Stmt>> body;
        bool isStar;                                 ///< except* (exception groups)
    };

    std::vector<std::unique_ptr<Stmt>> tryBody;
    std::vector<ExceptHandler> handlers;
    std::vector<std::unique_ptr<Stmt>> elseBody;      ///< Runs if no exception raised
    std::vector<std::unique_ptr<Stmt>> finallyBody;   ///< Always runs
};

/** With statement (context manager): with open("f") as fh: ... */
class WithStmt : public Stmt {
public:
    /** A single context manager item. */
    struct WithItem {
        std::unique_ptr<Expr> contextExpr;           ///< Context manager expression
        std::unique_ptr<Expr> optionalVars;          ///< "as" target (optional)
    };

    std::vector<WithItem> items;                     ///< One or more context managers
    std::vector<std::unique_ptr<Stmt>> body;
};


// ============================================================================
// 12. STATEMENTS - SIMPLE (9 types)
// ============================================================================

/** Return statement: return, return value. */
class ReturnStmt : public Stmt {
public:
    std::unique_ptr<Expr> value;  ///< Return value (optional)
};

/** Raise statement: raise, raise exc, raise exc from cause. */
class RaiseStmt : public Stmt {
public:
    std::unique_ptr<Expr> exception;  ///< Exception to raise (optional for bare raise)
    std::unique_ptr<Expr> cause;      ///< "from" cause (optional)
};

/** Break statement. */
class BreakStmt : public Stmt {};

/** Continue statement. */
class ContinueStmt : public Stmt {};

/** Pass statement (no-op). */
class PassStmt : public Stmt {};

/** Assert statement: assert test, assert test, message. */
class AssertStmt : public Stmt {
public:
    std::unique_ptr<Expr> test;  ///< Condition to assert
    std::unique_ptr<Expr> msg;   ///< Optional failure message
};

/** Global declaration (.py mode): global x, y. */
class GlobalStmt : public Stmt {
public:
    std::vector<std::string> names;  ///< Variable names declared global
};

/** Nonlocal declaration: nonlocal x, y. */
class NonlocalStmt : public Stmt {
public:
    std::vector<std::string> names;
};

/** Delete statement: del x, del arr[0]. */
class DeleteStmt : public Stmt {
public:
    std::vector<std::unique_ptr<Expr>> targets;  ///< Expressions to delete
};


// ============================================================================
// 13. STATEMENTS - THREADING & IMPORTS (3 types)
// ============================================================================

/** Scoped OS thread: thread { block }. Auto-joins at scope exit. */
class ThreadStmt : public Stmt {
public:
    std::vector<std::unique_ptr<Stmt>> body;
};

/** Import statement: import x, import x as y. */
class ImportStmt : public Stmt {
public:
    struct Alias {
        std::string name;                  ///< Module name
        std::optional<std::string> asName; ///< Optional alias
    };
    std::vector<Alias> names;
};

/** From-import statement: from x import y, from ..x import y as z. */
class FromImportStmt : public Stmt {
public:
    std::string module;                    ///< Module path (dotted)
    int level;                             ///< Number of dots for relative import (0 = absolute)
    std::vector<ImportStmt::Alias> names;  ///< Imported symbols
};


// ============================================================================
// 14. DECLARATIONS (3 types)
// ============================================================================

/**
 * Shared parameter struct for functions and lambdas.
 */
struct Parameter {
    std::string name;
    std::unique_ptr<TypeExpr> type;          ///< Type annotation (required in .dr, optional in .py)
    std::unique_ptr<Expr> defaultValue;      ///< Default value (optional)
    bool isVarArg = false;                   ///< *args
    bool isKwArg = false;                    ///< **kwargs
};

/**
 * Function declaration with comprehensive feature flags.
 * Covers regular functions, methods, constructors, generators, async, extern "C".
 */
class FunctionDecl : public Stmt {
public:
    std::string name;
    std::vector<Parameter> params;
    std::unique_ptr<TypeExpr> returnType;
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Expr>> decorators;  ///< @staticmethod, @classmethod, custom

    // Flags
    bool isAsync;            ///< async def (note: Dragon has colorless await, so this is rarely needed)
    bool isMethod;           ///< Defined inside a class
    bool hasImplicitSelf;    ///< First param is self (auto-added by parser)
    bool isStatic;           ///< @staticmethod
    bool isClassMethod;      ///< @classmethod
    bool isConstructor;      ///< __init__ or self() constructor
    bool isExtern;           ///< extern "C" FFI function

    std::string externLib;   ///< Library hint for extern (e.g., "m" for libm)
    int constructorIndex;    ///< Overload index for self() constructors (.dr mode)
    int posOnlyEnd;          ///< Positional-only parameter boundary (/)
    int kwOnlyStart;         ///< Keyword-only parameter boundary (*)
};

/** Class declaration with inheritance, metaclass keywords, and decorators. */
class ClassDecl : public Stmt {
public:
    std::string name;
    std::vector<std::unique_ptr<Expr>> bases;                                  ///< Base classes
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> keywords;       ///< Metaclass keywords (metaclass=X)
    std::vector<std::unique_ptr<Stmt>> body;                                   ///< Class body
    std::vector<std::unique_ptr<Expr>> decorators;                             ///< Class decorators
};

/** Type alias: type Point = tuple[int, int] (PEP 695). */
class TypeAliasStmt : public Stmt {
public:
    std::string name;
    std::unique_ptr<TypeExpr> value;
};


// ============================================================================
// 15. MODULE (root node)
// ============================================================================

/**
 * Complete Dragon/Python module - the root AST node.
 * One per source file.
 */
class Module : public ASTNode {
public:
    std::string filename;      ///< Source file path
    std::string moduleName;    ///< Module name (e.g., "math_utils", "os.path")
    bool isDragonFile;         ///< true = .dr mode, false = .py mode
    std::vector<std::unique_ptr<Stmt>> body;  ///< Top-level statements
};


// ============================================================================
// 16. AST VISITOR INTERFACE (56 pure virtual methods)
// ============================================================================

/**
 * Base visitor for double dispatch over all AST node types.
 * Implemented by Sema, TypeChecker, CodeGen, ASTPrinter, etc.
 *
 * Methods grouped:
 *  6 type expression visitors
 *  29 expression visitors (literals + operators + containers + comprehensions + async)
 *  18 statement visitors (assignment + control flow + exception + simple + imports)
 *  3 declaration visitors (function, class, type alias)
 *  1 module visitor
 */
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

    // Literal expressions
    virtual void visit(IntegerLiteral& node) = 0;
    virtual void visit(FloatLiteral& node) = 0;
    virtual void visit(StringLiteral& node) = 0;
    virtual void visit(BooleanLiteral& node) = 0;
    virtual void visit(NoneLiteral& node) = 0;

    // Operator & reference expressions
    virtual void visit(NameExpr& node) = 0;
    virtual void visit(BinaryExpr& node) = 0;
    virtual void visit(ChainedCompExpr& node) = 0;
    virtual void visit(WalrusExpr& node) = 0;
    virtual void visit(UnaryExpr& node) = 0;

    // Call & access expressions
    virtual void visit(CallExpr& node) = 0;
    virtual void visit(AttributeExpr& node) = 0;
    virtual void visit(SubscriptExpr& node) = 0;
    virtual void visit(SliceExpr& node) = 0;

    // Container expressions
    virtual void visit(ListExpr& node) = 0;
    virtual void visit(TupleExpr& node) = 0;
    virtual void visit(DictExpr& node) = 0;
    virtual void visit(SetExpr& node) = 0;

    // Comprehension expressions
    virtual void visit(ListCompExpr& node) = 0;
    virtual void visit(DictCompExpr& node) = 0;
    virtual void visit(SetCompExpr& node) = 0;
    virtual void visit(GeneratorExpr& node) = 0;

    // Special expressions
    virtual void visit(LambdaExpr& node) = 0;
    virtual void visit(IfExpr& node) = 0;
    virtual void visit(AwaitExpr& node) = 0;
    virtual void visit(FireExpr& node) = 0;
    virtual void visit(YieldExpr& node) = 0;
    virtual void visit(StarredExpr& node) = 0;
    virtual void visit(TemplateExpr& node) = 0;

    // Assignment statements
    virtual void visit(ExprStmt& node) = 0;
    virtual void visit(AssignStmt& node) = 0;
    virtual void visit(AugAssignStmt& node) = 0;
    virtual void visit(AnnAssignStmt& node) = 0;

    // Control flow statements
    virtual void visit(IfStmt& node) = 0;
    virtual void visit(WhileStmt& node) = 0;
    virtual void visit(ForStmt& node) = 0;
    virtual void visit(TryStmt& node) = 0;
    virtual void visit(WithStmt& node) = 0;
    virtual void visit(ThreadStmt& node) = 0;
    virtual void visit(MatchStmt& node) = 0;

    // Simple statements
    virtual void visit(ReturnStmt& node) = 0;
    virtual void visit(RaiseStmt& node) = 0;
    virtual void visit(BreakStmt& node) = 0;
    virtual void visit(ContinueStmt& node) = 0;
    virtual void visit(PassStmt& node) = 0;
    virtual void visit(AssertStmt& node) = 0;
    virtual void visit(GlobalStmt& node) = 0;
    virtual void visit(NonlocalStmt& node) = 0;
    virtual void visit(DeleteStmt& node) = 0;

    // Import statements
    virtual void visit(ImportStmt& node) = 0;
    virtual void visit(FromImportStmt& node) = 0;

    // Declarations
    virtual void visit(FunctionDecl& node) = 0;
    virtual void visit(ClassDecl& node) = 0;
    virtual void visit(TypeAliasStmt& node) = 0;

    // Module
    virtual void visit(Module& node) = 0;
};
