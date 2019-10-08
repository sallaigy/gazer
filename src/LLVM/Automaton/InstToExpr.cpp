#include "gazer/LLVM/Automaton/InstToExpr.h"

#include <llvm/Support/Debug.h>

#define DEBUG_TYPE "InstToExpr"

using namespace gazer;
using namespace llvm;

ExprPtr InstToExpr::transform(const llvm::Instruction& inst)
{
    LLVM_DEBUG(llvm::dbgs() << "  Transforming instruction " << inst << "\n");
#define HANDLE_INST(OPCODE, NAME)                                       \
        if (inst.getOpcode() == (OPCODE)) {                             \
            return visit##NAME(*llvm::cast<llvm::NAME>(&inst));         \
        }                                                               \

    if (inst.isBinaryOp()) {
        return visitBinaryOperator(*dyn_cast<llvm::BinaryOperator>(&inst));
    }
    
    if (inst.isCast()) {
        return visitCastInst(*dyn_cast<llvm::CastInst>(&inst));
    }
    
    if (auto gep = llvm::dyn_cast<llvm::GEPOperator>(&inst)) {
        return visitGEPOperator(*gep);
    }

    HANDLE_INST(Instruction::ICmp,      ICmpInst)
    HANDLE_INST(Instruction::Call,      CallInst)
    HANDLE_INST(Instruction::FCmp,      FCmpInst)
    HANDLE_INST(Instruction::Select,    SelectInst)
    HANDLE_INST(Instruction::Load,      LoadInst)
    HANDLE_INST(Instruction::Alloca,    AllocaInst)

#undef HANDLE_INST

    llvm::errs() << inst << "\n";
    llvm_unreachable("Unsupported instruction kind");
}

// Transformation functions
//-----------------------------------------------------------------------------

static bool isLogicInstruction(unsigned opcode) {
    return opcode == Instruction::And || opcode == Instruction::Or || opcode == Instruction::Xor;
}

static bool isFloatInstruction(unsigned opcode) {
    return opcode == Instruction::FAdd || opcode == Instruction::FSub
           || opcode == Instruction::FMul || opcode == Instruction::FDiv;
}

static bool isNonConstValue(const llvm::Value* value) {
    return isa<Instruction>(value) || isa<Argument>(value) || isa<GlobalVariable>(value);
}

