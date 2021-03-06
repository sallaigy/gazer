//==-------------------------------------------------------------*- C++ -*--==//
//
// Copyright 2019 Contributors to the Gazer project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
#include "gazer/Trace/Trace.h"
#include "gazer/LLVM/Trace/TestHarnessGenerator.h"
#include "gazer/Core/LiteralExpr.h"

#include <llvm/IR/Constants.h>

#include <gtest/gtest.h>

using namespace gazer;

TEST(TestHarnessGeneratorTest, SmokeTest1)
{
    // Build a trace
    GazerContext ctx;
    BvType& bv32Ty = BvType::Get(ctx, 32);

    std::vector<std::unique_ptr<TraceEvent>> events;
    events.emplace_back(new FunctionCallEvent("__VERIFIER_nondet_int", BvLiteralExpr::Get(bv32Ty, llvm::APInt{32, 0})));
    events.emplace_back(new FunctionCallEvent("__VERIFIER_nondet_int", BvLiteralExpr::Get(bv32Ty, llvm::APInt{32, 1})));
    events.emplace_back(new FunctionCallEvent("__VERIFIER_nondet_int", BvLiteralExpr::Get(bv32Ty, llvm::APInt{32, 2})));
    events.emplace_back(new FunctionCallEvent("__VERIFIER_nondet_int", BvLiteralExpr::Get(bv32Ty, llvm::APInt{32, 3})));

    auto trace = std::make_unique<Trace>(std::move(events));

    llvm::LLVMContext llvmContext;
    auto module = std::make_unique<llvm::Module>("test1", llvmContext);
    auto llvmInt32Ty = llvm::IntegerType::getInt32Ty(llvmContext);
    module->getOrInsertFunction("__VERIFIER_nondet_int", llvm::FunctionType::get(llvmInt32Ty, /*isVarArg=*/false));
    
    // Generate the harness
    auto harness = GenerateTestHarnessModuleFromTrace(*trace, llvmContext, *module);

    auto func = harness->getFunction("__VERIFIER_nondet_int");

    ASSERT_TRUE(func != nullptr);

    auto values = harness->getGlobalVariable("gazer.trace_value.__VERIFIER_nondet_int", /*allowInternal=*/true);
    ASSERT_TRUE(values != nullptr);

    auto counter = harness->getGlobalVariable("gazer.trace_counter.__VERIFIER_nondet_int", /*allowInternal=*/true);
    ASSERT_TRUE(counter != nullptr);

    auto ca = llvm::ConstantArray::get(llvm::ArrayType::get(llvmInt32Ty, 4), {
        llvm::ConstantInt::get(llvmInt32Ty, llvm::APInt{32, 0}),
        llvm::ConstantInt::get(llvmInt32Ty, llvm::APInt{32, 1}),
        llvm::ConstantInt::get(llvmInt32Ty, llvm::APInt{32, 2}),
        llvm::ConstantInt::get(llvmInt32Ty, llvm::APInt{32, 3})
    });

    ASSERT_EQ(values->getInitializer(), ca);
}
