#include "gazer/LLVM/Analysis/BmcPass.h"
#include "gazer/LLVM/Analysis/TopologicalSort.h"
#include "gazer/LLVM/Analysis/CfaBuilderPass.h"
#include "gazer/LLVM/BMC/BmcTrace.h"
#include "gazer/Z3Solver/Z3Solver.h"
#include "gazer/Core/Expr.h"
#include "gazer/Core/LiteralExpr.h"
#include "gazer/Core/ExprTypes.h"
#include "gazer/Core/Utils/ExprUtils.h"

#include "gazer/LLVM/Ir2Expr.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/InstIterator.h>



#include <vector>

using namespace gazer;
using namespace llvm;

char BmcPass::ID = 0;

#if 0
namespace
{
class BmcTrace
{
public:
    struct LocationInfo
    {
        int row;
        int column;
    };

    struct Assignment
    {
        Variable* variable;
        std::shared_ptr<LiteralExpr> expr;

        llvm::Value* value;
        LocationInfo location;
        std::string variableName;
    };
private:
    BmcTrace(std::vector<BasicBlock*> blocks, std::vector<Assignment> assignments = {})
        : mBlocks(blocks), mAssignments(assignments)
    {}

public:
    static BmcTrace Create(
        llvm::Function& function,
        TopologicalSort& topo,
        llvm::DenseMap<BasicBlock*, size_t>& blocks,
        llvm::DenseMap<BasicBlock*, llvm::Value*>& preds,
        BasicBlock* errorBlock,
        Valuation& model,
        const InstToExpr::ValueToVariableMapT& valueMap
    );

    using iterator = std::vector<Assignment>::iterator;
    iterator begin() { return mAssignments.begin(); }
    iterator end() { return mAssignments.end(); }

private:
    std::vector<Assignment> mAssignments;
    std::vector<BasicBlock*> mBlocks;
};

}


BmcTrace BmcTrace::Create(
    Function& function,
    TopologicalSort& topo,
    llvm::DenseMap<BasicBlock*, size_t>& blocks,
    llvm::DenseMap<BasicBlock*, llvm::Value*>& preds,
    BasicBlock* errorBlock,
    Valuation& model,
    const InstToExpr::ValueToVariableMapT& valueMap
) {
    std::vector<Assignment> assigns;
    std::vector<BasicBlock*> traceBlocks;

    bool hasParent = true;
    BasicBlock* current = errorBlock;

    while (hasParent) {
        traceBlocks.push_back(current);

        auto predRes = preds.find(current);
        if (predRes != preds.end()) {
            size_t predId;
            if (auto ci = llvm::dyn_cast<llvm::ConstantInt>(predRes->second)) {
                predId = ci->getLimitedValue();
            } else {
                auto varRes = valueMap.find(predRes->second);
                assert(varRes != valueMap.end()
                    && "Pred variables should be in the variable map");
                
                auto exprRes = model.find(varRes->second);
                assert(exprRes != model.end()
                    && "Pred values should be present in the model");
                
                auto lit = llvm::dyn_cast<IntLiteralExpr>(exprRes->second.get());
                predId = lit->getValue().getLimitedValue();

            }

            //current->printAsOperand(llvm::errs());
            //llvm::errs() << " PRED " << predId << "\n";
            current = topo[predId];
        } else {
            hasParent = false;
        }
    }

    std::reverse(traceBlocks.begin(), traceBlocks.end());
    
    for (BasicBlock* bb : traceBlocks) {
        for (Instruction& instr : *bb) {
            llvm::CallInst* call = llvm::dyn_cast<llvm::CallInst>(&instr);
            if (!call) {
                continue;
            }

            llvm::Function* callee = call->getCalledFunction();

            if (auto dvi = llvm::dyn_cast<llvm::DbgValueInst>(&instr)) {
                if (dvi->getValue() && dvi->getVariable()) {
                    llvm::Value* value = dvi->getValue();
                    llvm::DILocalVariable* diVar = dvi->getVariable();

                    auto result = valueMap.find(value);
                    
                    if (result == valueMap.end()) {
                        // This is an unknown value for a given variable
                        //assigns.push_back({
                        //    nullptr, nullptr, value, {-1, -1}, diVar->getName()
                        //});
                        continue;
                    }
                    //assert(result != valueMap.end()
                    //    && "Named values should be present in the value map");

                    Variable* variable = result->second;
                    auto exprResult = model.find(variable);

                    if (exprResult == model.end()) {
                        continue;
                    }
                    
                    std::shared_ptr<LiteralExpr> expr = exprResult->second;
                    LocationInfo location = { -1, -1 };

                    if (auto valInst = llvm::dyn_cast<llvm::Instruction>(value)) {
                        llvm::DebugLoc debugLoc = nullptr;
                        if (valInst->getDebugLoc()) {
                            debugLoc = valInst->getDebugLoc();
                        } else if (dvi->getDebugLoc()) {
                            debugLoc = dvi->getDebugLoc();
                        }

                        if (debugLoc) {
                            location.row = debugLoc->getLine();
                            location.column = debugLoc->getColumn();
                        }
                    }

                    assigns.push_back({
                        variable,
                        exprResult->second,
                        value,
                        location,
                        diVar->getName()
                    });
                }
            } else if (callee->getName() == "gazer.inlined_global.write") {
                auto mdValue = cast<MetadataAsValue>(call->getArgOperand(0))->getMetadata();
                auto value = cast<ValueAsMetadata>(mdValue)->getValue();

                auto mdGlobal = dyn_cast<DIGlobalVariable>(
                    cast<MetadataAsValue>(call->getArgOperand(1))->getMetadata()
                );

                std::shared_ptr<LiteralExpr> expr = nullptr;
                if (auto ci = dyn_cast<ConstantInt>(value)) {
                    expr = IntLiteralExpr::get(
                        *IntType::get(ci->getBitWidth()),
                        ci->getValue()
                    );
                } else {
                    auto result = valueMap.find(value);
                    if (result != valueMap.end()) {
                        Variable* variable = result->second;
                        auto exprResult = model.find(variable);

                        if (exprResult != model.end()) {
                            expr = exprResult->second;
                        }
                    }
                }

                LocationInfo location = { -1, -1 };
                if (auto debugLoc = call->getDebugLoc()) {
                    location.row = debugLoc->getLine();
                    location.column = debugLoc->getColumn();
                }

                assigns.push_back({
                    nullptr,
                    expr,
                    value,
                    location,
                    mdGlobal->getName()
                });
            }
        }
    }

    return BmcTrace(traceBlocks, assigns);
}
#endif

