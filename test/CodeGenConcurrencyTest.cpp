#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenConcurrencyTest.md", block);
}

TEST(CodeGenTest, FireBasicIR) {
    auto ir = generateIR(code("fire_basic_ir"));
    EXPECT_NE(ir.find("dragon_thread_fire"), std::string::npos);
}

TEST(CodeGenTest, FireJoinIR) {
    auto ir = generateIR(code("fire_join_ir"));
    EXPECT_NE(ir.find("dragon_thread_join"), std::string::npos);
}

TEST(CodeGenTest, FireIsAliveIR) {
    auto ir = generateIR(code("fire_is_alive_ir"));
    EXPECT_NE(ir.find("dragon_vthread_is_alive"), std::string::npos);
}

TEST(CodeGenTest, LockNewIR) {
    auto ir = generateIR(code("lock_new_ir"));
    EXPECT_NE(ir.find("dragon_lock_new"), std::string::npos);
}

TEST(CodeGenTest, LockAcquireReleaseIR) {
    auto ir = generateIR(code("lock_acquire_release_ir"));
    EXPECT_NE(ir.find("dragon_lock_acquire"), std::string::npos);
    EXPECT_NE(ir.find("dragon_lock_release"), std::string::npos);
}

TEST(CodeGenTest, LockAcquireNonblockingBoolIR) {
    auto ir = generateIR(code("lock_acquire_nonblocking_bool_ir"));
    EXPECT_NE(ir.find("dragon_lock_acquire_ex"), std::string::npos);
    EXPECT_NE(ir.find("icmp ne i64"), std::string::npos);
}

TEST(CodeGenTest, LockAcquireTimeoutIR) {
    auto ir = generateIR(code("lock_acquire_timeout_ir"));
    EXPECT_NE(ir.find("dragon_lock_acquire_ex"), std::string::npos);
    EXPECT_NE(ir.find("double"), std::string::npos);
}

TEST(CodeGenTest, LockFastPathNoOverheadIR) {
    auto ir = generateIR(code("lock_fast_path_no_overhead_ir"));
    EXPECT_NE(ir.find("dragon_lock_new"), std::string::npos);
    EXPECT_EQ(ir.find("threading__Lock___init__"), std::string::npos);
    EXPECT_EQ(ir.find("_dragon_Lock"), std::string::npos);
}

TEST(CodeGenTest, LockWithStatementIR) {
    auto ir = generateIR(code("lock_with_statement_ir"));
    EXPECT_NE(ir.find("dragon_lock_acquire"), std::string::npos);
    EXPECT_NE(ir.find("dragon_lock_release"), std::string::npos);
}

TEST(CodeGenTest, ThreadBlockIR) {
    auto ir = generateIR(code("thread_block_ir"));
    EXPECT_NE(ir.find("__dragon_thread_"), std::string::npos);
    EXPECT_NE(ir.find("dragon_thread_fire"), std::string::npos);
    EXPECT_NE(ir.find("dragon_thread_join"), std::string::npos);
}

TEST(CodeGenTest, FireBlockIR) {
    auto ir = generateIR(code("fire_block_ir"));
    EXPECT_NE(ir.find("dragon_vthread_spawn"), std::string::npos);
    EXPECT_NE(ir.find("__dragon_fire_"), std::string::npos);
}

TEST(CodeGenTest, FireVthreadSpawnIR) {
    auto ir = generateIR(code("fire_vthread_spawn_ir"));
    EXPECT_NE(ir.find("dragon_vthread_spawn"), std::string::npos);
    EXPECT_NE(ir.find("call ptr @dragon_vthread_spawn"), std::string::npos);
}

TEST(CodeGenTest, FireVthreadJoinIR) {
    auto ir = generateIR(code("fire_vthread_join_ir"));
    EXPECT_NE(ir.find("dragon_vthread_join"), std::string::npos);
}

TEST(CodeGenTest, FireVthreadIsAliveIR) {
    auto ir = generateIR(code("fire_vthread_is_alive_ir"));
    EXPECT_NE(ir.find("dragon_vthread_is_alive"), std::string::npos);
}

TEST(CodeGenTest, AsyncDefIR) {
    auto ir = generateIR(code("async_def_ir"));
    EXPECT_NE(ir.find("fetch__async_body"), std::string::npos);
    EXPECT_NE(ir.find("dragon_vthread_spawn"), std::string::npos);
}

