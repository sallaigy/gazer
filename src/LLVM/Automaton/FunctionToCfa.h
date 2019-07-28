#ifndef GAZER_SRC_FUNCTIONTOAUTOMATA_H
#define GAZER_SRC_FUNCTIONTOAUTOMATA_H

#include "gazer/Core/GazerContext.h"
#include "gazer/Core/Expr/ExprBuilder.h"
#include "gazer/Automaton/Cfa.h"
#include "gazer/LLVM/Analysis/MemoryObject.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/ADT/MapVector.h>

namespace gazer
{

using ValueToVariableMap = llvm::DenseMap<llvm::Value*, Variable*>;

/// Stores information about loops which were transformed to automata.
struct CfaGenInfo
{
    llvm::MapVector<const llvm::Value*, Variable*> Inputs;
    llvm::MapVector<const llvm::Value*, Variable*> Outputs;
    llvm::MapVector<const llvm::Value*, Variable*> PhiInputs;
    llvm::MapVector<const llvm::Value*, VariableAssignment> LoopOutputs;

    llvm::DenseMap<llvm::Value*, Variable*> Locals;
    llvm::DenseMap<llvm::BasicBlock*, std::pair<Location*, Location*>> Blocks;

    Cfa* Automaton;

    // For automata with multiple exit paths, this variable tells us which was taken.
    Variable* ExitVariable = nullptr;
    llvm::SmallDenseMap<llvm::BasicBlock*, ExprRef<BvLiteralExpr>, 4> ExitBlocks;

    CfaGenInfo() = default;
    CfaGenInfo(CfaGenInfo&&) = default;

    CfaGenInfo(const CfaGenInfo&) = delete;
    CfaGenInfo& operator=(const CfaGenInfo&) = delete;

    void addInput(llvm::Value* value, Variable* variable) {
        Inputs[value] = variable;
    }
    void addPhiInput(llvm::Value* value, Variable* variable) {
        PhiInputs[value] = variable;
    }
    void addLocal(llvm::Value* value, Variable* variable) {
        Locals[value] = variable;
    }

    Variable* findVariable(const llvm::Value* value) {
        Variable* result = Inputs.lookup(value);
        if (result != nullptr) { return result; }

        result = PhiInputs.lookup(value);
        if (result != nullptr) { return result; }

        result = Locals.lookup(value);
        if (result != nullptr) { return result; }

        return nullptr;
    }

    Variable* findLocal(const llvm::Value* value) {
        return Locals.lookup(value);   
    }

    bool hasInput(llvm::Value* value) {
        return Inputs.find(value) != Inputs.end();
    }
    bool hasLocal(const llvm::Value* value) {
        return Locals.count(value) != 0;
    }
};

/// Helper structure for CFA generation information.
struct GenerationContext
{
    llvm::DenseMap<llvm::Function*, CfaGenInfo> FunctionMap;
    llvm::DenseMap<llvm::Loop*, CfaGenInfo> LoopMap;
    llvm::LoopInfo* LoopInfo;

    AutomataSystem& System;
    MemoryModel& TheMemoryModel;

public:
    explicit GenerationContext(AutomataSystem& system, MemoryModel& memoryModel)
        : System(system), TheMemoryModel(memoryModel)
    {}

    GenerationContext(const GenerationContext&) = delete;
    GenerationContext& operator=(const GenerationContext&) = delete;
};

class ModuleToCfa final
{
public:
    using LoopInfoMapTy = std::unordered_map<llvm::Function*, llvm::LoopInfo*>;

    static constexpr char FunctionReturnValueName[] = "RET_VAL";
    static constexpr char LoopOutputSelectorName[] = "__output_selector";

    ModuleToCfa(
        llvm::Module& module,
        LoopInfoMapTy& loops,
        GazerContext& context,
        MemoryModel& memoryModel
    )
        : mModule(module),
        mLoops(loops), mContext(context),
        mMemoryModel(memoryModel)
    {
        mSystem = std::make_unique<AutomataSystem>(context);
    }

    std::unique_ptr<AutomataSystem> generate(
        llvm::DenseMap<llvm::Value*, Variable*>& variables,
        llvm::DenseMap<Location*, llvm::BasicBlock*>& blockEntries
    );

private:
    llvm::Module& mModule;
    LoopInfoMapTy& mLoops;

    GazerContext& mContext;
    std::unique_ptr<AutomataSystem> mSystem;

    MemoryModel& mMemoryModel;

    // Generation helpers
    std::unordered_map<llvm::Function*, Cfa*> mFunctionMap;
    std::unordered_map<llvm::Loop*, Cfa*> mLoopMap;
};

class BlocksToCfa
{
public:
    BlocksToCfa(
        GenerationContext& generationContext,
        CfaGenInfo& genInfo,
        llvm::ArrayRef<llvm::BasicBlock*> blocks,
        Cfa* cfa,
        ExprBuilder& exprBuilder
    ) : mGenCtx(generationContext),
        mGenInfo(genInfo),
        mBlocks(blocks),
        mCfa(cfa),
        mExprBuilder(exprBuilder)
    {}

    void encode(llvm::BasicBlock* entry);

    ExprPtr transform(llvm::Instruction& inst);

    ExprPtr visitBinaryOperator(llvm::BinaryOperator& binop);
    ExprPtr visitSelectInst(llvm::SelectInst& select);
    ExprPtr visitICmpInst(llvm::ICmpInst& icmp);
    ExprPtr visitFCmpInst(llvm::FCmpInst& fcmp);
    ExprPtr visitCastInst(llvm::CastInst& cast);
    ExprPtr visitCallInst(llvm::CallInst& call);
    ExprPtr visitLoadInst(llvm::LoadInst& load);
    ExprPtr visitGEPOperator(llvm::GEPOperator& gep);

private:
    GazerContext& getContext() const { return mGenCtx.System.getContext(); }
private:

    bool tryToEliminate(llvm::Instruction& inst, ExprPtr expr);

    ExprPtr integerCast(llvm::CastInst& cast, ExprPtr operand, unsigned int width);
    ExprPtr operand(const llvm::Value* value);

    Variable* getVariable(const llvm::Value* value);
    ExprPtr asBool(ExprPtr operand);

    ExprPtr asInt(ExprPtr operand, unsigned int bits);
    ExprPtr castResult(ExprPtr expr, const Type& type);

    void insertOutputAssignments(CfaGenInfo& callee, std::vector<VariableAssignment>& outputArgs);
    void insertPhiAssignments(llvm::BasicBlock* source, llvm::BasicBlock* target, std::vector<VariableAssignment>& phiAssignments);

    void handleSuccessor(
        llvm::BasicBlock* succ, ExprPtr& succCondition, llvm::BasicBlock* parent,
        llvm::BasicBlock* entryBlock, Location* exit
    );
    void createExitTransition(llvm::BasicBlock* target, Location* pred, ExprPtr succCondition);
    ExprPtr getExitCondition(llvm::BasicBlock* target, Variable* exitSelector, CfaGenInfo& nestedInfo);
private:
    GenerationContext& mGenCtx;
    CfaGenInfo& mGenInfo;
    llvm::ArrayRef<llvm::BasicBlock*> mBlocks;
    Cfa* mCfa;
    ExprBuilder& mExprBuilder;
    unsigned mCounter = 0;
    llvm::DenseMap<llvm::Value*, ExprPtr> mInlinedVars;
    llvm::DenseSet<Variable*> mEliminatedVarsSet;
};

} // end namespace gazer

#endif //GAZER_SRC_FUNCTIONTOAUTOMATA_H