ExprPtr InstToExpr::visitBinaryOperator(const llvm::BinaryOperator& binop)
{
    auto variable = getVariable(&binop);
    auto lhs = operand(binop.getOperand(0));
    auto rhs = operand(binop.getOperand(1));
    
    auto opcode = binop.getOpcode();
    if (isLogicInstruction(opcode) && binop.getType()->isIntegerTy(1)) {
        auto boolLHS = asBool(lhs);
        auto boolRHS = asBool(rhs);

        switch (binop.getOpcode()) {
            case Instruction::And:
                return mExprBuilder.And(boolLHS, boolRHS);
            case Instruction::Or:
                return mExprBuilder.Or(boolLHS, boolRHS);
            case Instruction::Xor:
                return mExprBuilder.Xor(boolLHS, boolRHS);
            default:
                llvm_unreachable("Unknown logic instruction opcode");
        }
    }
    
    if (isFloatInstruction(opcode)) {
        ExprPtr expr;
        switch (binop.getOpcode()) {
            case Instruction::FAdd:
                return mExprBuilder.FAdd(lhs, rhs, llvm::APFloat::rmNearestTiesToEven);
            case Instruction::FSub:
                return mExprBuilder.FSub(lhs, rhs, llvm::APFloat::rmNearestTiesToEven);
            case Instruction::FMul:
                return mExprBuilder.FMul(lhs, rhs, llvm::APFloat::rmNearestTiesToEven);
            case Instruction::FDiv:
                return mExprBuilder.FDiv(lhs, rhs, llvm::APFloat::rmNearestTiesToEven);
            default:
                llvm_unreachable("Invalid floating-point operation");
        }

        return expr;
    }

    assert(variable->getType().isIntType() || variable->getType().isBvType());
    
    if (variable->getType().isBvType()) {
        const BvType* type = llvm::cast<BvType>(&variable->getType());

        auto intLHS = asBv(lhs, type->getWidth());
        auto intRHS = asBv(rhs, type->getWidth());

        #define HANDLE_INSTCASE(OPCODE, EXPRNAME)                   \
            case OPCODE:                                            \
                return mExprBuilder.EXPRNAME(intLHS, intRHS);       \

        ExprPtr expr;
        switch (binop.getOpcode()) {
            HANDLE_INSTCASE(Instruction::Add,   Add)
            HANDLE_INSTCASE(Instruction::Sub,   Sub)
            HANDLE_INSTCASE(Instruction::Mul,   Mul)
            HANDLE_INSTCASE(Instruction::SDiv,  BvSDiv)
            HANDLE_INSTCASE(Instruction::UDiv,  BvUDiv)
            HANDLE_INSTCASE(Instruction::SRem,  BvSRem)
            HANDLE_INSTCASE(Instruction::URem,  BvURem)
            HANDLE_INSTCASE(Instruction::Shl,   Shl)
            HANDLE_INSTCASE(Instruction::LShr,  LShr)
            HANDLE_INSTCASE(Instruction::AShr,  AShr)
            HANDLE_INSTCASE(Instruction::And,   BvAnd)
            HANDLE_INSTCASE(Instruction::Or,    BvOr)
            HANDLE_INSTCASE(Instruction::Xor,   BvXor)
            default:
                LLVM_DEBUG(llvm::dbgs() << "Unsupported instruction: " << binop << "\n");
                llvm_unreachable("Unsupported arithmetic instruction opcode");
        }

        #undef HANDLE_INSTCASE
    }

    if (variable->getType().isIntType()) {
        auto intLHS = asInt(lhs);
        auto intRHS = asInt(rhs);

        switch (binop.getOpcode()) {
            case Instruction::Add:
                // TODO: Add modulo to represent overflow.
                return mExprBuilder.Add(intLHS, intRHS);
            case Instruction::Sub:
                return mExprBuilder.Sub(intLHS, intRHS);
            case Instruction::Mul:
                return mExprBuilder.Mul(intLHS, intRHS);
            case Instruction::SDiv:
            case Instruction::UDiv:
                return mExprBuilder.Div(intLHS, intRHS);
            case Instruction::SRem:
            case Instruction::URem:
                // TODO: Add arithmetic Rem
                llvm_unreachable("Unsupported Rem expression");
            case Instruction::Shl:
            case Instruction::LShr:
            case Instruction::AShr:
            case Instruction::And:
            case Instruction::Or:
            case Instruction::Xor:
                // TODO: Some magic could be applied here to transform operations on
                // certain bit-patterns, e.g. all-ones, single-one, all-zero, single-zero, etc.
                return mExprBuilder.Undef(variable->getType());
        }

    }

    llvm_unreachable("Invalid binary operation kind");
}

ExprPtr InstToExpr::visitSelectInst(const llvm::SelectInst& select)
{
    Variable* selectVar = getVariable(&select);
    const Type& type = selectVar->getType();

    auto cond = asBool(operand(select.getCondition()));
    auto then = castResult(operand(select.getTrueValue()), type);
    auto elze = castResult(operand(select.getFalseValue()), type);

    return mExprBuilder.Select(cond, then, elze);
}

ExprPtr InstToExpr::unsignedCompareOperand(const ExprPtr& expr, unsigned width)
{
    // If the number is negative, we must substract it from the maximum value
    // of the given bit width to represent it as unsigned. Otherwise, we can
    // just use the original value.
    return mExprBuilder.Select(
        mExprBuilder.Lt(expr, mExprBuilder.IntLit(0)),
        mExprBuilder.Add(
            mExprBuilder.IntLit(llvm::APInt::getMaxValue(width).getZExtValue()),
            expr
        ),
        expr
    );
}

