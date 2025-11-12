//
// This file is distributed under the Apache License v2.0. See LICENSE for
// details.
//

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include "omvll/ObfuscationConfig.hpp"
#include "omvll/PyConfig.hpp"
#include "omvll/log.hpp"
#include "omvll/passes/indirect-global-variable/IndirectGlobalVariable.hpp"
#include "omvll/passes/indirect-global-variable/IndirectGlobalVariableOpt.hpp"
#include "omvll/utils.hpp"

#include <unordered_map>
#include <vector>
#include <set>

using namespace llvm;
using namespace omvll;

namespace omvll {

PreservedAnalyses IndirectGlobalVariable::run(Module &M, ModuleAnalysisManager &MAM) {
  if (omvll::isModuleGloballyExcluded(&M)) {
    SINFO("Excluding module [{}]", M.getName());
    return PreservedAnalyses::all();
  }

  PyConfig &Config = PyConfig::instance();
  SINFO("[{}] Executing on module {}", name(), M.getName());

  std::unique_ptr<RandomNumberGenerator> RNG = M.createRNG(name());

  // Step 1: collect global variables referenced in functions
  std::unordered_map<Function*, std::set<GlobalVariable*>> FunctionGVs;
  std::vector<GlobalVariable*> GlobalVariables;
  std::unordered_map<GlobalVariable*, unsigned> GVIndex;
  std::vector<GlobalVariable*> GVPageTable;

  for (Function &F : M) {
    if (F.isIntrinsic() || F.isDeclaration())
      continue;

    for (BasicBlock &BB : F) {
      for (Instruction &Inst : BB) {
        if (Inst.isEHPad() || isa<CallInst>(Inst))
          continue;

        for (unsigned i = 0; i < Inst.getNumOperands(); ++i) {
          if (auto *GV = dyn_cast<GlobalVariable>(Inst.getOperand(i))) {
            if (GV->isThreadLocal() || GV->isDLLImportDependent())
              continue;
            if (GV->getMetadata("noobf"))
              continue;

            FunctionGVs[&F].insert(GV);

            if (GVIndex.count(GV) == 0) {
              GVIndex[GV] = GlobalVariables.size();
              GlobalVariables.push_back(GV);
            }
          }
        }
      }
    }
  }

  if (GlobalVariables.empty()) {
    SINFO("[{}] No globals to process in module {}", name(), M.getName());
    return PreservedAnalyses::all();
  }

  LLVMContext &Ctx = M.getContext();
  Type *Int8PtrTy = Type::getInt8PtrTy(Ctx);
  ArrayType *ArrayTy = ArrayType::get(Int8PtrTy, GlobalVariables.size());

  // Create page table
  GlobalVariable *PageTable = new GlobalVariable(
      M,
      ArrayTy,
      /*isConstant=*/false,
      GlobalValue::ExternalLinkage,
      /*Initializer=*/nullptr,
      M.getName() + "_IndirectGVs");
  PageTable->setAlignment(MaybeAlign(8));

  std::vector<Constant*> InitValues;
  for (GlobalVariable *GV : GlobalVariables)
    InitValues.push_back(ConstantExpr::getBitCast(GV, Int8PtrTy));
  PageTable->setInitializer(ConstantArray::get(ArrayTy, InitValues));
  GVPageTable.push_back(PageTable);

  // Step 2: probabilistic replacement using Python callback
  bool Changed = false;
  for (auto &Entry : FunctionGVs) {
    Function *F = Entry.first;
    const std::set<GlobalVariable*> &GVs = Entry.second;
    if (GVs.empty())
      continue;

    for (GlobalVariable *GV : GVs) {
      IndirectGlobalVariableOpt Opt = Config.getUserConfig()->indirectGlobalVariable(&M, GV);

      auto *P = std::get_if<IndirectGlobalVariableWithProbability>(&Opt);
      if (P == nullptr || P->Probability == 0)
        continue;

      auto ShouldProcess = [&]() { return (*RNG)() % 100U < P->Probability; };
      if (!ShouldProcess())
        continue;

      // Replace all uses in this function
      for (BasicBlock &BB : *F) {
        for (Instruction &Inst : BB) {
          for (unsigned i = 0; i < Inst.getNumOperands(); ++i) {
            if (Inst.getOperand(i) == GV) {
              Instruction *InsertPt = &Inst;
              PHINode *PHI = dyn_cast<PHINode>(&Inst);
              if (PHI)
                InsertPt = PHI->getIncomingBlock(i)->getTerminator();

              IRBuilder<> Builder(InsertPt);
              Value *Idx0 = ConstantInt::get(Type::getInt64Ty(Ctx), 0);
              Value *IdxGV = ConstantInt::get(Type::getInt64Ty(Ctx), GVIndex[GV]);
              Value *GEP = Builder.CreateInBoundsGEP(PageTable->getValueType(), PageTable, {Idx0, IdxGV});
              LoadInst *LoadPtr = Builder.CreateLoad(Int8PtrTy, GEP);
              LoadPtr->setAlignment(MaybeAlign(8));
              Value *NewPtr = Builder.CreateBitCast(LoadPtr, GV->getType());

              if (PHI)
                PHI->setIncomingValue(i, NewPtr);
              else
                Inst.setOperand(i, NewPtr);

              Changed = true;
            }
          }
        }
      }
    }
  }

  // Step 3: preserve page table from optimization if changes occurred
  if (Changed && !GVPageTable.empty())
    appendToCompilerUsed(M, GVPageTable);

  SINFO("[{}] Changes {} applied on module {}", name(), Changed ? "" : "not", M.getName());
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace omvll