static bool isErrorFunctionName(llvm::StringRef name)
{
    return name == "__VERIFIER_error" || name == "__assert_fail"
        || name == "__gazer_error";
}

static bool isErrorBlock(const BasicBlock& bb)
{
    for (auto& instr : bb) {
        if (instr.getOpcode() == llvm::Instruction::Call) {
            auto call = llvm::dyn_cast<llvm::CallInst>(&instr);
            llvm::Function* callee = call->getCalledFunction();

            if (isErrorFunctionName(callee->getName())) {
                return true;
            }
        }
    }

    return false;
}

static ExprPtr encodeEdge(
    BasicBlock* from,
    BasicBlock* to,
    InstToExpr& ir2expr,
    llvm::DenseMap<const BasicBlock*, ExprPtr>& cache)
{
    /*
    llvm::errs() << "encoding edge between ";
    from->printAsOperand(llvm::errs());
    llvm::errs() << " and ";
    to->printAsOperand(llvm::errs());
    llvm::errs() << "\n";
    */

    std::vector<ExprPtr> exprs;

    // Find which branch we are taking
    auto terminator = from->getTerminator();

    size_t succIdx = 0;
    while (succIdx != terminator->getNumSuccessors()) {
        if (terminator->getSuccessor(succIdx) == to) {
            break;
        }

        succIdx++;
    }

    assert(succIdx != terminator->getNumSuccessors()
        && "'From' should be a parent of 'to'");

    auto cacheResult = cache.find(from);
    if (cacheResult == cache.end()) {
        std::vector<ExprPtr> fromExprs;

        // We are starting from the non-phi nodes at the 'from' part of the block
        for (auto it = from->getFirstInsertionPt(); it != from->end(); ++it) {
            if (it->isTerminator()) {
                // TODO: Quick hack to avoid handling terminators
                continue;
            }

            fromExprs.push_back(ir2expr.transform(*it));
        }

        auto fromExpr = ir2expr.getBuilder()->And(fromExprs.begin(), fromExprs.end());
        cache[from] = fromExpr;
        exprs.push_back(fromExpr);
    } else {
        exprs.push_back(cacheResult->second);
    }

    // Handle the branch
    exprs.push_back(ir2expr.transform(*from->getTerminator(), succIdx));

    // Handle the PHI nodes
    for (auto it = to->begin(); it != to->getFirstInsertionPt(); ++it) {
        auto expr = ir2expr.transform(*it, succIdx, from);
        //FormatPrintExpr(expr, llvm::errs());
        //gllvm::errs() << "\n";
        exprs.push_back(expr);
    }

    return ir2expr.getBuilder()->And(exprs.begin(), exprs.end());
}

/**
 * Encodes the bounded reachability of assertion failures into SMT formulas.
 */