ExprPtr InstToExpr::visitICmpInst(const llvm::ICmpInst& icmp)
{
    using llvm::CmpInst;

    auto pred = icmp.getPredicate();

    auto lhs = operand(icmp.getOperand(0));
    auto rhs = operand(icmp.getOperand(1));

    if (pred == CmpInst::ICMP_EQ) {
        return mExprBuilder.Eq(lhs, rhs);
    }

    if (pred == CmpInst::ICMP_NE) {
        return mExprBuilder.NotEq(lhs, rhs);
    }

    #define HANDLE_PREDICATE(PREDNAME, EXPRNAME)                \
        case PREDNAME:                                          \
            return mExprBuilder.EXPRNAME(lhs, rhs);             \

    if (lhs->getType().isBvType()) {
        switch (pred) {
            HANDLE_PREDICATE(CmpInst::ICMP_UGT, BvUGt)
            HANDLE_PREDICATE(CmpInst::ICMP_UGE, BvUGtEq)
            HANDLE_PREDICATE(CmpInst::ICMP_ULT, BvULt)
            HANDLE_PREDICATE(CmpInst::ICMP_ULE, BvULtEq)
            HANDLE_PREDICATE(CmpInst::ICMP_SGT, BvSGt)
            HANDLE_PREDICATE(CmpInst::ICMP_SGE, BvSGtEq)
            HANDLE_PREDICATE(CmpInst::ICMP_SLT, BvSLt)
            HANDLE_PREDICATE(CmpInst::ICMP_SLE, BvSLtEq)
            default:
                llvm_unreachable("Unknown ICMP predicate.");
        }
    }

    #undef HANDLE_PREDICATE

    if (lhs->getType().isArithmetic()) {
        unsigned bw = icmp.getOperand(0)->getType()->getIntegerBitWidth();

        ExprPtr leftOp = lhs;
        ExprPtr rightOp = rhs;

        if (icmp.isUnsigned()) {
            // We need to apply some extra care here as unsigned comparisons
            // interpret the operands as unsigned values, changing some semantics.
            // As an example, -5 < x would normally be true for x = 2. However,
            // `ult i8 -5, %x` interprets -5 (0b11111011) as unsigned, thus
            // it will be compared as 251, yielding false.
            leftOp = unsignedCompareOperand(lhs, bw);
            rightOp = unsignedCompareOperand(rhs, bw);
        }

        switch (pred) {
            case CmpInst::ICMP_UGT:
            case CmpInst::ICMP_SGT:
                return mExprBuilder.Gt(leftOp, rightOp);
            case CmpInst::ICMP_UGE:
            case CmpInst::ICMP_SGE:
                return mExprBuilder.GtEq(leftOp, rightOp);
            case CmpInst::ICMP_ULT:
            case CmpInst::ICMP_SLT:
                return mExprBuilder.Lt(leftOp, rightOp);
            case CmpInst::ICMP_ULE:
            case CmpInst::ICMP_SLE:
                return mExprBuilder.LtEq(leftOp, rightOp);
            default:
                llvm_unreachable("Unknown ICMP predicate.");
        }
    }

    llvm_unreachable("Invalid type for comparison instruction!");
}

ExprPtr InstToExpr::visitFCmpInst(const llvm::FCmpInst& fcmp)
{
    using llvm::CmpInst;

    auto left = operand(fcmp.getOperand(0));
    auto right = operand(fcmp.getOperand(1));

    auto pred = fcmp.getPredicate();

    ExprPtr cmpExpr = nullptr;
    switch (pred) {
        case CmpInst::FCMP_OEQ:
        case CmpInst::FCMP_UEQ:
            cmpExpr = mExprBuilder.FEq(left, right);
            break;
        case CmpInst::FCMP_OGT:
        case CmpInst::FCMP_UGT:
            cmpExpr = mExprBuilder.FGt(left, right);
            break;
        case CmpInst::FCMP_OGE:
        case CmpInst::FCMP_UGE:
            cmpExpr = mExprBuilder.FGtEq(left, right);
            break;
        case CmpInst::FCMP_OLT:
        case CmpInst::FCMP_ULT:
            cmpExpr = mExprBuilder.FLt(left, right);
            break;
        case CmpInst::FCMP_OLE:
        case CmpInst::FCMP_ULE:
            cmpExpr = mExprBuilder.FLtEq(left, right);
            break;
        case CmpInst::FCMP_ONE:
        case CmpInst::FCMP_UNE:
            cmpExpr = mExprBuilder.Not(mExprBuilder.FEq(left, right));
            break;
        default:
            break;
    }

    ExprPtr expr = nullptr;
    if (pred == CmpInst::FCMP_FALSE) {
        expr = mExprBuilder.False();
    } else if (pred == CmpInst::FCMP_TRUE) {
        expr = mExprBuilder.True();
    } else if (pred == CmpInst::FCMP_ORD) {
        expr = mExprBuilder.And(
            mExprBuilder.Not(mExprBuilder.FIsNan(left)),
            mExprBuilder.Not(mExprBuilder.FIsNan(right))
        );
    } else if (pred == CmpInst::FCMP_UNO) {
        expr = mExprBuilder.Or(
            mExprBuilder.FIsNan(left),
            mExprBuilder.FIsNan(right)
        );
    } else if (CmpInst::isOrdered(pred)) {
        // An ordered instruction can only be true if it has no NaN operands.
        // As our comparison operators are defined to be false if either
        // argument is NaN, we we can just return the compare expression.
        expr = cmpExpr;
    } else if (CmpInst::isUnordered(pred)) {
        // An unordered instruction may be true if either operand is NaN
        expr = mExprBuilder.Or({
            mExprBuilder.FIsNan(left),
            mExprBuilder.FIsNan(right),
            cmpExpr
        });
    } else {
        llvm_unreachable("Invalid FCmp predicate");
    }

    return expr;
}

