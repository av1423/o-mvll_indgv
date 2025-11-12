// IndirectGlobalVariable.hpp

#pragma once

#include "llvm/IR/PassManager.h"

namespace omvll {

/// Pass that replaces direct global variable accesses with indirect accesses via a page table.
struct IndirectGlobalVariable : llvm::PassInfoMixin<IndirectGlobalVariable> {
  /// Run the pass on the given module.
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);
};

} // namespace omvll
