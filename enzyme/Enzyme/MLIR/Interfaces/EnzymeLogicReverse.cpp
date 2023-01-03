#include "Interfaces/GradientUtils.h"
#include "Interfaces/GradientUtilsReverse.h"
#include "Dialect/Ops.h"
#include "Interfaces/AutoDiffOpInterface.h"
#include "Interfaces/AutoDiffTypeInterface.h"
#include "mlir/IR/FunctionInterfaces.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/SymbolTable.h"


// TODO: this shouldn't depend on specific dialects except Enzyme.
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "llvm/ADT/BreadthFirstIterator.h"
#include "mlir/IR/Dominance.h"

#include "GradientUtils.h"
#include "EnzymeLogic.h"

using namespace mlir;
using namespace mlir::enzyme;

SmallVector<mlir::Block*> getDominatorToposort(MGradientUtilsReverse *gutils){
  SmallVector<mlir::Block*> dominatorToposortBlocks;
  auto dInfo = mlir::detail::DominanceInfoBase<false>(nullptr);
  llvm::DominatorTreeBase<Block, false> & dt = dInfo.getDomTree(&(gutils->oldFunc.getBody()));
  auto root = dt.getNode(&*(gutils->oldFunc.getBody().begin()));

  for(llvm::DomTreeNodeBase<mlir::Block> * node : llvm::breadth_first(root)){
    dominatorToposortBlocks.push_back(node->getBlock());
  }
  
  return dominatorToposortBlocks;
}

void mapInvertArguments(Block * oBB, Block * reverseBB, MDiffeGradientUtilsReverse * gutils){
  for (int i = 0; i < gutils->mapBlockArguments[oBB].size(); i++){
    auto x = gutils->mapBlockArguments[oBB][i];
    OpBuilder builder(reverseBB, reverseBB->begin());
    gutils->mapInvertPointer(x.second, reverseBB->getArgument(i), builder);
  }
}

void handleReturns(Block * oBB, Block * newBB, Block * reverseBB, MDiffeGradientUtilsReverse * gutils){
  if (oBB->getNumSuccessors() == 0){
    OpBuilder forwardToBackwardBuilder(newBB, newBB->end());
    gutils->mapInvertPointer(oBB->getTerminator()->getOperand(0), gutils->newFunc.getArgument(gutils->newFunc.getNumArguments() - 1), forwardToBackwardBuilder); //TODO handle multiple return values
    Operation * newBranchOp = forwardToBackwardBuilder.create<cf::BranchOp>(gutils->getNewFromOriginal(&*(oBB->rbegin()))->getLoc(), reverseBB);
    
    Operation * returnStatement = newBB->getTerminator();
    Operation * retVal = oBB->getTerminator();
    gutils->originalToNewFnOps[retVal] = newBranchOp;
    gutils->erase(returnStatement);
  }
}

void visitChildren(Block * oBB, Block * reverseBB, MDiffeGradientUtilsReverse * gutils){
  OpBuilder revBuilder(reverseBB, reverseBB->end());
  if (!oBB->empty()){
    auto first = oBB->rbegin();
    auto last = oBB->rend();
    for (auto it = first; it != last; ++it) {
      (void)gutils->visitChildReverse(&*it, revBuilder);
    }
  }
}