TEST(CodeGenTest, AwaitIR) {
    auto ir = generateIR(code("await_ir"));
    EXPECT_NE(ir.find("dragon_vthread_join"), std::string::npos);
}

TEST(CodeGenTest, AsyncDefReturnsPtrIR) {
    auto ir = generateIR(code("async_def_returns_ptr_ir"));
    EXPECT_NE(ir.find("compute__async_body"), std::string::npos);
    EXPECT_NE(ir.find("dragon_vthread_spawn"), std::string::npos);
}

TEST(CodeGenTest, VthreadSleepIR) {
    auto ir = generateIR(code("vthread_sleep_ir"));
    EXPECT_NE(ir.find("dragon_vthread_sleep"), std::string::npos);
}

TEST(CodeGenTest, SyncListNewIR) {
    auto ir = generateIR(code("sync_list_new_ir"));
    EXPECT_NE(ir.find("dragon_synclist_new"), std::string::npos);
}

TEST(CodeGenTest, SyncListAppendIR) {
    auto ir = generateIR(code("sync_list_append_ir"));
    EXPECT_NE(ir.find("dragon_synclist_append"), std::string::npos);
}

TEST(CodeGenTest, SyncDictNewIR) {
    auto ir = generateIR(code("sync_dict_new_ir"));
    EXPECT_NE(ir.find("dragon_syncdict_new"), std::string::npos);
}

