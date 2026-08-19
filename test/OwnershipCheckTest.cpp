#include "TestHelpers.h"
#include "dragon/OwnershipCheck.h"
#include <gtest/gtest.h>

using namespace dragon;
using namespace dragon::test;

namespace {

std::string ownError(const std::string& src) {
    auto mod = parse(src, true);
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

}

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
    std::string e = ownError(
        "def f(s: str) -> int {\n"
        "    del s\n"
        "    return 1\n"
        "}\n");
    EXPECT_NE(e.find("not the owner"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfWithSubjectErrors) {
    std::string e = ownError(
        "def f() -> int {\n"
        "    with open(\"x\") as r {\n"
        "        del r\n"
        "    }\n"
        "    return 1\n"
        "}\n");
    EXPECT_NE(e.find("not the owner"), std::string::npos) << e;
}

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

TEST(OwnershipCheckTest, ContainerOfLockErrors) {
    std::string e = ownError("locks: list[Lock] = []\n");
    EXPECT_NE(e.find("cannot hold raw Lock"), std::string::npos) << e;
}

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

TEST(OwnershipCheckTest, MutationDuringIterationErrors) {
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

TEST(OwnershipCheckTest, FireReadOnlyDiscardedHandleAccepted) {
    EXPECT_TRUE(ownAccepts(
        "def worker(s: list[str]) -> int { return len(s) }\n"
        "def run() -> None {\n"
        "    shared: list[str] = [\"a\", \"b\"]\n"
        "    fire worker(shared)\n"
        "}\n"));
}

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

// `defer f(own x)` moves x at the STATEMENT (a later use is the use-after-move E-class); a pending
// defer PINS every referenced binding, so a later own move or del of it must refuse at compile time.

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

namespace {

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

}

TEST(OwnershipCheckTest, OwnCtorMoveThenUseErrors) {
    std::string e = ownError(withPair(
        "def f() -> int {\n"
        "    h: H = H(4)\n"
        "    r: R = R(own h)\n"
        "    return h.fd()\n"
        "}\n"));
    EXPECT_NE(e.find("was moved into"), std::string::npos) << e;
}

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

TEST(OwnershipCheckTest, OwnCtorFreshTemporaryAccepted) {
    EXPECT_TRUE(ownAccepts(withPair(
        "def f() -> int {\n"
        "    r: R = R(H(4))\n"
        "    return r.probe()\n"
        "}\n")));
}

TEST(OwnershipCheckTest, OwnCtorMoveAccepted) {
    EXPECT_TRUE(ownAccepts(withPair(
        "def f() -> int {\n"
        "    h: H = H(4)\n"
        "    r: R = R(own h)\n"
        "    return r.probe()\n"
        "}\n")));
}

TEST(OwnershipCheckTest, DelOfLentTaskErrorsAtDelSite) {
    std::string e = ownError(
        "class Counter {\n"
        "    n: int\n"
        "    def(n: int) {\n"
        "        self.n = n\n"
        "    }\n"
        "}\n"
        "def worker(c: Counter) -> int {\n"
        "    c.n = c.n + 1\n"
        "    return c.n\n"
        "}\n"
        "def run() -> int {\n"
        "    c: Counter = Counter(7)\n"
        "    t: Task[int] = fire worker(c)\n"
        "    del t\n"
        "    return 0\n"
        "}\n");
    EXPECT_NE(e.find("cannot del task 't'"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfUnlentTaskAccepted) {
    EXPECT_TRUE(ownAccepts(
        "async def make(n: int) -> str {\n"
        "    return \"x\"\n"
        "}\n"
        "def run() -> None {\n"
        "    t: Task[str] = make(1)\n"
        "    del t\n"
        "}\n"));
}

TEST(OwnershipCheckTest, DoubleAwaitRejected) {
    std::string e = ownError(
        "async def make(n: int) -> str {\n"
        "    return str(n)\n"
        "}\n"
        "def run() -> str {\n"
        "    t: Task[str] = make(1)\n"
        "    a: str = await t\n"
        "    b: str = await t\n"
        "    return a + b\n"
        "}\n");
    EXPECT_NE(e.find("moves out exactly once"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, AwaitThenJoinRejected) {
    std::string e = ownError(
        "async def make(n: int) -> str {\n"
        "    return str(n)\n"
        "}\n"
        "def run() -> str {\n"
        "    t: Task[str] = make(1)\n"
        "    a: str = await t\n"
        "    return t.join()\n"
        "}\n");
    EXPECT_NE(e.find("moves out exactly once"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, IsAliveAfterAwaitRejected) {
    std::string e = ownError(
        "async def make(n: int) -> str {\n"
        "    return str(n)\n"
        "}\n"
        "def run() -> bool {\n"
        "    t: Task[str] = make(1)\n"
        "    a: str = await t\n"
        "    return t.is_alive()\n"
        "}\n");
    EXPECT_NE(e.find("moves out exactly once"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, AwaitInLoopOfOuterTaskRejected) {
    std::string e = ownError(
        "async def make(n: int) -> str {\n"
        "    return str(n)\n"
        "}\n"
        "def run() -> int {\n"
        "    t: Task[str] = make(1)\n"
        "    total: int = 0\n"
        "    i: int = 0\n"
        "    while i < 3 {\n"
        "        s: str = await t\n"
        "        total = total + len(s)\n"
        "        i = i + 1\n"
        "    }\n"
        "    return total\n"
        "}\n");
    EXPECT_NE(e.find("awaited on iteration 1"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ConditionalAwaitThenSecondAwaitRejected) {
    std::string e = ownError(
        "async def make(n: int) -> str {\n"
        "    return str(n)\n"
        "}\n"
        "def run(ready: bool) -> str {\n"
        "    t: Task[str] = make(1)\n"
        "    early: str = \"\"\n"
        "    if ready {\n"
        "        early = await t\n"
        "    }\n"
        "    late: str = await t\n"
        "    return early + late\n"
        "}\n");
    EXPECT_NE(e.find("moves out exactly once"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ConditionalAwaitAloneAccepted) {
    EXPECT_TRUE(ownAccepts(
        "async def make(n: int) -> str {\n"
        "    return str(n)\n"
        "}\n"
        "def run(ready: bool) -> str {\n"
        "    t: Task[str] = make(1)\n"
        "    if ready {\n"
        "        return await t\n"
        "    }\n"
        "    return \"skipped\"\n"
        "}\n"));
}

TEST(OwnershipCheckTest, PollThenAwaitAccepted) {
    EXPECT_TRUE(ownAccepts(
        "async def make(n: int) -> str {\n"
        "    return str(n)\n"
        "}\n"
        "def run() -> str {\n"
        "    t: Task[str] = make(1)\n"
        "    while t.is_alive() {\n"
        "        pass\n"
        "    }\n"
        "    return await t\n"
        "}\n"));
}

TEST(OwnershipCheckTest, RebindThenAwaitAccepted) {
    EXPECT_TRUE(ownAccepts(
        "async def make(n: int) -> str {\n"
        "    return str(n)\n"
        "}\n"
        "def run() -> str {\n"
        "    t: Task[str] = make(1)\n"
        "    a: str = await t\n"
        "    t = make(2)\n"
        "    b: str = await t\n"
        "    return a + b\n"
        "}\n"));
}

TEST(OwnershipCheckTest, KeywordMoveMarksBindingMoved) {
    std::string e = ownError(
        "def take(own s: str) -> int { return len(s) }\n"
        "def f() -> int {\n"
        "    b: str = \"abc\" + \"def\"\n"
        "    take(s=own b)\n"
        "    return len(b)\n"
        "}\n");
    EXPECT_NE(e.find("was moved into"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, KeywordMoveWithoutReuseAccepted) {
    EXPECT_TRUE(ownAccepts(
        "def take(own s: str) -> int { return len(s) }\n"
        "def f() -> int {\n"
        "    b: str = \"abc\" + \"def\"\n"
        "    return take(s=own b)\n"
        "}\n"));
}

TEST(OwnershipCheckTest, KeywordDubKeepsBindingUsable) {
    EXPECT_TRUE(ownAccepts(
        "def borrows(s: str) -> int { return len(s) }\n"
        "def f() -> int {\n"
        "    b: str = \"abc\" + \"def\"\n"
        "    return borrows(s=dub b) + len(b)\n"
        "}\n"));
}

TEST(OwnershipCheckTest, DubIntoOwnParamKeepsBindingUsable) {
    EXPECT_TRUE(ownAccepts(
        "def take(own s: str) -> int { return len(s) }\n"
        "def f() -> int {\n"
        "    b: str = \"abc\" + \"def\"\n"
        "    return take(dub b) + len(b)\n"
        "}\n"));
}
