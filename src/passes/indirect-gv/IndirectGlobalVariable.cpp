// IndirectGlobalVariable.cpp

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "omvll/utils.hpp"
#include "omvll/log.hpp"

#include <vector>
#include <unordered_map>
#include <set>
#include <cstdint>

using namespace llvm;

PreservedAnalyses omvll::IndirectGlobalVariable::run(Module &M, ModuleAnalysisManager &MAM) {
  // Skip if module is globally excluded from obfuscation
  if (omvll::isModuleGloballyExcluded(&M)) {
    SINFO("Excluding module [{}]", M.getName());
    return PreservedAnalyses::all();
  }

  SINFO("[{}] Executing on module {}", name(), M.getName());
  bool Changed = false;
  // Create a random number generator for this pass
  std::unique_ptr<RandomNumberGenerator> Rng = M.createRNG(name());
  omvll::IRChangesMonitor ModuleChanges(M, name());

  // Data structures for tracking global variables
  std::unordered_map<Function*, std::set<GlobalVariable*>> FunctionGVs;
  std::vector<GlobalVariable*> GlobalVariables;
  std::unordered_map<GlobalVariable*, unsigned> GVIndex;
  std::unordered_map<GlobalVariable*, uint64_t> GVKeys;
  std::vector<GlobalVariable*> GVPageTable;

  // Phase 1: Scan all functions and collect global variable references
  for (Function &F : M) {
    if (F.isIntrinsic())
      continue;
    // Lower any constant expressions referring to globals
    LowerConstantExpr(F);
    for (BasicBlock &BB : F) {
      for (Instruction &Inst : BB) {
        if (Inst.isEHPad() || isa<CallInst>(Inst))
          continue;
        for (unsigned op = 0; op < Inst.getNumOperands(); ++op) {
          Value *Val = Inst.getOperand(op);
          if (auto *GV = dyn_cast<GlobalVariable>(Val)) {
            if (GV->isThreadLocal() || GV->isDLLImportDependent())
              continue;
            if (GV->getMetadata("noobf"))
              continue;
            // Record the global variable for this function
            FunctionGVs[&F].insert(GV);
            // If first time seeing this GV, add to global list and assign a random key
            if (GVKeys.count(GV) == 0) {
              GlobalVariables.push_back(GV);
              GVKeys[GV] = (*Rng)();
            }
          }
        }
      }
    }
  }

  // If no global variables to process, exit without changes
  if (GlobalVariables.empty()) {
    SINFO("[{}] No global variables found in module {}", name(), M.getName());
    ModuleChanges.notify(Changed);
    return ModuleChanges.report();
  }

  // Phase 2: Create a global page table (array) of pointers to all global variables
  // Element type is i8* (byte pointer)
  LLVMContext &Ctx = M.getContext();
  Type *Int8PtrTy = Type::getInt8PtrTy(Ctx);
  ArrayType *ArrayTy = ArrayType::get(Int8PtrTy, GlobalVariables.size());

  // Create the global array (page table) with external linkage
  std::string TableName = (M.getName().str() + "_IndirectGVs");
  GlobalVariable *PageTable = new GlobalVariable(
    M,
    ArrayTy,
    /*isConstant=*/false,
    GlobalValue::ExternalLinkage,
    /*Initializer=*/nullptr,
    TableName
  );
  PageTable->setAlignment(MaybeAlign(8));

  // Initialize the page table: each element is the bitcast of a global variable pointer to i8*
  std::vector<Constant*> InitValues;
  InitValues.reserve(GlobalVariables.size());
  for (GlobalVariable *GV : GlobalVariables) {
    Constant *Ptr = ConstantExpr::getBitCast(GV, Int8PtrTy);
    InitValues.push_back(Ptr);
  }
  Constant *InitArray = ConstantArray::get(ArrayTy, InitValues);
  PageTable->setInitializer(InitArray);

  // Record index of each global variable in the table
  for (unsigned i = 0; i < GlobalVariables.size(); ++i) {
    GVIndex[GlobalVariables[i]] = i;
  }
  GVPageTable.push_back(PageTable);

  // Phase 3: Replace direct references in each function with indirect loads
  for (auto &Entry : FunctionGVs) {
    Function *F = Entry.first;
    const std::set<GlobalVariable*> &FuncGVs = Entry.second;
    if (FuncGVs.empty())
      continue;
    SINFO("[{}] Visiting function {}", name(), F->getName());

    for (BasicBlock &BB : *F) {
      for (Instruction &Inst : BB) {
        if (isa<CallInst>(&Inst) || isa<CatchReturnInst>(&Inst) ||
            isa<ResumeInst>(&Inst) || Inst.isEHPad())
        {
          continue;
        }
        for (unsigned i = 0; i < Inst.getNumOperands(); ++i) {
          Value *Val = Inst.getOperand(i);
          if (auto *GV = dyn_cast<GlobalVariable>(Val)) {
            // If this global is not in our global index map, skip
            auto It = GVIndex.find(GV);
            if (It == GVIndex.end())
              continue;
            unsigned Index = It->second;

            // Determine insertion point: for PHI nodes, insert at incoming block's terminator
            Instruction *InsertPt = &Inst;
            PHINode *PHI = dyn_cast<PHINode>(&Inst);
            if (PHI) {
              BasicBlock *Incoming = PHI->getIncomingBlock(i);
              InsertPt = Incoming->getTerminator();
            }
            IRBuilder<> Builder(InsertPt);

            // Compute pointer to the table element: &PageTable[0][Index]
            Value *Idx0 = ConstantInt::get(Type::getInt64Ty(Ctx), 0);
            Value *IdxGlobal = ConstantInt::get(Type::getInt64Ty(Ctx), Index);
            Value *GEP = Builder.CreateInBoundsGEP(
              PageTable->getValueType(), PageTable, {Idx0, IdxGlobal}
            );

            // Load the i8* from the page table
            LoadInst *LoadPtr = Builder.CreateLoad(Int8PtrTy, GEP);
            LoadPtr->setAlignment(MaybeAlign(8));

            // Cast the loaded i8* back to the original global variable pointer type
            Value *NewPtr = Builder.CreateBitCast(LoadPtr, GV->getType());

            // Replace operand: for PHI set incoming value, otherwise replace uses in instruction
            if (PHI) {
              PHI->setIncomingValue(i, NewPtr);
            } else {
              Inst.replaceUsesOfWith(GV, NewPtr);
            }

            Changed = true;
          }
        }
      }
    }
  }

  // Phase 4: Finalize: if changes occurred, ensure the page table is not optimized away
  if (Changed && !GVPageTable.empty()) {
    appendToCompilerUsed(M, GVPageTable);
  }

  SINFO("[{}] Changes {} applied on module {}", name(), Changed ? "" : "not ", M.getName());
  ModuleChanges.notify(Changed);
  return ModuleChanges.report();
}
