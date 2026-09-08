#include "TestHelpers.h"
#include "dragon/OwnershipCheck.h"
#include <gtest/gtest.h>
#include "CodeBlock.h"

using namespace dragon;
using namespace dragon::test;

static std::string code(const std::string& block) {
    return extractCode("OwnershipCheckTest.md", block);
}

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

std::string ownErrorWithModule(const std::string& moduleName,
                               const std::string& moduleSrc,
                               const std::string& src,
                               bool ownershipSeesModule = true) {
    auto dep = parse(moduleSrc, true);
    dep->moduleName = moduleName;
    Sema depSema;
    depSema.analyze(*dep);
    TypeChecker depTc;
    depTc.check(*dep);
    auto mod = parse(src, true);
    Sema sema;
    sema.analyze(*mod);
    TypeChecker tc;
    tc.registerExternalModule(moduleName, depTc.getExports(), "",
                              depTc.getTypeExports());
    tc.check(*mod);
    OwnershipCheck oc;
    if (ownershipSeesModule) oc.registerExternalModule(*dep);
    if (oc.analyze(*mod)) return "";
    return oc.diagnostics().empty() ? "<no message>"
                                    : oc.diagnostics()[0].message;
}

}

TEST(OwnershipCheckTest, DelAfterContainerStoreErrors) {
    std::string e = ownError(code("del_after_container_store_errors"));
    EXPECT_NE(e.find("escaped into"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfCapturedLocalErrors) {
    std::string e = ownError(code("del_of_captured_local_errors"));
    EXPECT_NE(e.find("captured"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfAliasedOwnerErrors) {
    std::string e = ownError(code("del_of_aliased_owner_errors"));
    EXPECT_NE(e.find("alias"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DoubleDelErrors) {
    std::string e = ownError(code("double_del_errors"));
    EXPECT_NE(e.find("already deleted"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfBorrowedElementErrors) {
    std::string e = ownError(code("del_of_borrowed_element_errors"));
    EXPECT_NE(e.find("not the owner"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfFieldErrors) {
    std::string e = ownError(code("del_of_field_errors"));
    EXPECT_NE(e.find("field"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ConditionalDelThenUseErrors) {
    std::string e = ownError(code("conditional_del_then_use_errors"));
    EXPECT_NE(e.find("every path"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfOuterBindingInLoopErrors) {
    std::string e = ownError(code("del_of_outer_binding_in_loop_errors"));
    EXPECT_NE(e.find("iteration"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfModuleGlobalErrors) {
    std::string e = ownError("g: str = \"a\" + \"b\"\ndel g\n");
    EXPECT_NE(e.find("module global"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, UseAfterDelErrors) {
    std::string e = ownError(code("use_after_del_errors"));
    EXPECT_NE(e.find("was deleted"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfPlainParamErrors) {
    std::string e = ownError(code("del_of_plain_param_errors"));
    EXPECT_NE(e.find("not the owner"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfWithSubjectErrors) {
    std::string e = ownError(code("del_of_with_subject_errors"));
    EXPECT_NE(e.find("not the owner"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, OwnFieldBorrowStoreErrors) {
    std::string e = ownError(code("own_field_borrow_store_errors"));
    EXPECT_NE(e.find("sole ownership"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, OwnParamPlainStoreIntoOwnFieldCompiles) {
    EXPECT_TRUE(ownAccepts(code("own_param_plain_store_into_own_field_compiles")));
}

TEST(OwnershipCheckTest, FreshOwnedLocalPlainStoreIntoOwnFieldCompiles) {
    EXPECT_TRUE(ownAccepts(code("fresh_owned_local_plain_store_into_own_field_compiles")));
}

// The implicit move CONSUMES the name: a later use is use-after-move, exactly
// as with an explicit `own`.
TEST(OwnershipCheckTest, PlainStoreIntoOwnFieldThenUseIsUseAfterMove) {
    std::string e = ownError(code("plain_store_into_own_field_then_use_is_use_after_move"));
    EXPECT_NE(e.find("was moved into"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, PlainStoreOfEscapedOwnerIntoOwnFieldErrors) {
    std::string e = ownError(code("plain_store_of_escaped_owner_into_own_field_errors"));
    EXPECT_NE(e.find("sole ownership"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, OwnScalarFieldErrors) {
    std::string e = ownError(code("own_scalar_field_errors"));
    EXPECT_NE(e.find("scalar"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, OwnLocalErrors) {
    std::string e = ownError(code("own_local_errors"));
    EXPECT_NE(e.find("class FIELD"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, PlainLockFieldErrors) {
    std::string e = ownError(code("plain_lock_field_errors"));
    EXPECT_NE(e.find("must be declared own"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, UndeclaredLockFieldStoreErrors) {
    std::string e = ownError(code("undeclared_lock_field_store_errors"));
    EXPECT_NE(e.find("must be declared own"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ContainerOfLockErrors) {
    std::string e = ownError("locks: list[Lock] = []\n");
    EXPECT_NE(e.find("cannot hold raw Lock"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ConditionalDelNoUseErrorsAtJoin) {
    std::string e = ownError(code("conditional_del_no_use_errors_at_join"));
    EXPECT_NE(e.find("every path"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, UseAfterMoveErrors) {
    std::string e = ownError(code("use_after_move_errors"));
    EXPECT_NE(e.find("was moved into"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ConditionalMoveNoUseErrorsAtJoin) {
    std::string e = ownError(code("conditional_move_no_use_errors_at_join"));
    EXPECT_NE(e.find("every path"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, MoveToAliasDestinationErrors) {
    std::string e = ownError(code("move_to_alias_destination_errors"));
    EXPECT_NE(e.find("consuming destination"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DoubleMoveErrors) {
    std::string e = ownError(code("double_move_errors"));
    EXPECT_NE(e.find("already moved"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, MoveOfEscapedOwnerErrors) {
    std::string e = ownError(code("move_of_escaped_owner_errors"));
    EXPECT_NE(e.find("escaped into"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, MutationDuringIterationErrors) {
    std::string e = ownError(code("mutation_during_iteration_errors"));
    EXPECT_NE(e.find("observe its own mutations"), std::string::npos) << e;
    EXPECT_NE(e.find("dub names"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, SubscriptStoreDuringIterationErrors) {
    std::string e = ownError(code("subscript_store_during_iteration_errors"));
    EXPECT_NE(e.find("observe its own mutations"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, MutatingADifferentContainerCompiles) {
    EXPECT_TRUE(ownAccepts(code("mutating_a_different_container_compiles")));
}

TEST(OwnershipCheckTest, DubImmutableIterableErrors) {
    std::string e = ownError(code("dub_immutable_iterable_errors"));
    EXPECT_NE(e.find("immutable"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, TouchWhileLentErrors) {
    std::string e = ownError(code("touch_while_lent_errors"));
    EXPECT_NE(e.find("lent to"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DiscardedHandleBorrowErrors) {
    std::string e = ownError(code("discarded_handle_borrow_errors"));
    EXPECT_NE(e.find("crosses a thread boundary"), std::string::npos) << e;
    EXPECT_NE(e.find("discarded"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, StillLentAtScopeEndErrors) {
    std::string e = ownError(code("still_lent_at_scope_end_errors"));
    EXPECT_NE(e.find("still lent"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, TaskRebindWhileLentErrors) {
    std::string e = ownError(code("task_rebind_while_lent_errors"));
    EXPECT_NE(e.find("rebound while"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, LendThenAwaitThenUseCompiles) {
    EXPECT_TRUE(ownAccepts(code("lend_then_await_then_use_compiles")));
}

TEST(OwnershipCheckTest, FireReadOnlyListFanOutAccepted) {
    EXPECT_TRUE(ownAccepts(code("fire_read_only_list_fan_out_accepted")));
}

TEST(OwnershipCheckTest, FireReadOnlyDiscardedHandleAccepted) {
    EXPECT_TRUE(ownAccepts(code("fire_read_only_discarded_handle_accepted")));
}

TEST(OwnershipCheckTest, FireMutatingWorkerInLoopRejected) {
    std::string e = ownError(code("fire_mutating_worker_in_loop_rejected"));
    EXPECT_FALSE(e.empty()) << "mutating worker fan-out must be refused";
}

TEST(OwnershipCheckTest, FireBorrowingWriterRejectedNamesTheWrite) {
    std::string e = ownError(code("fire_borrowing_writer_rejected"));
    EXPECT_NE(e.find("crosses a thread boundary"), std::string::npos) << e;
    EXPECT_NE(e.find("'mutate' writes 's' ('s.append()' at line 2)"),
              std::string::npos) << e;
    EXPECT_NE(e.find("copy it (dub shared)"), std::string::npos) << e;
    EXPECT_NE(e.find("declare 's' own in 'mutate' and move it (own shared)"),
              std::string::npos) << e;
    EXPECT_EQ(e.find("move it (own shared), copy it"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, FireKeywordArgumentReadOnlyAccepted) {
    EXPECT_TRUE(ownAccepts(code("fire_keyword_argument_read_only_accepted")));
}

TEST(OwnershipCheckTest, FireNonDubableWriterOmitsDubHint) {
    std::string e = ownError(code("fire_non_dubable_writer_rejected"));
    EXPECT_NE(e.find("'fill' writes 'b'"), std::string::npos) << e;
    EXPECT_EQ(e.find("(dub b)"), std::string::npos) << e;
    EXPECT_NE(e.find("dub does not apply"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, FireImportedReadOnlyAccepted) {
    const std::string readers = code("fire_readers_module");
    EXPECT_EQ(ownErrorWithModule("readers", readers,
                                 code("fire_imported_read_only_accepted")), "");
    EXPECT_EQ(ownErrorWithModule("readers", readers,
                                 code("fire_imported_aliased_read_only_accepted")), "");
    EXPECT_EQ(ownErrorWithModule("readers", readers,
                                 code("fire_imported_qualified_read_only_accepted")), "");
}

TEST(OwnershipCheckTest, FireImportedWriterRejectedNamesTheWrite) {
    std::string e = ownErrorWithModule("readers", code("fire_readers_module"),
                                       code("fire_imported_writer_rejected"));
    EXPECT_NE(e.find("crosses a thread boundary"), std::string::npos) << e;
    EXPECT_NE(e.find("'writer' writes 'd' (a subscript store on 'd' at line 3)"),
              std::string::npos) << e;
    EXPECT_NE(e.find("copy it (dub doc)"), std::string::npos) << e;
    EXPECT_NE(e.find("declare 'd' own in 'writer' and move it (own doc)"),
              std::string::npos) << e;
}

TEST(OwnershipCheckTest, FireImportedOwnTakerSuggestsMove) {
    std::string e = ownErrorWithModule("readers", code("fire_readers_module"),
                                       code("fire_imported_own_taker_rejected"));
    EXPECT_NE(e.find("move it (own doc)"), std::string::npos) << e;
    EXPECT_EQ(e.find("declare"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, FireUnregisteredImportStaysRefused) {
    std::string e = ownErrorWithModule("readers", code("fire_readers_module"),
                                       code("fire_imported_read_only_accepted"),
                                       false);
    EXPECT_NE(e.find("crosses a thread boundary"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, FireImportedLockedTypeAccepted) {
    EXPECT_EQ(ownErrorWithModule("guarded", code("fire_locked_module"),
                                 code("fire_imported_locked_type_accepted")), "");
}

TEST(OwnershipCheckTest, FireImportedLockedTypeUnseenStaysRefused) {
    std::string e = ownErrorWithModule("guarded", code("fire_locked_module"),
                                       code("fire_imported_locked_type_accepted"),
                                       false);
    EXPECT_NE(e.find("crosses a thread boundary"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, FireShadowedLockedNameRefused) {
    std::string e = ownErrorWithModule("guarded", code("fire_locked_module"),
                                       code("fire_shadowed_locked_name_refused"));
    EXPECT_NE(e.find("crosses a thread boundary"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelAfterReadOnlyShareRejected) {
    std::string e = ownError(code("del_after_read_only_share_rejected"));
    EXPECT_FALSE(e.empty()) << "del of a value shared into a thread must refuse";
}

// `defer f(own x)` moves x at the STATEMENT (a later use is the use-after-move E-class); a pending
// defer PINS every referenced binding, so a later own move or del of it must refuse at compile time.

TEST(OwnershipCheckTest, UseAfterDeferOwnMoveErrors) {
    std::string e = ownError(code("use_after_defer_own_move_errors"));
    EXPECT_NE(e.find("was moved into"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DoubleMoveAfterDeferOwnErrors) {
    std::string e = ownError(code("double_move_after_defer_own_errors"));
    EXPECT_FALSE(e.empty()) << "second move of a defer-moved binding must refuse";
}

TEST(OwnershipCheckTest, OwnMoveOfDeferPinnedArgErrors) {
    std::string e = ownError(code("own_move_of_defer_pinned_arg_errors"));
    EXPECT_NE(e.find("pending defer"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, OwnMoveOfDeferredReceiverErrors) {
    std::string e = ownError(code("own_move_of_deferred_receiver_errors"));
    EXPECT_NE(e.find("pending defer"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfDeferPinnedBindingErrors) {
    std::string e = ownError(code("del_of_defer_pinned_binding_errors"));
    EXPECT_NE(e.find("pending defer"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, PinExpiresWithDeferScope) {
    EXPECT_TRUE(ownAccepts(code("pin_expires_with_defer_scope")));
}

TEST(OwnershipCheckTest, DeferBorrowThenContinuedUseAccepted) {
    EXPECT_TRUE(ownAccepts(code("defer_borrow_then_continued_use_accepted")));
}

TEST(OwnershipCheckTest, DeferOwnWithNoLaterUseAccepted) {
    EXPECT_TRUE(ownAccepts(code("defer_own_with_no_later_use_accepted")));
}

TEST(OwnershipCheckTest, DeferDubLeavesSourceLive) {
    EXPECT_TRUE(ownAccepts(code("defer_dub_leaves_source_live")));
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
    std::string e = ownError(withPair(code("own_ctor_move_then_use_errors")));
    EXPECT_NE(e.find("was moved into"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, OwnCtorDoubleMoveErrors) {
    std::string e = ownError(withPair(code("own_ctor_double_move_errors")));
    EXPECT_NE(e.find("already moved"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, OwnCtorFreshTemporaryAccepted) {
    EXPECT_TRUE(ownAccepts(withPair(code("own_ctor_fresh_temporary_accepted"))));
}

TEST(OwnershipCheckTest, OwnCtorMoveAccepted) {
    EXPECT_TRUE(ownAccepts(withPair(code("own_ctor_move_accepted"))));
}

TEST(OwnershipCheckTest, DelOfLentTaskErrorsAtDelSite) {
    std::string e = ownError(code("del_of_lent_task_errors_at_del_site"));
    EXPECT_NE(e.find("cannot del task 't'"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DelOfUnlentTaskAccepted) {
    EXPECT_TRUE(ownAccepts(code("del_of_unlent_task_accepted")));
}

TEST(OwnershipCheckTest, DoubleAwaitRejected) {
    std::string e = ownError(code("double_await_rejected"));
    EXPECT_NE(e.find("moves out exactly once"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, AwaitThenJoinRejected) {
    std::string e = ownError(code("await_then_join_rejected"));
    EXPECT_NE(e.find("moves out exactly once"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, IsAliveAfterAwaitRejected) {
    std::string e = ownError(code("is_alive_after_await_rejected"));
    EXPECT_NE(e.find("moves out exactly once"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, AwaitInLoopOfOuterTaskRejected) {
    std::string e = ownError(code("await_in_loop_of_outer_task_rejected"));
    EXPECT_NE(e.find("awaited on iteration 1"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ConditionalAwaitThenSecondAwaitRejected) {
    std::string e = ownError(code("conditional_await_then_second_await_rejected"));
    EXPECT_NE(e.find("moves out exactly once"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ConditionalAwaitAloneAccepted) {
    EXPECT_TRUE(ownAccepts(code("conditional_await_alone_accepted")));
}

TEST(OwnershipCheckTest, PollThenAwaitAccepted) {
    EXPECT_TRUE(ownAccepts(code("poll_then_await_accepted")));
}

TEST(OwnershipCheckTest, RebindThenAwaitAccepted) {
    EXPECT_TRUE(ownAccepts(code("rebind_then_await_accepted")));
}

TEST(OwnershipCheckTest, KeywordMoveMarksBindingMoved) {
    std::string e = ownError(code("keyword_move_marks_binding_moved"));
    EXPECT_NE(e.find("was moved into"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, KeywordMoveWithoutReuseAccepted) {
    EXPECT_TRUE(ownAccepts(code("keyword_move_without_reuse_accepted")));
}

TEST(OwnershipCheckTest, KeywordDubKeepsBindingUsable) {
    EXPECT_TRUE(ownAccepts(code("keyword_dub_keeps_binding_usable")));
}

TEST(OwnershipCheckTest, DubIntoOwnParamKeepsBindingUsable) {
    EXPECT_TRUE(ownAccepts(code("dub_into_own_param_keeps_binding_usable")));
}

TEST(OwnershipCheckTest, ReturningAnOwnCollectionFieldErrors) {
    std::string e = ownError(code("returning_an_own_collection_field_errors"));
    EXPECT_NE(e.find("is an own field"), std::string::npos) << e;
    EXPECT_NE(e.find("dub self.headers"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ReturningAnOwnFieldViaALocalStillErrors) {
    std::string e = ownError(code("returning_an_own_field_via_a_local_still_errors"));
    EXPECT_NE(e.find("is an own field"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, ReturningAnOwnInstanceFieldAdvisesAMethod) {
    std::string e = ownError(code("returning_an_own_instance_field_advises_a_method"));
    EXPECT_NE(e.find("is an own field"), std::string::npos) << e;
    EXPECT_EQ(e.find("dub self.inst"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, DubbingAnOwnFieldOutIsTheSanctionedExit) {
    EXPECT_TRUE(ownAccepts(code("dubbing_an_own_field_out_is_the_sanctioned_exit")));
}

TEST(OwnershipCheckTest, ImmutableOwnFieldsAreNotSealed) {
    EXPECT_TRUE(ownAccepts(code("immutable_own_fields_are_not_sealed")));
}

TEST(OwnershipCheckTest, PlainCollectionFieldStaysUnsealed) {
    EXPECT_TRUE(ownAccepts(code("plain_collection_field_stays_unsealed")));
}

TEST(OwnershipCheckTest, TaskJoinThroughModuleGlobalRejected) {
    std::string e = ownError(code("task_join_through_module_global_rejected"));
    EXPECT_NE(e.find("moves out exactly once"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, TaskJoinThroughSubscriptRejected) {
    std::string e = ownError(code("task_join_through_subscript_rejected"));
    EXPECT_NE(e.find("not a binding"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, TaskJoinThroughFieldRejected) {
    std::string e = ownError(code("task_join_through_field_rejected"));
    EXPECT_NE(e.find("not a binding"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, TaskReboundToSecondHandleRejected) {
    std::string e = ownError(code("task_rebound_to_second_handle_rejected"));
    EXPECT_NE(e.find("single-owner"), std::string::npos) << e;
}

TEST(OwnershipCheckTest, TaskIsAliveThroughAnyReceiverOk) {
    EXPECT_TRUE(ownAccepts(code("task_is_alive_through_any_receiver_ok")));
}