ExprPtr InstToExpr::visitCastInst(const llvm::CastInst& cast)
{
    auto castOp = operand(cast.getOperand(0));
    //if (cast.getOperand(0)->getType()->isPointerTy()) {
    //    return mMemoryModel.handlePointerCast(cast, castOp);
    //}

    if (cast.getType()->isFloatingPointTy()) {
        auto& fltTy = this->translateTypeTo<FloatType>(cast.getType());

        switch (cast.getOpcode()) {
            case Instruction::FPExt:
            case Instruction::FPTrunc:
                return mExprBuilder.FCast(castOp, fltTy, llvm::APFloat::rmNearestTiesToEven);
            case Instruction::SIToFP:
                return mExprBuilder.SignedToFp(castOp, fltTy, llvm::APFloat::rmNearestTiesToEven);
            case Instruction::UIToFP:
                return mExprBuilder.UnsignedToFp(castOp, fltTy, llvm::APFloat::rmNearestTiesToEven);
            default:
                break;
        }
    }

    if (cast.getOpcode() == Instruction::FPToSI) {
        auto& bvTy = this->translateTypeTo<BvType>(cast.getType());
        return mExprBuilder.FpToSigned(castOp, bvTy, llvm::APFloat::rmNearestTiesToEven);
    }
    
    if (cast.getOpcode() == Instruction::UIToFP) {
        auto& bvTy = this->translateTypeTo<BvType>(cast.getType());
        return mExprBuilder.FpToUnsigned(castOp, bvTy, llvm::APFloat::rmNearestTiesToEven);
    }
    
    if (cast.getType()->isPointerTy()) {
        return mMemoryModel.handlePointerCast(cast);
    }

    if (castOp->getType().isBoolType()) {
        return boolToIntCast(cast, castOp);
    }
    
    // If the instruction truncates an integer to an i1 boolean, cast to boolean instead.
    if (cast.getType()->isIntegerTy(1)
        && cast.getOpcode() == Instruction::Trunc
        && getVariable(&cast)->getType().isBoolType()    
    ) {
        return asBool(castOp);
    }

    if (castOp->getType().isBvType()) {
        return integerCast(
            cast, castOp, dyn_cast<BvType>(&castOp->getType())->getWidth()
        );
    }

    if (castOp->getType().isIntType()) {
        // ZExt and SExt are no-op in this case.
        if (cast.getOpcode() == Instruction::ZExt || cast.getOpcode() == Instruction::SExt) {
            return castOp;
        }

        if (cast.getOpcode() == Instruction::Trunc) {
            // We can get the lower 'w' bits of 'n' if we do 'n mod 2^w'.
            // However, due to LLVM's two's complement representation, this
            // could turn into a signed number.
            // For example:
            //  trunc i6 51 to i4: 11|0011 --> 3
            //  trunc i6 60 to i4: 11|1100 --> -4
            // To overcome this, we check the sign bit of the resulting value
            // and if it set, we substract '2^w' from the result.
            auto maxVal = mExprBuilder.IntLit(
                llvm::APInt::getMaxValue(cast.getType()->getIntegerBitWidth()).getZExtValue()
            );
            auto maxValDiv2 = mExprBuilder.IntLit(
                llvm::APInt::getMaxValue(cast.getType()->getIntegerBitWidth() - 1).getZExtValue()
            );
            auto modVal = mExprBuilder.Mod(castOp, maxVal);

            return mExprBuilder.Select(
                mExprBuilder.Eq(
                    mExprBuilder.Mod(
                        mExprBuilder.Div(castOp, maxValDiv2),
                        mExprBuilder.IntLit(2)
                    ),
                    mExprBuilder.IntLit(0)
                ),
                modVal,
                mExprBuilder.Sub(modVal, maxVal)
            );
        }

        return mExprBuilder.Undef(castOp->getType());
    }

    if (cast.getOpcode() == Instruction::BitCast) {
        // TODO...
    }

    llvm_unreachable("Unsupported cast operation");
}

