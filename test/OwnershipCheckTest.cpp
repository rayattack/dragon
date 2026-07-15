// Tests for the ownership pass (del / own / dub, docs/001-memory.md + the
// docs/002 ADR). Must-not-compile programs (the ADR's adversarial acceptance
// list) drive the pass directly; the must-compile guards prove the discipline
// is opt-in and refusal never hits correct unannotated code. Runtime behavior
// (early release, the debug rc==1 tripwire) is covered by the dogfooded
// test/dr/test_rc_del.dr and scratchpad tripwire probes - value-assertable
// runtime behavior stays in .dr tests per the testing conventions; this gtest
// exists because a program that must be REJECTED cannot be a .dr unittest

#include "TestHelpers.h"
#include "dragon/OwnershipCheck.h"
#include <gtest/gtest.h>

using namespace dragon;
using namespace dragon::test;

namespace {

// Parse + Sema + TypeChecker (the pass reads expression types) + ownership.
// Returns the first diagnostic message, or "" when the program is accepted.
std::string ownError(const std::string& src) {
    auto mod = parse(src, /*isDragon=*/true);
    Sema sema;
    sema.analyze(*mod);
    TypeChecker tc;
    tc.check(*mod);
    OwnershipCheck oc;
    if (oc.analyze(*mod)) return "";
    return oc.diagnostics().empty() ? "<no message>"
                                    : oc.diagnostics()[0].message;
}

bool ownAccepts(const std::string& src) { return ownError(src).empty(); }

} // namespace

//===----------------------------------------------------------------------===//
// The adversarial acceptance list (ADR section 5) - each must refuse with a
// diagnostic naming the cause.
//===----------------------------------------------------------------------===//

