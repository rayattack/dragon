#include "../CodeGenImpl.h"

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/CFG.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

namespace dragon {

namespace {

const llvm::StringSet<>& runtimeNoRaiseSymbols() {
    static const llvm::StringSet<> symbols = {
        "dragon_exc_push_frame",
        "dragon_exc_pop_frame",
        "dragon_exc_active_frames",
        "setjmp",
        "_setjmp",
        "dragon_incref",
        "dragon_incref_str",
        "dragon_incref_atomic",
        "dragon_mark_shared_str",
        "dragon_cleanup_push",
        "dragon_cleanup_push_if_live",
        "dragon_cleanup_update",
        "dragon_cleanup_depth",
        "dragon_cleanup_reset",
        "dragon_list_len",
        "dragon_dict_len",
        "dragon_set_len",
        "dragon_tuple_len",
        "dragon_bytes_len",
        "dragon_str_len",
    };
    return symbols;
}

using RaiseSet = llvm::DenseSet<const llvm::Function*>;

bool calleeCannotRaise(const llvm::Function* callee, const RaiseSet& mayRaise) {
    if (!callee) return false;
    if (callee->isIntrinsic())
        return callee->getIntrinsicID() != llvm::Intrinsic::eh_sjlj_longjmp;
    if (!callee->isDeclaration()) return mayRaise.count(callee) == 0;
    return runtimeNoRaiseSymbols().contains(callee->getName());
}

bool instructionCanRaise(const llvm::Instruction& inst, const RaiseSet& mayRaise) {
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (!call) return false;
    if (call->isInlineAsm()) return false;
    return !calleeCannotRaise(call->getCalledFunction(), mayRaise);
}

bool bodyCanRaise(const llvm::Function& fn, const RaiseSet& mayRaise) {
    for (const llvm::BasicBlock& bb : fn)
        for (const llvm::Instruction& inst : bb)
            if (instructionCanRaise(inst, mayRaise)) return true;
    return false;
}

RaiseSet computeMayRaise(llvm::Module& module) {
    RaiseSet mayRaise;
    bool changed = true;
    while (changed) {
        changed = false;
        for (llvm::Function& fn : module) {
            if (fn.isDeclaration() || mayRaise.count(&fn)) continue;
            if (!bodyCanRaise(fn, mayRaise)) continue;
            mayRaise.insert(&fn);
            changed = true;
        }
    }
    return mayRaise;
}

bool protectedRegionCanRaise(const ExcFrameSite& site,
                             const RaiseSet& mayRaise) {
    llvm::SmallPtrSet<const llvm::Instruction*, 8> boundary(site.pops.begin(),
                                                           site.pops.end());
    llvm::SmallPtrSet<const llvm::BasicBlock*, 32> seen;
    llvm::SmallVector<const llvm::BasicBlock*, 32> work;
    work.push_back(site.bodyBB);
    seen.insert(site.bodyBB);
    while (!work.empty()) {
        const llvm::BasicBlock* bb = work.pop_back_val();
        bool leftRegion = false;
        for (const llvm::Instruction& inst : *bb) {
            if (boundary.contains(&inst)) { leftRegion = true; break; }
            if (instructionCanRaise(inst, mayRaise)) return true;
        }
        if (leftRegion) continue;
        for (const llvm::BasicBlock* succ : llvm::successors(bb))
            if (seen.insert(succ).second) work.push_back(succ);
    }
    return false;
}

bool frameIsSelfContained(const ExcFrameSite& site) {
    if (!site.push || !site.setjmp || !site.normalCond || !site.armBranch ||
        !site.bodyBB || !site.func)
        return false;
    if (!site.push->hasOneUse() || site.push->user_back() != site.setjmp)
        return false;
    if (!site.setjmp->hasOneUse() || site.setjmp->user_back() != site.normalCond)
        return false;
    if (!site.normalCond->hasOneUse() || site.normalCond->user_back() != site.armBranch)
        return false;
    for (const llvm::CallInst* pop : site.pops)
        if (!pop->use_empty()) return false;
    return true;
}

void dropFrame(ExcFrameSite& site) {
    llvm::BranchInst::Create(site.bodyBB, site.armBranch->getIterator());
    site.armBranch->eraseFromParent();
    site.normalCond->eraseFromParent();
    site.setjmp->eraseFromParent();
    site.push->eraseFromParent();
    for (llvm::CallInst* pop : site.pops) pop->eraseFromParent();
}

}

size_t CodeGen::Impl::armExcFrame(llvm::BasicBlock* bodyBB,
                                  llvm::BasicBlock* unwindBB) {
    auto* jmpbufPtr =
        builder->CreateCall(runtimeFuncs["dragon_exc_push_frame"], {}, "jmpbuf");
    auto* setjmpResult =
        builder->CreateCall(runtimeFuncs["setjmp"], {jmpbufPtr}, "setjmp.result");
    auto* isNormal = builder->CreateICmpEQ(
        setjmpResult,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0),
        "is.normal");
    auto* armBranch = builder->CreateCondBr(isNormal, bodyBB, unwindBB);

    ExcFrameSite site;
    site.func = currentFunction;
    site.push = jmpbufPtr;
    site.setjmp = setjmpResult;
    site.normalCond = llvm::dyn_cast<llvm::Instruction>(isNormal);
    site.armBranch = armBranch;
    site.bodyBB = bodyBB;
    excFrameSites.push_back(site);
    return excFrameSites.size() - 1;
}

void CodeGen::Impl::emitExcFramePop(size_t site) {
    auto* pop = builder->CreateCall(runtimeFuncs["dragon_exc_pop_frame"], {});
    if (site < excFrameSites.size()) excFrameSites[site].pops.push_back(pop);
}

void CodeGen::Impl::elideUnreachableExcFrames() {
    auto sites = std::move(excFrameSites);
    excFrameSites.clear();
    if (sites.empty()) return;

    const RaiseSet mayRaise = computeMayRaise(*module);

    llvm::SmallVector<size_t, 16> elidable;
    for (size_t i = 0; i < sites.size(); ++i) {
        if (!frameIsSelfContained(sites[i])) continue;
        if (protectedRegionCanRaise(sites[i], mayRaise)) continue;
        elidable.push_back(i);
    }

    llvm::SmallPtrSet<llvm::Function*, 16> touched;
    for (size_t i : elidable) {
        dropFrame(sites[i]);
        touched.insert(sites[i].func);
    }
    for (llvm::Function* fn : touched) llvm::EliminateUnreachableBlocks(*fn);
}

}