ExprPtr InstToExpr::integerCast(const llvm::CastInst& cast, const ExprPtr& operand, unsigned width)
{
    auto variable = getVariable(&cast);

    if (auto bvTy = llvm::dyn_cast<gazer::BvType>(&variable->getType())) {
        ExprPtr intOp = asBv(operand, width);

        switch (cast.getOpcode()) {
            case Instruction::ZExt:
                return mExprBuilder.ZExt(intOp, *bvTy);
            case Instruction::SExt:
                return mExprBuilder.SExt(intOp, *bvTy);
            case Instruction::Trunc:
                return mExprBuilder.Trunc(intOp, *bvTy);
            default:
                llvm_unreachable("Unhandled integer cast operation");
        }
    }

    llvm_unreachable("Invalid bit-vector type!");
}

ExprPtr InstToExpr::boolToIntCast(const llvm::CastInst& cast, const ExprPtr& operand)
{
    auto variable = getVariable(&cast);

    auto one  = llvm::APInt{1, 1};
    auto zero = llvm::APInt{1, 0};

    if (auto bvTy = dyn_cast<gazer::BvType>(&variable->getType())) {
        switch (cast.getOpcode())
        {
            case Instruction::ZExt:
                return mExprBuilder.Select(
                    operand,
                    mExprBuilder.BvLit(one.zext(bvTy->getWidth())),
                    mExprBuilder.BvLit(zero.zext(bvTy->getWidth()))
                );
            case Instruction::SExt:
                return mExprBuilder.Select(
                    operand,
                    mExprBuilder.BvLit(one.sext(bvTy->getWidth())),
                    mExprBuilder.BvLit(zero.sext(bvTy->getWidth()))
                );
            default:
                llvm_unreachable("Invalid integer cast operation");
        }
    }

    if (auto intTy = dyn_cast<gazer::IntType>(&variable->getType())) {
        switch (cast.getOpcode())
        {
            case Instruction::ZExt:
                return mExprBuilder.Select(
                    operand,
                    mExprBuilder.IntLit(1),
                    mExprBuilder.IntLit(0)
                );
            case Instruction::SExt: {
                // In two's complement 111..11 corresponds to -1, 111..10 to -2
                return mExprBuilder.Select(
                    operand,
                    mExprBuilder.IntLit(-1),
                    mExprBuilder.IntLit(-2)
                );
            }
            default:
                llvm_unreachable("Invalid integer cast operation");
        }
    }
    
    llvm_unreachable("Invalid integer cast type!");
}

ExprPtr InstToExpr::visitCallInst(const llvm::CallInst& call)
{
    gazer::Type& callTy = this->translateType(call.getType());

    const Function* callee = call.getCalledFunction();
    if (callee == nullptr) {
        return UndefExpr::Get(callTy);
        // This is an indirect call, use the memory model to resolve it.
        //return mMemoryModel.handleCall(call);
    }

    return UndefExpr::Get(callTy);
}

ExprPtr InstToExpr::visitLoadInst(const llvm::LoadInst& load)
{
    return mMemoryModel.handleLoad(load);
}