TEST(OwnershipCheckTest, DelAfterContainerStoreErrors) {
    std::string e = ownError(
        "def f() -> int {\n"
        "    cache: dict[str, str] = {}\n"
        "    buf: str = \"abc\" + \"def\"\n"
        "    cache[\"k\"] = buf\n"
        "    del buf\n"
        "    return 1\n"
        "}\n");
    EXPECT_NE(e.find("escaped into"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfCapturedLocalErrors) {
    std::string e = ownError(
        "def f() -> int {\n"
        "    x: str = \"a\" + \"b\"\n"
        "    g: Callable[[], str] = lambda () -> str { return x }\n"
        "    del x\n"
        "    return 1\n"
        "}\n");
    EXPECT_NE(e.find("captured"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfAliasedOwnerErrors) {
    // Q2 (signed off): one owner, one name - ambiguity is a compile error.
    std::string e = ownError(
        "def f() -> int {\n"
        "    x: str = \"a\" + \"b\"\n"
        "    y: str = x\n"
        "    del x\n"
        "    return len(y)\n"
        "}\n");
    EXPECT_NE(e.find("alias"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DoubleDelErrors) {
    std::string e = ownError(
        "def f() -> int {\n"
        "    x: str = \"a\" + \"b\"\n"
        "    del x\n"
        "    del x\n"
        "    return 1\n"
        "}\n");
    EXPECT_NE(e.find("already deleted"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfBorrowedElementErrors) {
    std::string e = ownError(
        "def f() -> int {\n"
        "    xs: list[str] = [\"aa\", \"bb\"]\n"
        "    e: str = xs[0]\n"
        "    del e\n"
        "    return 1\n"
        "}\n");
    EXPECT_NE(e.find("not the owner"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfFieldErrors) {
    std::string e = ownError(
        "class C {\n"
        "    buf: str = \"\"\n"
        "    def clear() {\n"
        "        del self.buf\n"
        "    }\n"
        "}\n");
    EXPECT_NE(e.find("field"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ConditionalDelThenUseErrors) {
    std::string e = ownError(
        "def f(c: bool) -> int {\n"
        "    x: str = \"a\" + \"b\"\n"
        "    if c {\n"
        "        del x\n"
        "    }\n"
        "    return len(x)\n"
        "}\n");
    EXPECT_NE(e.find("every path"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfOuterBindingInLoopErrors) {
    std::string e = ownError(
        "def f() -> int {\n"
        "    x: str = \"a\" + \"b\"\n"
        "    i: int = 0\n"
        "    while i < 3 {\n"
        "        del x\n"
        "        i = i + 1\n"
        "    }\n"
        "    return 1\n"
        "}\n");
    EXPECT_NE(e.find("iteration"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfModuleGlobalErrors) {
    std::string e = ownError("g: str = \"a\" + \"b\"\ndel g\n");
    EXPECT_NE(e.find("module global"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, UseAfterDelErrors) {
    std::string e = ownError(
        "def f() -> int {\n"
        "    x: str = \"a\" + \"b\"\n"
        "    del x\n"
        "    return len(x)\n"
        "}\n");
    EXPECT_NE(e.find("was deleted"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfPlainParamErrors) {
    // Plain parameters are borrows (ADR 2.1); `own p` arrives in slice C.
    std::string e = ownError(
        "def f(s: str) -> int {\n"
        "    del s\n"
        "    return 1\n"
        "}\n");
    EXPECT_NE(e.find("not the owner"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfWithSubjectErrors) {
    // The with statement owns its subject's release (__exit__/cleanup).
    std::string e = ownError(
        "def f() -> int {\n"
        "    with open(\"x\") as r {\n"
        "        del r\n"
        "    }\n"
        "    return 1\n"
        "}\n");
    EXPECT_NE(e.find("not the owner"), std::string::npos) << e;
}

// NOTE deliberately ABSENT here: must-COMPILE guard cases (del after a plain
// call, rebind after del, del on every branch, loop-local del, container-
// element del, unannotated code). Those are real compilable Dragon and live
// as dogfooded runtime tests in test/dr/test_rc_del.dr and
// test/dr/test_rc_own_fields.dr - if one of them ever regressed into a
// refusal, that .dr test would fail to compile and the dr ctest tier would
// catch it. Only programs that must be REJECTED belong in this file.

//===----------------------------------------------------------------------===//
// own fields (slice B) - rejection cases only. Everything that COMPILES is
// dogfooded (test/dr/test_rc_del.dr, test/dr/test_rc_own_fields.dr) per the
// testing conventions; this suite exists solely because a program that must
// be REJECTED cannot be a .dr unittest. The no-registered-releaser case is a
// CODEGEN error (Classes.cpp), exercised at driver level, not this harness.
// This harness type-checks in isolation (no module registry), so rejection
// fixtures use import-free types (str), not threading.Lock
//===----------------------------------------------------------------------===//

TEST(OwnershipCheckTest, OwnFieldBorrowStoreErrors) {
    std::string e = ownError(
        "class Box {\n"
        "    own _s: str\n"
        "    def(s: str) {\n"
        "        self._s = s\n"
        "    }\n"
        "}\n");
    EXPECT_NE(e.find("sole ownership"), std::string::npos) << e;
}

// The own-transfer slice: a bare name that is a LIVE sole owner - an `own`
// parameter or a fresh-owned local - stored into an own field is an IMPLICIT
// move, no `own` keyword needed at the store (matches the book's
// `self._data = d`). Compiles.
TEST(OwnershipCheckTest, OwnParamPlainStoreIntoOwnFieldCompiles) {
    EXPECT_TRUE(ownAccepts(
        "class Box {\n"
        "    own _s: str\n"
        "    def(own s: str) {\n"
        "        self._s = s\n"
        "    }\n"
        "}\n"));
}

TEST(OwnershipCheckTest, FreshOwnedLocalPlainStoreIntoOwnFieldCompiles) {
    EXPECT_TRUE(ownAccepts(
        "class Box {\n"
        "    own _s: str\n"
        "    def() {\n"
        "        v: str = \"a\" + \"b\"\n"
        "        self._s = v\n"
        "    }\n"
        "}\n"));
}

// The implicit move CONSUMES the name: a later use is use-after-move, exactly
// as with an explicit `own`.
TEST(OwnershipCheckTest, PlainStoreIntoOwnFieldThenUseIsUseAfterMove) {
    std::string e = ownError(
        "class Box {\n"
        "    own _s: str\n"
        "    def(own s: str) -> int {\n"
        "        self._s = s\n"
        "        return len(s)\n"
        "    }\n"
        "}\n");
    EXPECT_NE(e.find("was moved into"), std::string::npos) << e;
}

// An ALIASED/escaped owner is not a sole owner, so a plain store into a
// sole-owner field is still refused (only a live sole owner moves).
TEST(OwnershipCheckTest, PlainStoreOfEscapedOwnerIntoOwnFieldErrors) {
    std::string e = ownError(
        "class Box {\n"
        "    own _s: str\n"
        "    def(own s: str, sink: dict[str, str]) {\n"
        "        sink[\"k\"] = s\n"
        "        self._s = s\n"
        "    }\n"
        "}\n");
    EXPECT_NE(e.find("sole ownership"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, OwnScalarFieldErrors) {
    std::string e = ownError(
        "class Box {\n"
        "    own n: int = 0\n"
        "    def() { self.n = 1 }\n"
        "}\n");
    EXPECT_NE(e.find("scalar"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, OwnLocalErrors) {
    std::string e = ownError(
        "def f() {\n"
        "    own x: str = \"a\"\n"
        "}\n");
    EXPECT_NE(e.find("class FIELD"), std::string::npos) << e;
}

// ADR 2.10 (acceptance case 11): a raw-resource field without own has no
// owner to destroy it - both the declared and the ctor-assigned-only shape.
TEST(OwnershipCheckTest, PlainLockFieldErrors) {
    std::string e = ownError(
        "class Router {\n"
        "    _storage_lock: Lock\n"
        "    def() { self._storage_lock = Lock() }\n"
        "}\n");
    EXPECT_NE(e.find("must be declared own"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, UndeclaredLockFieldStoreErrors) {
    std::string e = ownError(
        "class Cache {\n"
        "    def() {\n"
        "        self.lk = Lock()\n"
        "    }\n"
        "}\n");
    EXPECT_NE(e.find("must be declared own"), std::string::npos) << e;
}

// ADR 2.10 (acceptance case 12): raw resources cannot be container elements.
TEST(OwnershipCheckTest, ContainerOfLockErrors) {
    std::string e = ownError("locks: list[Lock] = []\n");
    EXPECT_NE(e.find("cannot hold raw Lock"), std::string::npos) << e;
}

// ADR 2.5 corrected + acceptance case 13: consumption must agree at the JOIN,
// with or without a later use (a disagreement would need a runtime drop flag)
TEST(OwnershipCheckTest, ConditionalDelNoUseErrorsAtJoin) {
    std::string e = ownError(
        "def branchy(cond: bool) {\n"
        "    x: str = \"a\" + \"b\"\n"
        "    if cond {\n"
        "        del x\n"
        "    }\n"
        "}\n");
    EXPECT_NE(e.find("every path"), std::string::npos) << e;
}

//===----------------------------------------------------------------------===//
// Slice C: own parameters and moves (ADR 2.8, corrected 2.5).
//===----------------------------------------------------------------------===//

TEST(OwnershipCheckTest, UseAfterMoveErrors) {
    std::string e = ownError(
        "def consume(own s: str) -> int { return len(s) }\n"
        "def f() -> int {\n"
        "    b: str = \"a\" + \"b\"\n"
        "    n: int = consume(own b)\n"
        "    return n + len(b)\n"
        "}\n");
    EXPECT_NE(e.find("was moved into"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ConditionalMoveNoUseErrorsAtJoin) {
    // ADR acceptance case 13: no later use required; the fix is else { del x }.
    std::string e = ownError(
        "def consume(own s: str) -> int { return len(s) }\n"
        "def f(c: bool) {\n"
        "    x: str = \"a\" + \"b\"\n"
        "    if c {\n"
        "        consume(own x)\n"
        "    }\n"
        "}\n");
    EXPECT_NE(e.find("every path"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, MoveToAliasDestinationErrors) {
    std::string e = ownError(
        "def f() {\n"
        "    x: str = \"a\" + \"b\"\n"
        "    y: str = own x\n"
        "}\n");
    EXPECT_NE(e.find("consuming destination"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DoubleMoveErrors) {
    std::string e = ownError(
        "def consume(own s: str) -> int { return len(s) }\n"
        "def f() {\n"
        "    x: str = \"a\" + \"b\"\n"
        "    consume(own x)\n"
        "    consume(own x)\n"
        "}\n");
    EXPECT_NE(e.find("already moved"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, MoveOfEscapedOwnerErrors) {
    std::string e = ownError(
        "def consume(own s: str) -> int { return len(s) }\n"
        "def f() {\n"
        "    cache: dict[str, str] = {}\n"
        "    x: str = \"a\" + \"b\"\n"
        "    cache[\"k\"] = x\n"
        "    consume(own x)\n"
        "}\n");
    EXPECT_NE(e.find("escaped into"), std::string::npos) << e;
}

//===----------------------------------------------------------------------===//
// Slice D: dub + E17 (ADR 2.7, 2.11 - the one mandatory-dub site).
//===----------------------------------------------------------------------===//

TEST(OwnershipCheckTest, MutationDuringIterationErrors) {
    // The live-reproduced footgun: remove() shifts the next element past the
    // loop cursor and the wrong list survives, silently.
    std::string e = ownError(
        "def f() {\n"
        "    names: list[str] = [\"a\", \"tmp1\", \"tmp2\", \"b\"]\n"
        "    for name in names {\n"
        "        names.remove(name)\n"
        "    }\n"
        "}\n");
    EXPECT_NE(e.find("observe its own mutations"), std::string::npos) << e;
    EXPECT_NE(e.find("dub names"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, SubscriptStoreDuringIterationErrors) {
    std::string e = ownError(
        "def f() {\n"
        "    xs: list[int] = [1, 2, 3]\n"
        "    for x in xs {\n"
        "        xs[0] = x\n"
        "    }\n"
        "}\n");
    EXPECT_NE(e.find("observe its own mutations"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, MutatingADifferentContainerCompiles) {
    // Only the ITERATED binding is protected (ADR: v1 scope).
    EXPECT_TRUE(ownAccepts(
        "def f() {\n"
        "    xs: list[int] = [1, 2, 3]\n"
        "    out: list[int] = []\n"
        "    for x in xs {\n"
        "        out.append(x)\n"
        "    }\n"
        "}\n"));
}

TEST(OwnershipCheckTest, DubImmutableIterableErrors) {
    std::string e = ownError(
        "def f() {\n"
        "    s: str = \"abc\" + \"def\"\n"
        "    for c in dub s {\n"
        "        print(c)\n"
        "    }\n"
        "}\n");
    EXPECT_NE(e.find("immutable"), std::string::npos) << e;
}

//===----------------------------------------------------------------------===//
// Slice E: the spawn boundary (ADR 2.9, E12) with the joined-Task borrow
// door - LENT is a checker state (a Dead flavor with the machine's only
// backwards transition at await/join), not a keyword.
//===----------------------------------------------------------------------===//

TEST(OwnershipCheckTest, TouchWhileLentErrors) {
    std::string e = ownError(
        "class Op { name: str\n def(n: str) { self.name = n } }\n"
        "def work(o: Op) -> int { o.name = \"x\"\n return len(o.name) }\n"
        "def f() -> int {\n"
        "    o: Op = Op(\"inc\")\n"
        "    t: Task[int] = fire work(o)\n"
        "    n: int = len(o.name)\n"
        "    const r: int = await t\n"
        "    return r + n\n"
        "}\n");
    EXPECT_NE(e.find("lent to"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DiscardedHandleBorrowErrors) {
    std::string e = ownError(
        "class Op { name: str\n def(n: str) { self.name = n } }\n"
        "def work(o: Op) -> int { o.name = \"x\"\n return len(o.name) }\n"
        "def f() {\n"
        "    o: Op = Op(\"inc\")\n"
        "    fire work(o)\n"
        "}\n");
    EXPECT_NE(e.find("crosses a thread boundary"), std::string::npos) << e;
    EXPECT_NE(e.find("discarded"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, StillLentAtScopeEndErrors) {
    std::string e = ownError(
        "class Op { name: str\n def(n: str) { self.name = n } }\n"
        "def work(o: Op) -> int { o.name = \"x\"\n return len(o.name) }\n"
        "def f() {\n"
        "    o: Op = Op(\"inc\")\n"
        "    t: Task[int] = fire work(o)\n"
        "}\n");
    EXPECT_NE(e.find("still lent"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, TaskRebindWhileLentErrors) {
    std::string e = ownError(
        "class Op { name: str\n def(n: str) { self.name = n } }\n"
        "def work(o: Op) -> int { o.name = \"x\"\n return len(o.name) }\n"
        "def f() -> int {\n"
        "    o: Op = Op(\"inc\")\n"
        "    t: Task[int] = fire work(o)\n"
        "    t = fire work(o)\n"
        "    return await t\n"
        "}\n");
    EXPECT_NE(e.find("rebound while"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, LendThenAwaitThenUseCompiles) {
    EXPECT_TRUE(ownAccepts(
        "class Op { name: str\n def(n: str) { self.name = n } }\n"
        "def work(o: Op) -> int { return len(o.name) }\n"
        "def f() -> int {\n"
        "    o: Op = Op(\"inc\")\n"
        "    t: Task[int] = fire work(o)\n"
        "    const r: int = await t\n"
        "    return r + len(o.name)\n"
        "}\n"));
}

//===----------------------------------------------------------------------===//
// ADR 2.9 door 5 - read-only shared borrow across `fire`.
// A plain `fire worker(shared)` that COMPILES proves `worker` only reads its
// parameter; a mutating worker misses the door and keeps the lend/E12 rules.
//===----------------------------------------------------------------------===//

// The worker-pool fan-out: one task per worker over the SAME shared list,
// joined after the loop. Read-only worker -> door 5 accepts (pre-door this was
// E10 on the loop back edge).
TEST(OwnershipCheckTest, FireReadOnlyListFanOutAccepted) {
    EXPECT_TRUE(ownAccepts(
        "def worker(s: list[str]) -> int {\n"
        "    n: int = 0\n"
        "    i: int = 0\n"
        "    while i < len(s) {\n"
        "        n = n + len(s[i])\n"
        "        i = i + 1\n"
        "    }\n"
        "    return n\n"
        "}\n"
        "def run() -> int {\n"
        "    shared: list[str] = [\"a\", \"b\"]\n"
        "    tasks: list[Task[int]] = []\n"
        "    w: int = 0\n"
        "    while w < 4 {\n"
        "        t: Task[int] = fire worker(shared)\n"
        "        tasks.append(t)\n"
        "        w = w + 1\n"
        "    }\n"
        "    total: int = 0\n"
        "    for t in tasks { total = total + t.join() }\n"
        "    return total\n"
        "}\n"));
}

// A discarded handle firing a read-only worker is also fine: the worker holds
// its own atomic-RC reference, so the value cannot dangle.
TEST(OwnershipCheckTest, FireReadOnlyDiscardedHandleAccepted) {
    EXPECT_TRUE(ownAccepts(
        "def worker(s: list[str]) -> int { return len(s) }\n"
        "def run() -> None {\n"
        "    shared: list[str] = [\"a\", \"b\"]\n"
        "    fire worker(shared)\n"
        "}\n"));
}

// The door is READ-ONLY: a worker that appends to its parameter mutates shared
// state. Fired in a loop it must still be refused (it never reaches door 5, so
// the loop back-edge lend kills it) - dub/own is the answer.
TEST(OwnershipCheckTest, FireMutatingWorkerInLoopRejected) {
    std::string e = ownError(
        "def mutate(s: list[int]) -> int {\n"
        "    s.append(1)\n"
        "    return len(s)\n"
        "}\n"
        "def run() -> int {\n"
        "    shared: list[int] = [1, 2, 3]\n"
        "    tasks: list[Task[int]] = []\n"
        "    w: int = 0\n"
        "    while w < 4 {\n"
        "        t: Task[int] = fire mutate(shared)\n"
        "        tasks.append(t)\n"
        "        w = w + 1\n"
        "    }\n"
        "    return len(tasks)\n"
        "}\n");
    EXPECT_FALSE(e.empty()) << "mutating worker fan-out must be refused";
}

// A read-only share ESCAPES: the value cannot be `del`'d while a thread may
// still be reading it. This must be refused even though the worker is read-only.
TEST(OwnershipCheckTest, DelAfterReadOnlyShareRejected) {
    std::string e = ownError(
        "def worker(s: list[str]) -> int { return len(s) }\n"
        "def run() -> int {\n"
        "    shared: list[str] = [\"a\", \"b\"]\n"
        "    t: Task[int] = fire worker(shared)\n"
        "    del shared\n"
        "    return t.join()\n"
        "}\n");
    EXPECT_FALSE(e.empty()) << "del of a value shared into a thread must refuse";
}

//===----------------------------------------------------------------------===//
// defer: scope-exit calls with ownership. `defer f(own x)` moves x at the
// STATEMENT; any later use is the same E-class as use-after-move. A pending
// defer PINS every binding it references (args and receiver): a later own
// move or del of a pinned binding would leave the defer holding a value
// somebody else now owns, so it must refuse at compile time.
//===----------------------------------------------------------------------===//

TEST(OwnershipCheckTest, UseAfterDeferOwnMoveErrors) {
    std::string e = ownError(
        "def sink(own d: list[int]) -> None { }\n"
        "def f() -> int {\n"
        "    d: list[int] = [1, 2]\n"
        "    defer sink(own d)\n"
        "    return len(d)\n"
        "}\n");
    EXPECT_NE(e.find("was moved into"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DoubleMoveAfterDeferOwnErrors) {
    std::string e = ownError(
        "def sink(own d: list[int]) -> None { }\n"
        "def f() -> None {\n"
        "    d: list[int] = [1, 2]\n"
        "    defer sink(own d)\n"
        "    sink(own d)\n"
        "}\n");
    EXPECT_FALSE(e.empty()) << "second move of a defer-moved binding must refuse";
}

TEST(OwnershipCheckTest, OwnMoveOfDeferPinnedArgErrors) {
    std::string e = ownError(
        "def use(d: list[int]) -> None { }\n"
        "def sink(own d: list[int]) -> None { }\n"
        "def f() -> None {\n"
        "    d: list[int] = [1, 2]\n"
        "    defer use(d)\n"
        "    sink(own d)\n"
        "}\n");
    EXPECT_NE(e.find("pending defer"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, OwnMoveOfDeferredReceiverErrors) {
    std::string e = ownError(
        "class R {\n"
        "    def() { }\n"
        "    def close() -> None { }\n"
        "}\n"
        "def sink(own r: R) -> None { }\n"
        "def f() -> None {\n"
        "    r: R = R()\n"
        "    defer r.close()\n"
        "    sink(own r)\n"
        "}\n");
    EXPECT_NE(e.find("pending defer"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfDeferPinnedBindingErrors) {
    std::string e = ownError(
        "def use(d: list[int]) -> None { }\n"
        "def f() -> None {\n"
        "    d: list[int] = [1, 2]\n"
        "    defer use(d)\n"
        "    del d\n"
        "}\n");
    EXPECT_NE(e.find("pending defer"), std::string::npos) << e;
}

// The pin dies with the defer's scope: after the block exits, the defer has
// already run and the binding is movable again.
TEST(OwnershipCheckTest, PinExpiresWithDeferScope) {
    EXPECT_TRUE(ownAccepts(
        "def use(d: list[int]) -> None { }\n"
        "def sink(own d: list[int]) -> None { }\n"
        "def f(flag: bool) -> None {\n"
        "    d: list[int] = [1, 2]\n"
        "    if flag {\n"
        "        defer use(d)\n"
        "    }\n"
        "    sink(own d)\n"
        "}\n"));
}

TEST(OwnershipCheckTest, DeferBorrowThenContinuedUseAccepted) {
    EXPECT_TRUE(ownAccepts(
        "def use(d: list[int]) -> None { }\n"
        "def f() -> int {\n"
        "    d: list[int] = [1, 2]\n"
        "    defer use(d)\n"
        "    d.append(3)\n"
        "    return len(d)\n"
        "}\n"));
}

TEST(OwnershipCheckTest, DeferOwnWithNoLaterUseAccepted) {
    EXPECT_TRUE(ownAccepts(
        "def sink(own d: list[int]) -> None { }\n"
        "def f() -> None {\n"
        "    d: list[int] = [1, 2]\n"
        "    defer sink(own d)\n"
        "}\n"));
}

TEST(OwnershipCheckTest, DeferDubLeavesSourceLive) {
    EXPECT_TRUE(ownAccepts(
        "def use(d: list[int]) -> None { }\n"
        "def sink(own d: list[int]) -> None { }\n"
        "def f() -> None {\n"
        "    d: list[int] = [1, 2]\n"
        "    defer use(dub d)\n"
        "    sink(own d)\n"
        "}\n"));
}

//===----------------------------------------------------------------------===//
// Identity resources (docs/1604 "Identity resources": the SocketHandle
// shape). A handle class holds the one claim; an owner class takes it via an
// own-param constructor. Each promise the book makes is a bouncer case here:
// the borrow-forge, the stale toucher, the second claimant, and the copy all
// get refused with a diagnostic naming the cause. The must-compile guards
// prove the blessed spellings (fresh temporary, explicit move) stay friction-
// free. Runtime poison semantics live in test/dr/test_socket_handle.dr.
//===----------------------------------------------------------------------===//

namespace {

// The minimal identity-resource pair, shared by the cases below.
const char* kHandlePair =
    "class H {\n"
    "    _fd: int\n"
    "    def(fd: int) { self._fd = fd }\n"
    "    def fd() -> int { return self._fd }\n"
    "}\n"
    "class R {\n"
    "    own _h: H\n"
    "    def(own h: H) { self._h = h }\n"
    "    def probe() -> int { return self._h.fd() }\n"
    "}\n";

std::string withPair(const std::string& body) {
    return std::string(kHandlePair) + body;
}

} // namespace

// The borrow-forge ("constructor takes ownership of its argument") and the
// copy ("not dubable") are TYPECHECKER diagnostics, so those two bouncer
// cases live in TypeCheckerTest.cpp (IdentityResource*). This suite owns the
// move/claim cases below.

// The stale toucher: the handle was moved into the reader; the old binding
// is dead and every later use says so.
TEST(OwnershipCheckTest, OwnCtorMoveThenUseErrors) {
    std::string e = ownError(withPair(
        "def f() -> int {\n"
        "    h: H = H(4)\n"
        "    r: R = R(own h)\n"
        "    return h.fd()\n"
        "}\n"));
    EXPECT_NE(e.find("was moved into"), std::string::npos) << e;
}

// The second claimant: one claim cannot be moved into two owners.
TEST(OwnershipCheckTest, OwnCtorDoubleMoveErrors) {
    std::string e = ownError(withPair(
        "def f() -> int {\n"
        "    h: H = H(4)\n"
        "    r1: R = R(own h)\n"
        "    r2: R = R(own h)\n"
        "    return r1.probe()\n"
        "}\n"));
    EXPECT_NE(e.find("already moved"), std::string::npos) << e;
}

// Guard: a FRESH temporary (the adopt_raw shape) carries its own +1 into the
// own-param constructor with no `own` spelled at the call site.
TEST(OwnershipCheckTest, OwnCtorFreshTemporaryAccepted) {
    EXPECT_TRUE(ownAccepts(withPair(
        "def f() -> int {\n"
        "    r: R = R(H(4))\n"
        "    return r.probe()\n"
        "}\n")));
}

// Guard: the explicit move with no later use of the source compiles, and the
// new owner reads through its claim.
TEST(OwnershipCheckTest, OwnCtorMoveAccepted) {
    EXPECT_TRUE(ownAccepts(withPair(
        "def f() -> int {\n"
        "    h: H = H(4)\n"
        "    r: R = R(own h)\n"
        "    return r.probe()\n"
        "}\n")));
}
