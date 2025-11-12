#pragma once

//
// This file is distributed under the Apache License v2.0. See LICENSE for
// details.
//

#include "llvm/IR/PassManager.h"
#include <memory>

namespace omvll {

/// Pass that replaces direct global variable accesses with indirect accesses via a page table.
struct IndirectGlobalVariable : llvm::PassInfoMixin<IndirectGlobalVariable> {
  /// Run the pass on the given module.
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);

private:
  /// Random number generator used for assigning keys to global variables
  std::unique_ptr<llvm::RandomNumberGenerator> RNG;

  // Optional: helper function to lower constant expressions in a function
  void lowerConstantExpr(llvm::Function &F);
};

} // namespace omvll