void handlePredecessors(Block * oBB, Block * reverseBB, MDiffeGradientUtilsReverse * gutils){
  OpBuilder revBuilder(reverseBB, reverseBB->end());
  if (oBB->hasNoPredecessors()){
    SmallVector<mlir::Value, 2> retargs;
    for (Value attribute : gutils->oldFunc.getBody().getArguments()) {
      Value attributeGradient = gutils->invertPointerM(attribute, revBuilder);
      retargs.push_back(attributeGradient);
    }
    revBuilder.create<func::ReturnOp>(oBB->rbegin()->getLoc(), retargs);
  }
  else {
    Value cache = gutils->insertInitBackwardCache(gutils->getIndexCacheType());
    Value flag = revBuilder.create<enzyme::PopCacheOp>(oBB->rbegin()->getLoc(), gutils->getIndexType(), cache);
    SmallVector<Block *> blocks;
    SmallVector<APInt> indices;
    SmallVector<ValueRange> arguments;
    ValueRange defaultArguments;
    Block * defaultBlock;
    int i = 1;
    for (auto it = oBB->getPredecessors().begin(); it != oBB->getPredecessors().end(); it++){
      Block * predecessor = *it;
      Block * predecessorRevMode = gutils->mapReverseModeBlocks.lookupOrNull(predecessor);

      SmallVector<Value> operands;
      auto argumentsIt = gutils->mapBlockArguments.find(predecessor);
      if (argumentsIt != gutils->mapBlockArguments.end()){
        for(auto operandOld : argumentsIt->second){
          if (oBB == operandOld.first.getParentBlock() && gutils->hasInvertPointer(operandOld.second)){
            operands.push_back(gutils->invertPointerM(operandOld.second, revBuilder));
          }
          else{
            if (auto iface = operandOld.second.getType().cast<AutoDiffTypeInterface>()) {
              Value nullValue = iface.createNullValue(revBuilder, oBB->rbegin()->getLoc());
              operands.push_back(nullValue);
            }
            else{
              llvm_unreachable("non canonial null value found");
            }
          }
        }
      }

      if (it != oBB->getPredecessors().begin()){
        blocks.push_back(predecessorRevMode);
        indices.push_back(APInt(32, i++));
        arguments.push_back(ValueRange(operands));
      }
      else{
        defaultBlock = predecessorRevMode;
        defaultArguments = ValueRange(operands);
      }
    }
    //Remove Dependency to CF dialect
    if (std::next(oBB->getPredecessors().begin()) == oBB->getPredecessors().end()){
      //If there is only one block we can directly create a branch for simplicity sake
      revBuilder.create<cf::BranchOp>(gutils->getNewFromOriginal(&*(oBB->rbegin()))->getLoc(), defaultBlock, defaultArguments);
    }
    else{
      revBuilder.create<cf::SwitchOp>(oBB->rbegin()->getLoc(), flag, defaultBlock, defaultArguments, ArrayRef<APInt>(indices), ArrayRef<Block *>(blocks), ArrayRef<ValueRange>(arguments));
    }
  }
}

FunctionOpInterface mlir::enzyme::MEnzymeLogic::CreateReverseDiff(FunctionOpInterface fn, DIFFE_TYPE retType, std::vector<DIFFE_TYPE> constants, MTypeAnalysis &TA, bool returnUsed, DerivativeMode mode, bool freeMemory, size_t width, mlir::Type addedType, MFnTypeInfo type_args, std::vector<bool> volatile_args, void *augmented) {
  
  if (fn.getBody().empty()) {
    llvm::errs() << fn << "\n";
    llvm_unreachable("Differentiating empty function");
  }

  ReturnType returnValue = ReturnType::Tape;
  MDiffeGradientUtilsReverse * gutils = MDiffeGradientUtilsReverse::CreateFromClone(*this, mode, width, fn, TA, type_args, retType, /*diffeReturnArg*/ true, constants, returnValue, addedType);


  SmallVector<mlir::Block*> dominatorToposortBlocks = getDominatorToposort(gutils);

  for (auto it = dominatorToposortBlocks.rbegin(); it != dominatorToposortBlocks.rend(); ++it){
    Block * oBB = *it;
    Block * newBB = gutils->getNewFromOriginal(oBB);
    Block * reverseBB = gutils->mapReverseModeBlocks.lookupOrNull(oBB);

    mapInvertArguments(oBB, reverseBB, gutils);

    handleReturns(oBB, newBB, reverseBB, gutils);
    
    visitChildren(oBB, reverseBB, gutils);
    
    handlePredecessors(oBB, reverseBB, gutils);
  }

  auto nf = gutils->newFunc;

  llvm::errs() << "nf\n";
  nf.dump();
  llvm::errs() << "nf end\n";
  delete gutils;
  return nf;
}