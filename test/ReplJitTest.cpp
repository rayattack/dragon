#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "dragon.h"
#include "../src/repl/ReplJit.h"

using namespace llvm;

using dragon::ReplJit;
using dragon::ReplJitOptions;

extern "C" const char* dragon_repr_int(int64_t x);
extern "C" void dragon_decref_str(const char* s);
extern "C" uint64_t __dragon_hash_k0;
extern "C" uint64_t __dragon_hash_k1;

namespace {

std::string takeReprString(const char* owned) {
    if (!owned) return {};
    std::string copy(owned);
    dragon_decref_str(owned);
    return copy;
}

class ReplJitTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { dragon::initialize(); }

    std::unique_ptr<ReplJit> makeJit() {
        ReplJitOptions options;
        auto jit = ReplJit::create(options);
        if (!jit) {
            ADD_FAILURE() << "ReplJit::create failed: " << toString(jit.takeError());
            return nullptr;
        }
        return std::move(*jit);
    }

    orc::ThreadSafeModule buildCallReprInt(const std::string& moduleName,
                                           const std::string& entryName,
                                           int64_t value,
                                           const DataLayout& dl) {
        auto context = std::make_unique<LLVMContext>();
        auto module = std::make_unique<Module>(moduleName, *context);
        module->setDataLayout(dl);

        auto* i64 = Type::getInt64Ty(*context);
        auto* ptr = PointerType::getUnqual(*context);

        auto* reprTy = FunctionType::get(ptr, {i64}, false);
        auto* repr = Function::Create(reprTy, Function::ExternalLinkage,
                                      "dragon_repr_int", module.get());

        auto* entryTy = FunctionType::get(ptr, {}, false);
        auto* entry = Function::Create(entryTy, Function::ExternalLinkage,
                                       entryName, module.get());

        IRBuilder<> builder(BasicBlock::Create(*context, "entry", entry));
        auto* call = builder.CreateCall(repr, {ConstantInt::get(i64, value)});
        builder.CreateRet(call);

        return orc::ThreadSafeModule(std::move(module), std::move(context));
    }
};

TEST_F(ReplJitTest, RuntimeIsReachableFromTheProcessImage) {
    EXPECT_EQ("42", takeReprString(dragon_repr_int(42)))
        << "the runtime must be linked into the test binary itself";
}

TEST_F(ReplJitTest, HashSecretConstructorRan) {
    EXPECT_NE(__dragon_hash_k0, 0u)
        << "runtime_platform.cpp's __attribute__((constructor)) did not run; "
           "dict and set hashing would be keyed on zero";
    EXPECT_NE(__dragon_hash_k1, 0u);
}

TEST_F(ReplJitTest, JitsAModuleThatCallsTheRuntime) {
    auto jit = makeJit();
    ASSERT_NE(jit, nullptr);

    auto module = buildCallReprInt("smoke", "repl.smoke", 42, jit->dataLayout());
    auto tracker = jit->newTurnTracker();
    auto err = jit->addTurnModule(tracker, std::move(module));
    ASSERT_FALSE(static_cast<bool>(err)) << toString(std::move(err));

    auto addr = jit->lookupInSession("repl.smoke");
    ASSERT_TRUE(static_cast<bool>(addr)) << toString(addr.takeError());

    auto fn = addr->toPtr<const char* (*)()>();
    EXPECT_EQ("42", takeReprString(fn()));
}

TEST_F(ReplJitTest, ResourceTrackerRemovalUnloadsTheTurn) {
    auto jit = makeJit();
    ASSERT_NE(jit, nullptr);

    auto tracker = jit->newTurnTracker();
    auto err = jit->addTurnModule(
        tracker, buildCallReprInt("t1", "repl.turn.1", 7, jit->dataLayout()));
    ASSERT_FALSE(static_cast<bool>(err)) << toString(std::move(err));

    auto addr = jit->lookupInSession("repl.turn.1");
    ASSERT_TRUE(static_cast<bool>(addr)) << toString(addr.takeError());
    EXPECT_EQ("7", takeReprString(addr->toPtr<const char* (*)()>()()));

    auto removed = tracker->remove();
    ASSERT_FALSE(static_cast<bool>(removed)) << toString(std::move(removed));

    auto gone = jit->lookupInSession("repl.turn.1");
    EXPECT_FALSE(static_cast<bool>(gone))
        << "a removed tracker must take its symbols with it";
    if (!gone) consumeError(gone.takeError());
}

TEST_F(ReplJitTest, TurnsAreIndependentAndNamesDoNotCollide) {
    auto jit = makeJit();
    ASSERT_NE(jit, nullptr);

    auto t1 = jit->newTurnTracker();
    auto e1 = jit->addTurnModule(
        t1, buildCallReprInt("m1", "repl.turn.1", 1, jit->dataLayout()));
    ASSERT_FALSE(static_cast<bool>(e1)) << toString(std::move(e1));

    auto t2 = jit->newTurnTracker();
    auto e2 = jit->addTurnModule(
        t2, buildCallReprInt("m2", "repl.turn.2", 2, jit->dataLayout()));
    ASSERT_FALSE(static_cast<bool>(e2)) << toString(std::move(e2));

    auto a1 = jit->lookupInSession("repl.turn.1");
    auto a2 = jit->lookupInSession("repl.turn.2");
    ASSERT_TRUE(static_cast<bool>(a1)) << toString(a1.takeError());
    ASSERT_TRUE(static_cast<bool>(a2)) << toString(a2.takeError());

    EXPECT_EQ("1", takeReprString(a1->toPtr<const char* (*)()>()()));
    EXPECT_EQ("2", takeReprString(a2->toPtr<const char* (*)()>()()));

    auto removed = t1->remove();
    ASSERT_FALSE(static_cast<bool>(removed)) << toString(std::move(removed));

    auto still = jit->lookupInSession("repl.turn.2");
    ASSERT_TRUE(static_cast<bool>(still)) << toString(still.takeError());
    EXPECT_EQ("2", takeReprString(still->toPtr<const char* (*)()>()()));
}

TEST_F(ReplJitTest, EditorDylibSurvivesASessionClear) {
    auto jit = makeJit();
    ASSERT_NE(jit, nullptr);

    auto err = jit->addEditorModule(
        buildCallReprInt("editor", "repl.editor.main", 99, jit->dataLayout()));
    ASSERT_FALSE(static_cast<bool>(err)) << toString(std::move(err));

    auto tracker = jit->newTurnTracker();
    auto turnErr = jit->addTurnModule(
        tracker, buildCallReprInt("t1", "repl.turn.1", 5, jit->dataLayout()));
    ASSERT_FALSE(static_cast<bool>(turnErr)) << toString(std::move(turnErr));

    auto editorAddr = jit->lookupInEditor("repl.editor.main");
    ASSERT_TRUE(static_cast<bool>(editorAddr)) << toString(editorAddr.takeError());
    EXPECT_EQ("99", takeReprString(editorAddr->toPtr<const char* (*)()>()()));

    auto cleared = jit->clearSession();
    ASSERT_FALSE(static_cast<bool>(cleared)) << toString(std::move(cleared));

    auto afterReset = jit->lookupInEditor("repl.editor.main");
    ASSERT_TRUE(static_cast<bool>(afterReset)) << toString(afterReset.takeError());
    EXPECT_EQ("99", takeReprString(afterReset->toPtr<const char* (*)()>()()))
        << ":reset must not unmap the editor's code";
}

}