static llvm::SmallDenseMap<BasicBlock*, ExprPtr, 1> encode(
    Function& function,
    TopologicalSort& topo,
    SymbolTable& symbols,
    InstToExpr& ir2expr
) {
    ExprBuilder* builder = ir2expr.getBuilder();

    size_t numBB = function.getBasicBlockList().size();

    llvm::SmallDenseMap<size_t, BasicBlock*, 1> errorBlocks;
    for (size_t i = 0; i < numBB; ++i) {
        if (isErrorBlock(*topo[i])) {
            errorBlocks.insert({i, topo[i]});
        }
    }

    llvm::DenseMap<const BasicBlock*, size_t> blocks(numBB);
    for (size_t i = 0; i < numBB; ++i) {
        blocks.insert({topo[i], i});
    }

    llvm::DenseMap<const BasicBlock*, ExprPtr> formulaCache;

    std::vector<ExprPtr> dp(numBB, BoolLiteralExpr::getFalse());
    llvm::SmallDenseMap<BasicBlock*, ExprPtr, 1> result;

    // The entry block is always reachable
    dp[0] = BoolLiteralExpr::getTrue();

    for (size_t i = 1; i < numBB; ++i) {
        BasicBlock* bb = topo[i];
        llvm::SmallVector<ExprPtr, 2> exprs;

        for (BasicBlock* pred : llvm::predecessors(bb)) {
            auto predIt = blocks.find(pred);
            assert(predIt != blocks.end() && "All blocks must be present in the block map");

            size_t predIdx = blocks[pred];
            assert(predIdx < i && "Predecessors must be before block in a topological sort");

            ExprPtr predFormula = dp[predIdx];
            if (predFormula != BoolLiteralExpr::getFalse()) {
                auto andFormula = builder->And(
                    predFormula,
                    encodeEdge(pred, bb, ir2expr, formulaCache)
                );
                exprs.push_back(andFormula);
            }
        }

        if (!exprs.empty()) {
            auto expr = builder->Or(exprs.begin(), exprs.end());
            dp[i] = expr;
        }
    }

    for (auto& entry : errorBlocks) {
        size_t idx = entry.first;
        BasicBlock* block  = entry.second;

        result[block] = dp[idx];
    }

    return result;
}

void BmcPass::getAnalysisUsage(llvm::AnalysisUsage& au) const
{
    au.addRequired<TopologicalSortPass>();
    //au.addRequired<CfaBuilderPass>();

    au.setPreservesAll();
}

bool BmcPass::runOnFunction(llvm::Function& function)
{
    TopologicalSort& topo = getAnalysis<TopologicalSortPass>().getTopologicalSort();    

    SymbolTable st;
    llvm::DenseMap<const Variable*, llvm::Value*> variableToValueMap;

    llvm::errs() << "Program size: \n";
    llvm::errs() << "   Blocks: " << function.getBasicBlockList().size() << "\n";
    llvm::errs() << "Encoding program into SMT formula.\n";

    auto& context = function.getContext();

    llvm::DenseMap<BasicBlock*, size_t> blocks(topo.size());
    llvm::DenseMap<BasicBlock*, llvm::Value*> preds;

    for (size_t i = 0; i < topo.size(); ++i) {
        blocks.insert({topo[i], i});
    }

    // Create predecessor identifications
    llvm::Type* predTy = llvm::IntegerType::get(context, 32);
    for (BasicBlock& bb : function) {
        size_t bbID = blocks[&bb];

        auto phi = llvm::PHINode::Create(
            predTy,
            0,
            "pred" + llvm::Twine(bbID)
        );

        for (BasicBlock* pred : llvm::predecessors(&bb)) {
            size_t predID = blocks[pred];

            phi->addIncoming(
                llvm::ConstantInt::get(
                    predTy,
                    llvm::APInt(predTy->getIntegerBitWidth(), predID)
                ),
                pred
            );
        }

        if (phi->getNumIncomingValues() > 1) {
            bb.getInstList().push_front(phi);
            preds[&bb] = phi;
        } else {
            if (phi->getNumIncomingValues() == 1) {
                preds[&bb] = phi->getIncomingValue(0);
            }

            phi->dropAllReferences();
            phi->deleteValue();
        }
    }
   
    auto builder = CreateFoldingExprBuilder();
    //auto builder = CreateExprBuilder();
    InstToExpr ir2expr(function, st, builder.get(), &variableToValueMap);
    auto result = encode(function, topo, st, ir2expr);

    for (auto& entry : result) {
        llvm::errs() << "Checking for error block '";
        entry.first->printAsOperand(llvm::errs());
        llvm::errs() << "'\n";

        CachingZ3Solver solver(st);

        llvm::errs() << "   Transforming formula.\n";

        try {
            //entry.second->print(llvm::errs());
            //FormatPrintExpr(entry.second, llvm::errs());
            //llvm::errs() << "\n";
            solver.add(entry.second);

            llvm::errs() << "   Running solver.\n";
            auto status = solver.run();

            if (status == Solver::SAT) {
                llvm::errs() << "   Formula is SAT\n";
                Valuation model = solver.getModel();
                //model.print(llvm::errs());
                
                // Display a counterexample trace
                auto trace = BmcTrace::Create(
                    topo,
                    blocks,
                    preds,
                    entry.first,
                    model,
                    ir2expr.getVariableMap()
                );

                auto writer = bmc::CreateTextTraceWriter(llvm::errs());
                llvm::errs() << "Error trace:\n";
                llvm::errs() << "-----------\n";
                writer->write(trace);

                llvm::errs() << "Assertion failure found.\n";
                break;
            } else if (status == Solver::UNSAT) {
                llvm::errs() << "   Formula is UNSAT\n";
            } else {
                llvm::errs() << "   Unknown solver state.";
            }
        } catch (z3::exception& e) {
            llvm::errs() << e.msg() << "\n";
        }
    }

    return false;
}