TEST(CodeGenTest, FireBasicE2E) {
    auto out = compileAndRun(code("fire_basic_e2_e"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, FireReturnIntE2E) {
    auto out = compileAndRun(code("fire_return_int_e2_e"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, FireNoArgsE2E) {
    auto out = compileAndRun(code("fire_no_args_e2_e"));
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenTest, FireMultipleE2E) {
    auto out = compileAndRun(code("fire_multiple_e2_e"));
    EXPECT_EQ(out, "10\n20\n30\n");
}

TEST(CodeGenTest, FireSequentialE2E) {
    auto out = compileAndRun(code("fire_sequential_e2_e"));
    EXPECT_EQ(out, "9\n49\n");
}

TEST(CodeGenTest, FireVoidMethodE2E) {
    auto out = compileAndRun(code("fire_void_method_e2_e"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, FireVoidFunctionE2E) {
    auto out = compileAndRun(code("fire_void_function_e2_e"));
    EXPECT_EQ(out, "hello\n");
}

TEST(CodeGenTest, FireVoidNoReturnAnnotationE2E) {
    auto out = compileAndRun(code("fire_void_no_return_annotation_e2_e"));
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenTest, ThreadBlockBasicE2E) {
    auto out = compileAndRun(code("thread_block_basic_e2_e"));
    EXPECT_EQ(out, "from thread\nafter join\n");
}

TEST(CodeGenTest, ThreadBlockMultiStmtE2E) {
    auto out = compileAndRun(code("thread_block_multi_stmt_e2_e"));
    EXPECT_EQ(out, "a\nb\nc\ndone\n");
}

TEST(CodeGenTest, FireBlockBasicE2E) {
    auto out = compileAndRun(code("fire_block_basic_e2_e"));
    EXPECT_EQ(out, "from block\ndone\n");
}

TEST(CodeGenTest, FireBlockNoJoinE2E) {
    auto out = compileAndRun(code("fire_block_no_join_e2_e"));
    EXPECT_NE(out.find("bg"), std::string::npos);
    EXPECT_NE(out.find("main"), std::string::npos);
}

TEST(CodeGenTest, AsyncAwaitBasicE2E) {
    auto out = compileAndRun(code("async_await_basic_e2_e"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, AsyncAwaitWithArgsE2E) {
    auto out = compileAndRun(code("async_await_with_args_e2_e"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, AsyncAwaitMultipleArgsE2E) {
    auto out = compileAndRun(code("async_await_multiple_args_e2_e"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, AsyncAwaitInNormalDefE2E) {
    auto out = compileAndRun(code("async_await_in_normal_def_e2_e"));
    EXPECT_EQ(out, "100\n");
}

TEST(CodeGenTest, AsyncAwaitParallelE2E) {
    auto out = compileAndRun(code("async_await_parallel_e2_e"));
    EXPECT_EQ(out, "25\n");
}

TEST(CodeGenTest, AsyncAwaitChainE2E) {
    auto out = compileAndRun(code("async_await_chain_e2_e"));
    EXPECT_EQ(out, "30\n");
}

TEST(CodeGenTest, VthreadSleepE2E) {
    auto out = compileAndRun(code("vthread_sleep_e2_e"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, VthreadSleepConcurrentE2E) {
    auto out = compileAndRun(code("vthread_sleep_concurrent_e2_e"));
    EXPECT_EQ(out, "6\n");
}

TEST(CodeGenTest, SyncListBasicE2E) {
    auto out = compileAndRun(code("sync_list_basic_e2_e"));
    EXPECT_EQ(out, "3\n10\n20\n30\n");
}

TEST(CodeGenTest, SyncListPopSetE2E) {
    auto out = compileAndRun(code("sync_list_pop_set_e2_e"));
    EXPECT_EQ(out, "99\n3\n2\n");
}

TEST(CodeGenTest, SyncListSortReverseE2E) {
    auto out = compileAndRun(code("sync_list_sort_reverse_e2_e"));
    EXPECT_EQ(out, "1\n2\n3\n3\n");
}

TEST(CodeGenTest, SyncDictBasicE2E) {
    auto out = compileAndRun(code("sync_dict_basic_e2_e"));
    EXPECT_EQ(out, "10\n20\n2\n");
}

TEST(CodeGenTest, SyncDictGetDefaultE2E) {
    auto out = compileAndRun(code("sync_dict_get_default_e2_e"));
    EXPECT_EQ(out, "42\n-1\n1\n0\n");
}

TEST(CodeGenTest, SyncDictPopClearE2E) {
    auto out = compileAndRun(code("sync_dict_pop_clear_e2_e"));
    EXPECT_EQ(out, "1\n1\n0\n");
}

TEST(CodeGenTest, SyncListThreadedE2E) {
    auto out = compileAndRun(code("sync_list_threaded_e2_e"));
    EXPECT_EQ(out, "4\n");
}

// 10 fire workers x 100k allocations hit every GC race at once (parallel gc_track realloc, concurrent collects,
// decref vs refcount capture: each a double-free without gc_lock). Flat list[int] only: nested literals hit a pre-existing elem-incref bug, not these races.
TEST(CodeGenTest, GCThreadSafetyStressE2E) {
    auto out = compileAndRun(code("gc_thread_safety_stress_e2_e"));
    EXPECT_EQ(out, "1000000\n");
}

TEST(CodeGenE2E, VThreadDoneFlagSynchronizes) {
    auto out = compileAndRun(code("v_thread_done_flag_synchronizes"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, CycleCollectorWithStringFields) {
    auto out = compileAndRun(code("cycle_collector_with_string_fields"));
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, ThreadDoubleStartRejected) {
    auto out = compileAndRun(code("thread_double_start_rejected"));
    EXPECT_EQ(out, "0\n-1\n");
}

TEST(CodeGenIR, VthreadSleepInt64Param) {
    auto ir = generateIR(code("vthread_sleep_int64_param"));
    EXPECT_NE(ir.find("dragon_vthread_sleep(i64"), std::string::npos)
        << "Expected i64 ms parameter on dragon_vthread_sleep\nIR:\n" << ir;
}

TEST(CodeGenE2E, VthreadSleepShortStillWorks) {
    auto out = compileAndRun(code("vthread_sleep_short_still_works"));
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenE2E, VthreadSleepLargeValueDoesntTruncate) {
    auto out = compileAndRun(code("vthread_sleep_large_value_doesnt_truncate"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, NbRecvBadFdReturnsMinusOne) {
    auto out = compileAndRun(code("nb_recv_bad_fd_returns_minus_one"));
    EXPECT_EQ(out, "-1\n");
}

TEST(CodeGenE2E, NbSendBadFdReturnsMinusOne) {
    auto out = compileAndRun(code("nb_send_bad_fd_returns_minus_one"));
    EXPECT_EQ(out, "-1\n");
}

TEST(CodeGenE2E, NbAcceptBadFdReturnsMinusOne) {
    auto out = compileAndRun(code("nb_accept_bad_fd_returns_minus_one"));
    EXPECT_EQ(out, "-1\n");
}

TEST(CodeGenE2E, GCCycleCollectorMidConstructionTraverseNullCheck) {
    auto out = compileAndRun(code("gc_cycle_collector_mid_construction_traverse_null_check"));
    EXPECT_EQ(out, "48\n");
}

// Fired workers hammer incref/decref on the SAME heap strings: fire-site SHARED marking must
// dispatch atomic RC ops, else the torn refcount frees a string the shared list still points at.
TEST(CodeGenE2E, SharedRefcountAtomicDispatch_FireMultiWorker) {
    auto out = compileAndRun(code("shared_refcount_atomic_dispatch__fire_multi_worker"));
    EXPECT_EQ(out, "480000\n");
}

TEST(CodeGenE2E, FireVThreadUncaughtExceptionContained) {
    auto out = compileAndRun(code("fire_v_thread_uncaught_exception_contained"));
    EXPECT_EQ(out,
              "vthread terminated by uncaught ValueError: intentional\n"
              "20\n0\n40\nalive\n");
}

TEST(CodeGenE2E, TaskIntJoinRecoversNativeInt) {
    auto out = compileAndRun(code("task_int_join_recovers_native_int"));
    EXPECT_EQ(out, "21\n");
}

TEST(CodeGenE2E, TaskFloatJoinBitcastsNotConverts) {
    auto out = compileAndRun(code("task_float_join_bitcasts_not_converts"));
    EXPECT_EQ(out, "3.5\n");
}

TEST(CodeGenE2E, TaskStrJoinRecoversPointer) {
    auto out = compileAndRun(code("task_str_join_recovers_pointer"));
    EXPECT_EQ(out, "hello\n");
}

TEST(CodeGenE2E, AwaitAsyncDefRecoversNativeInt) {
    auto out = compileAndRun(code("await_async_def_recovers_native_int"));
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenE2E, BareTaskAnnotationRefinesAndJoins) {
    auto out = compileAndRun(code("bare_task_annotation_refines_and_joins"));
    EXPECT_EQ(out, "42\n");
}

static std::string irFunction(const std::string& ir, const std::string& name) {
    size_t pos = ir.find("\ndefine ");
    while (pos != std::string::npos) {
        size_t brace = ir.find('{', pos);
        if (brace == std::string::npos) return "";
        std::string header = ir.substr(pos, brace - pos);
        if (header.find("@" + name + "(") != std::string::npos) {
            size_t end = ir.find("\n}", brace);
            if (end == std::string::npos) return ir.substr(brace);
            return ir.substr(brace, end - brace);
        }
        pos = ir.find("\ndefine ", brace);
    }
    return "";
}

static void expectGuardedBody(const std::string& block,
                              const std::string& function) {
    std::string body = irFunction(generateIR(code(block)), function);
    ASSERT_FALSE(body.empty()) << block << ": no function @" << function;
    EXPECT_GE(countSubstring(body, "@dragon_lock_acquire"), 1u)
        << block << ": @" << function << " never acquires the lock";
    EXPECT_GE(countSubstring(body, "@dragon_lock_release"), 1u)
        << block << ": @" << function << " never releases the lock";
    EXPECT_EQ(countSubstring(body, "@dragon_lock_destroy"), 0u)
        << block << ": @" << function << " destroys a borrowed lock";
}

TEST(CodeGenTest, LockOwnFieldWithIR) {
    expectGuardedBody("lock_own_field_with_ir", "Counter_bump");
}

TEST(CodeGenTest, LockOtherObjectFieldWithIR) {
    expectGuardedBody("lock_other_object_field_with_ir", "bump_other");
}

TEST(CodeGenTest, LockNestedFieldWithIR) {
    expectGuardedBody("lock_nested_field_with_ir", "Outer_bump");
}

TEST(CodeGenTest, LockParameterWithIR) {
    expectGuardedBody("lock_parameter_with_ir", "bump_guarded");
}

TEST(CodeGenTest, LockLocalBoundFromFieldWithIR) {
    expectGuardedBody("lock_local_bound_from_field_with_ir", "Counter_bump");
}

TEST(CodeGenTest, LockFieldAcquireReleaseIR) {
    expectGuardedBody("lock_field_acquire_release_ir", "Counter_bump");
}

TEST(CodeGenTest, LockSharedAcrossFireIR) {
    expectGuardedBody("lock_shared_across_fire_ir", "Shared_add");
}

TEST(CodeGenTest, LockOwnedLocalStillDestroyedIR) {
    std::string body = irFunction(
        generateIR(code("lock_owned_local_still_destroyed_ir")), "guarded");
    ASSERT_FALSE(body.empty());
    EXPECT_GE(countSubstring(body, "@dragon_lock_acquire"), 1u);
    EXPECT_GE(countSubstring(body, "@dragon_lock_destroy"), 1u);
}

TEST(CodeGenE2E, LockSharedAcrossFireCountsBothWorkers) {
    auto out = compileAndRun(code("lock_shared_across_fire_ir"));
    EXPECT_EQ(out, "2\n");
}