ExprPtr InstToExpr::visitAllocaInst(const llvm::AllocaInst& alloc)
{
    return mMemoryModel.handleAlloca(alloc);
}

ExprPtr InstToExpr::visitGEPOperator(const llvm::GEPOperator& gep)
{
    return mMemoryModel.handleGetElementPtr(gep);
}

ExprPtr InstToExpr::operand(const Value* value)
{
    if (auto ci = dyn_cast<ConstantInt>(value)) {
        // Check for boolean literals
        if (ci->getType()->isIntegerTy(1)) {
            return ci->isZero() ? mExprBuilder.False() : mExprBuilder.True();
        }

        switch (mSettings.getIntRepresentation()) {
            case IntRepresentation::BitVectors:
                return mExprBuilder.BvLit(
                    ci->getValue().getLimitedValue(),
                    ci->getType()->getIntegerBitWidth()
                );
            case IntRepresentation::Integers:
                return mExprBuilder.IntLit(ci->getSExtValue());
        }

        llvm_unreachable("Invalid int representation strategy!");
    }
    
    if (const llvm::ConstantFP* cfp = dyn_cast<llvm::ConstantFP>(value)) {
        return mExprBuilder.FloatLit(cfp->getValueAPF());
    }
    
    if (value->getType()->isPointerTy()) {
        return mMemoryModel.handlePointerValue(value);
    }

    if (isNonConstValue(value)) {
        auto result = this->lookupInlinedVariable(value);
        if (result != nullptr) {
            return result;
        }

        return getVariable(value)->getRefExpr();
    }
    
    if (isa<llvm::UndefValue>(value)) {
        return mExprBuilder.Undef(this->translateType(value->getType()));
    }
    
    LLVM_DEBUG(llvm::dbgs() << "  Unhandled value for operand: " << *value << "\n");
    llvm_unreachable("Unhandled value type");
}

ExprPtr InstToExpr::asBool(const ExprPtr& operand)
{
    if (operand->getType().isBoolType()) {
        return operand;
    }
    
    if (operand->getType().isBvType()) {
        auto bvTy = dyn_cast<BvType>(&operand->getType());
        unsigned bits = bvTy->getWidth();

        return mExprBuilder.Select(
            mExprBuilder.Eq(operand, mExprBuilder.BvLit(0, bits)),
            mExprBuilder.False(),
            mExprBuilder.True()
        );
    }

    if (operand->getType().isIntType()) {
        return mExprBuilder.Select(
            mExprBuilder.Eq(operand, mExprBuilder.IntLit(0)),
            mExprBuilder.False(),
            mExprBuilder.True()
        );
    }

    llvm_unreachable("Attempt to cast to bool from unsupported type.");
}

ExprPtr InstToExpr::asBv(const ExprPtr& operand, unsigned int bits)
{
    if (operand->getType().isBoolType()) {
        return mExprBuilder.Select(
            operand,
            mExprBuilder.BvLit(1, bits),
            mExprBuilder.BvLit(0, bits)
        );
    }
    
    if (operand->getType().isBvType()) {
        return operand;
    }

    llvm_unreachable("Attempt to cast to bitvector from unsupported type.");
}

ExprPtr InstToExpr::asInt(const ExprPtr& operand)
{
    if (operand->getType().isBoolType()) {
        return mExprBuilder.Select(
            operand,
            mExprBuilder.IntLit(1),
            mExprBuilder.IntLit(0)
        );
    }
    
    if (operand->getType().isIntType()) {
        return operand;
    }

    llvm_unreachable("Attempt to cast to int from unsupported type.");
}

ExprPtr InstToExpr::castResult(const ExprPtr& expr, const Type& type)
{
    if (type.isBoolType()) {
        return asBool(expr);
    }
    
    if (type.isBvType()) {
        return asBv(expr, dyn_cast<BvType>(&type)->getWidth());
    }

    if (type.isIntType()) {
        return asInt(expr);
    }

    llvm_unreachable("Invalid cast result type");
}

gazer::Type& InstToExpr::translateType(const llvm::Type* type)
{
    return mMemoryModel.translateType(type);
}
