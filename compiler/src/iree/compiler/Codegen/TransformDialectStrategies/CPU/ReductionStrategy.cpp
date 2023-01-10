// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/TransformDialectStrategies/CPU/ReductionStrategy.h"

#include "iree-dialects/Dialect/LinalgTransform/StructuredTransformOpsExt.h"
#include "iree-dialects/Transforms/TransformMatchers.h"
#include "iree/compiler/Codegen/Common/TransformExtensions/CommonExtensions.h"
#include "iree/compiler/Codegen/LLVMCPU/TransformExtensions/LLVMCPUExtensions.h"
#include "iree/compiler/Codegen/Passes.h"
#include "iree/compiler/Codegen/TransformDialectStrategies/CPU/Common.h"
#include "iree/compiler/Codegen/TransformDialectStrategies/Common/AbstractReductionStrategy.h"
#include "iree/compiler/Codegen/TransformDialectStrategies/Common/Common.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "llvm/Support/Debug.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Transform/IR/TransformOps.h"
#include "mlir/Dialect/Utils/StaticValueUtils.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"

using namespace mlir;

#define DEBUG_TYPE "iree-transform-builder"
#define DBGS() (llvm::dbgs() << "[" DEBUG_TYPE "]: ")

// TODO: significantly better namespacing.
using iree_compiler::cpu::ReductionConfig;
using iree_compiler::cpu::ReductionStrategy;
using transform_ext::RegisterMatchCallbacksOp;

ReductionStrategy mlir::iree_compiler::cpu::ReductionStrategy::create(
    MLIRContext *context,
    const transform_ext::MatchedReductionCaptures &captures) {
  ReductionConfig reductionConfig = getReductionConfig(captures);
  ReductionStrategy strategy(context, captures);
  strategy.configure(reductionConfig);
  LLVM_DEBUG(DBGS() << "use CPU reduction strategy\n");
  return strategy;
}

void mlir::iree_compiler::cpu::ReductionStrategy::configure(
    const ReductionConfig &config) {
  // Block-level
  // ===========
  // Tile all the parallel dimensions to 8 for now.
  int64_t numParallelLoops = captures.reductionRank - 1;
  workgroupTileSizes.append(numParallelLoops, 8);
  vectorSize = config.vectorSize;
}

/// Builds the transform IR tiling reductions for CUDA targets. Supports
/// reductions in the last dimension, with optional leading and trailing
/// elementwise operations.
void mlir::iree_compiler::cpu::buildReductionStrategy(
    ImplicitLocOpBuilder &b, Value variantH,
    const ReductionStrategy &strategy) {
  // Step 1. Call the matcher. Note that this is the same matcher as used to
  // trigger this compilation path, so it must always apply.
  b.create<RegisterMatchCallbacksOp>();
  auto [maybeLeadingH, fillH, reductionH, maybeTrailingH] =
      unpackRegisteredMatchCallback<4>(
          b, "reduction", transform::FailurePropagationMode::Propagate,
          variantH);

  // Step 2. Use tiling to introduce a single-iteration loop mapped to a
  // single block/workgroup. Keep everything fused.
  auto [maybeLeadingHBlock, gridFillH, gridReductionH,
        maybeTiledTrailingHBlock] =
      buildReductionStrategyBlockDistribution(
          b, maybeLeadingH, fillH, reductionH, maybeTrailingH, strategy);

  // Step 3. Naive first strategy to tile the most minor dimension by
  // strategy.getVectorSize().
  for (auto [val, rank] : SmallVector<std::pair<Value, int64_t>>{
           {maybeLeadingHBlock, strategy.captures.maybeLeadingRank},
           {gridReductionH, strategy.captures.reductionRank},
           {maybeTiledTrailingHBlock, strategy.captures.maybeTrailingRank}}) {
    if (rank == 0) continue;
    SmallVector<int64_t> tileSizes(rank - 1, 0);
    tileSizes.push_back(strategy.getVectorSize());
    buildTileFuseToScfFor(b, val, {},
                          getAsOpFoldResult(b.getI64ArrayAttr(tileSizes)));
  }

  // Step 4-6. Common trailing steps.
  buildCommonTrailingStrategy(b, variantH);
}

ReductionConfig mlir::iree_compiler::cpu::getReductionConfig(
    const transform_ext::MatchedReductionCaptures &captures) {
  return ReductionConfig{16};
}