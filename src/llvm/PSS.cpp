#include <cassert>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Constant.h>
#include <llvm/Support/CFG.h>
#include <llvm/Support/raw_os_ostream.h>

#include "analysis/PSS.h"
#include "PSS.h"

#ifdef DEBUG_ENABLED
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#endif

namespace dg {
namespace analysis {
namespace pss {

/* keep it for debugging */
#if 0
static std::string
getInstName(const llvm::Value *val)
{
    using namespace llvm;

    std::ostringstream ostr;
    raw_os_ostream ro(ostr);

    assert(val);
    if (const Function *F = dyn_cast<Function>(val))
        ro << F->getName().data();
    else
        ro << *val;

    ro.flush();

    // break the string if it is too long
    return ostr.str();
}

const char *__get_name(const llvm::Value *val, const char *prefix)
{
    static std::string buf;
    buf.reserve(255);
    buf.clear();

    std::string nm = getInstName(val);
    if (prefix)
        buf.append(prefix);

    buf.append(nm);

    return buf.c_str();
}

{
    const char *name = __get_name(val, prefix);
}

{
    if (prefix) {
        std::string nm;
        nm.append(prefix);
        nm.append(name);
    } else
}
#endif

enum MemAllocationFuncs {
    NONEMEM = 0,
    MALLOC,
    CALLOC,
    ALLOCA,
};

static int getMemAllocationFunc(const llvm::Function *func)
{
    if (!func || !func->hasName())
        return NONEMEM;

    const char *name = func->getName().data();
    if (strcmp(name, "malloc") == 0)
        return MALLOC;
    else if (strcmp(name, "calloc") == 0)
        return CALLOC;
    else if (strcmp(name, "alloca") == 0)
        return ALLOCA;
    else if (strcmp(name, "realloc") == 0)
        // FIXME
        assert(0 && "realloc not implemented yet");

    return NONEMEM;
}

static inline unsigned getPointerBitwidth(const llvm::DataLayout *DL,
                                          const llvm::Value *ptr)

{
    const llvm::Type *Ty = ptr->getType();
    return DL->getPointerSizeInBits(Ty->getPointerAddressSpace());
}

static uint64_t getAllocatedSize(llvm::Type *Ty, const llvm::DataLayout *DL)
{
    // Type can be i8 *null or similar
    if (!Ty->isSized())
            return 0;

    return DL->getTypeAllocSize(Ty);
}

Pointer LLVMPSSBuilder::handleConstantBitCast(const llvm::BitCastInst *BC)
{
    using namespace llvm;

    if (!BC->isLosslessCast()) {
        errs() << "WARN: Not a loss less cast unhandled ConstExpr"
               << *BC << "\n";
        abort();
        return PointerUnknown;
    }

    const Value *llvmOp = BC->stripPointerCasts();
    // (possibly recursively) get the operand of this bit-cast
    PSSNode *op = getOperand(llvmOp);
    assert(op->pointsTo.size() == 1
           && "Constant BitCast with not only one pointer");

    return *op->pointsTo.begin();
}

Pointer LLVMPSSBuilder::handleConstantGep(const llvm::GetElementPtrInst *GEP)
{
    using namespace llvm;

    const Value *op = GEP->getPointerOperand();
    Pointer pointer(UNKNOWN_MEMORY, UNKNOWN_OFFSET);

    // get operand PSSNode (this may result in recursive call,
    // if this gep is recursively defined)
    PSSNode *opNode = getOperand(op);
    assert(opNode->pointsTo.size() == 1
           && "Constant node has more that 1 pointer");
    pointer = *(opNode->pointsTo.begin());

    unsigned bitwidth = getPointerBitwidth(DL, op);
    APInt offset(bitwidth, 0);

    // get offset of this GEP
    if (GEP->accumulateConstantOffset(*DL, offset)) {
        if (offset.isIntN(bitwidth) && !pointer.offset.isUnknown())
            pointer.offset = offset.getZExtValue();
        else
            errs() << "WARN: Offset greater than "
                   << bitwidth << "-bit" << *GEP << "\n";
    }

    return pointer;
}

Pointer LLVMPSSBuilder::getConstantExprPointer(const llvm::ConstantExpr *CE)
{
    using namespace llvm;

    Pointer pointer(UNKNOWN_MEMORY, UNKNOWN_OFFSET);
    const Instruction *Inst = const_cast<ConstantExpr*>(CE)->getAsInstruction();

    if (const GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Inst)) {
        pointer = handleConstantGep(GEP);
    } else if (const BitCastInst *BC = dyn_cast<BitCastInst>(Inst)) {
        pointer = handleConstantBitCast(BC);
    } else if (isa<IntToPtrInst>(Inst)) {
        // FIXME: we can do more!
        pointer = PointerUnknown;
    } else {
            errs() << "ERR: Unsupported ConstantExpr " << *CE << "\n";
            abort();
    }

    delete Inst;
    return pointer;
}

PSSNode *LLVMPSSBuilder::createConstantExpr(const llvm::ConstantExpr *CE)
{
    Pointer ptr = getConstantExprPointer(CE);
    PSSNode *node = new PSSNode(pss::CONSTANT, ptr.target, ptr.offset);

    addNode(CE, node);

    assert(node);
    return node;
}

PSSNode *LLVMPSSBuilder::getConstant(const llvm::Value *val)
{
    if (llvm::isa<llvm::ConstantPointerNull>(val)) {
        return NULLPTR;
    } else if (const llvm::ConstantExpr *CE
                    = llvm::dyn_cast<llvm::ConstantExpr>(val)) {
        return createConstantExpr(CE);
    } else if (llvm::isa<llvm::Function>(val)) {
        PSSNode *ret = new PSSNode(FUNCTION);
        addNode(val, ret);

        return ret;
    } else {
        llvm::errs() << "Unsupported constant: " << *val << "\n";
        abort();
    }
}

PSSNode *LLVMPSSBuilder::getOperand(const llvm::Value *val)
{
    PSSNode *op = nodes_map[val];
    if (!op)
        op = getConstant(val);

    // if the operand is a call, use the return node of the call instead
    // - this is the one that contains returned pointers
    if (op->getType() == pss::CALL
        || op->getType() == pss::CALL_FUNCPTR) {
        op = op->getPairedNode();
    }

    assert(op && "Did not find an operand");
    return op;
}

static PSSNode *createDynamicAlloc(const llvm::CallInst *CInst, int type)
{
    using namespace llvm;

    const Value *op;
    uint64_t size = 0, size2 = 0;
    PSSNode *node = new PSSNode(pss::DYN_ALLOC);

    switch (type) {
        case MALLOC:
            node->setIsHeap();
        case ALLOCA:
            op = CInst->getOperand(0);
            break;
        case CALLOC:
            node->setIsHeap();
            node->setZeroInitialized();
            op = CInst->getOperand(1);
            break;
        default:
            errs() << *CInst << "\n";
            assert(0 && "unknown memory allocation type");
    };

    if (const ConstantInt *C = dyn_cast<ConstantInt>(op)) {
        size = C->getLimitedValue();
        // if the size cannot be expressed as an uint64_t,
        // just set it to 0 (that means unknown)
        if (size == ~((uint64_t) 0))
            size = 0;

        // if this is call to calloc, the size is given
        // in the first argument too
        if (type == CALLOC) {
            C = dyn_cast<ConstantInt>(CInst->getOperand(0));
            if (C) {
                size2 = C->getLimitedValue();
                if (size2 == ~((uint64_t) 0))
                    size2 = 0;
                else
                    // OK, if getting the size fails, we end up with
                    // just 1 * size - still better than 0 and UNKNOWN
                    // (it may be cropped later anyway)
                    size *= size2;
            }
        }
    }

    node->setSize(size);
    return node;
}

std::pair<PSSNode *, PSSNode *>
LLVMPSSBuilder::createDynamicMemAlloc(const llvm::CallInst *CInst, int type)
{
    PSSNode *node = createDynamicAlloc(CInst, type);
    addNode(CInst, node);

    // we return (node, node), so that the parent function
    // will seamlessly connect this node into the graph
    return std::make_pair(node, node);
}

std::pair<PSSNode *, PSSNode *>
LLVMPSSBuilder::createCallToFunction(const llvm::CallInst *CInst,
                                     const llvm::Function *F)
{
    PSSNode *callNode, *returnNode;

    // the operands to the return node (which works as a phi node)
    // are going to be added when the subgraph is built
    returnNode = new PSSNode(pss::CALL_RETURN, nullptr);
    callNode = new PSSNode(pss::CALL, nullptr);

    returnNode->setPairedNode(callNode);
    callNode->setPairedNode(returnNode);


    // reuse built subgraphs if available
    Subgraph subg = subgraphs_map[F];
    if (!subg.root) {
        // create new subgraph
        buildLLVMPSS(*F);
        // FIXME: don't find it again, return it from buildLLVMPSS
        // this is redundant
        subg = subgraphs_map[F];
    }

    assert(subg.root && subg.ret);

    // add an edge from last argument to root of the subgraph
    // and from the subprocedure return node (which is one - unified
    // for all return nodes) to return from the call
    callNode->addSuccessor(subg.root);
    subg.ret->addSuccessor(returnNode);

    // add pointers to the arguments PHI nodes
    int idx = 0;
    PSSNode *arg = subg.args.first;
    for (auto A = F->arg_begin(), E = F->arg_end(); A != E; ++A, ++idx) {
        if (A->getType()->isPointerTy()) {
            assert(arg && "BUG: do not have argument");

            PSSNode *op = getOperand(CInst->getArgOperand(idx));
            arg->addOperand(op);

            // shift in arguments
            assert(arg->successorsNum() <= 1);
            if (arg->successorsNum() == 1)
                arg = arg->getSingleSuccessor();
        }
    }

    // is the function variadic? arg now contains the last argument node,
    // which is the variadic one and idx should be index of the first
    // value passed as variadic. So go through the rest of callinst
    // arguments and if some of them is pointer, add it as an operand
    // to the phi node
    if (F->isVarArg()) {
        assert(arg);
        for (unsigned int i = idx; i < CInst->getNumArgOperands(); ++i) {
            const llvm::Value *llvmOp = CInst->getArgOperand(i);
            if (llvmOp->getType()->isPointerTy()) {
                PSSNode *op = getOperand(llvmOp);
                arg->addOperand(op);
            }
        }
    }

    // handle value returned from the function if it is a pointer
    if (CInst->getType()->isPointerTy()) {
        // return node is like a PHI node
        for (PSSNode *r : subg.ret->getPredecessors())
            // we're interested only in the nodes that return some value
            // from subprocedure, not for all nodes that have no successor
            if (r->getType() == pss::RETURN)
                returnNode->addOperand(r);
    }

    return std::make_pair(callNode, returnNode);
}

std::pair<PSSNode *, PSSNode *>
LLVMPSSBuilder::createOrGetSubgraph(const llvm::CallInst *CInst,
                                    const llvm::Function *F)
{
    std::pair<PSSNode *, PSSNode *> cf = createCallToFunction(CInst, F);
    addNode(CInst, cf.first);

    // NOTE: we do not add return node into nodes_map, since this
    // is artificial node and does not correspond to any real node

    return cf;
}

std::pair<PSSNode *, PSSNode *>
LLVMPSSBuilder::createUnknownCall(const llvm::CallInst *CInst)
{
    assert(CInst->getType()->isPointerTy());
    PSSNode *call = new PSSNode(pss::CALL, nullptr);

    call->setPairedNode(call);

    // the only thing that the node will point at
    call->addPointsTo(PointerUnknown);

    addNode(CInst, call);

    return std::make_pair(call, call);
}

PSSNode *LLVMPSSBuilder::createMemTransfer(const llvm::IntrinsicInst *I)
{
    using namespace llvm;
    const Value *dest, *src, *lenVal;

    switch (I->getIntrinsicID()) {
        case Intrinsic::memmove:
        case Intrinsic::memcpy:
            dest = I->getOperand(0);
            src = I->getOperand(1);
            lenVal = I->getOperand(2);
            break;
        default:
            errs() << "ERR: unhandled mem transfer intrinsic" << *I << "\n";
            abort();
    }

    PSSNode *destNode = getOperand(dest);
    PSSNode *srcNode = getOperand(src);
    /* FIXME: compute correct value instead of UNKNOWN_OFFSET */
    PSSNode *node = new PSSNode(MEMCPY, srcNode, destNode,
                                UNKNOWN_OFFSET, UNKNOWN_OFFSET);

    addNode(I, node);
    return node;
}

std::pair<PSSNode *, PSSNode *>
LLVMPSSBuilder::createVarArg(const llvm::IntrinsicInst *Inst)
{
    // just store all the pointers from vararg argument
    // to the memory given in vastart() on UNKNOWN_OFFSET.
    // It is the easiest thing we can do without any further
    // analysis

    // first we need to get the vararg argument phi
    const llvm::Function *F = Inst->getParent()->getParent();
    Subgraph& subg = subgraphs_map[F];
    PSSNode *arg = subg.args.second;
    assert(F->isVarArg() && "vastart in non-variadic function");
    assert(arg && "Don't have variadic argument in variadic function");

    // vastart will be node that will keep the memory
    // with pointers, its argument is the alloca, that
    // alloca will keep pointer to vastart
    PSSNode *vastart = new PSSNode(pss::ALLOC);

    // vastart has only one operand which is the struct
    // it uses for storing the va arguments. Strip it so that we'll
    // get the underlying alloca inst
    PSSNode *op = getOperand(Inst->getOperand(0)->stripInBoundsOffsets());
    assert(op->getType() == pss::ALLOC
           && "Argument of vastart is not an alloca");
    // get node with the same pointer, but with UNKNOWN_OFFSET
    // FIXME: we're leaking it
    // make the memory in alloca point to our memory in vastart
    PSSNode *ptr = new PSSNode(pss::CONSTANT, op, UNKNOWN_OFFSET);
    PSSNode *S1 = new PSSNode(pss::STORE, vastart, ptr);
    // and also make vastart point to the vararg args
    PSSNode *S2 = new PSSNode(pss::STORE, arg, vastart);

    addNode(Inst, vastart);

    vastart->addSuccessor(S1);
    S1->addSuccessor(S2);

    return std::make_pair(vastart, S2);
}

std::pair<PSSNode *, PSSNode *>
LLVMPSSBuilder::createIntrinsic(const llvm::Instruction *Inst)
{
    using namespace llvm;

    const IntrinsicInst *I = cast<IntrinsicInst>(Inst);
    if (isa<MemTransferInst>(I)) {
        PSSNode *n = createMemTransfer(I);
        return std::make_pair(n, n);
    } else if (I->getIntrinsicID() == Intrinsic::vastart) {
        return createVarArg(I);
    } else if (I->getIntrinsicID() == Intrinsic::stacksave) {
        errs() << "WARNING: Saving stack may yield unsound results!: "
               << *Inst << "\n";
        PSSNode *n = createAlloc(Inst);
        return std::make_pair(n, n);
    } else if (I->getIntrinsicID() == Intrinsic::stackrestore) {
        PSSNode *n = createLoad(Inst);
        return std::make_pair(n, n);
    } else
        assert(0 && "Unhandled intrinsic");
}

// create subgraph or add edges to already existing subgraph,
// return the CALL node (the first) and the RETURN node (the second),
// so that we can connect them into the PSS
std::pair<PSSNode *, PSSNode *>
LLVMPSSBuilder::createCall(const llvm::Instruction *Inst)
{
    using namespace llvm;
    const CallInst *CInst = cast<CallInst>(Inst);
    const Value *calledVal = CInst->getCalledValue()->stripPointerCasts();

    const Function *func = dyn_cast<Function>(calledVal);
    if (func) {
        /// memory allocation (malloc, calloc, etc.)
        int type;
        if ((type = getMemAllocationFunc(func))) {
            // NOTE: must be before func->size() == 0 condition,
            // since malloc and similar are undefined too
            return createDynamicMemAlloc(CInst, type);
        } else if (func->isIntrinsic()) {
            return createIntrinsic(Inst);
        } else if (func->size() == 0) {
             return createUnknownCall(CInst);
        } else {
            return createOrGetSubgraph(CInst, func);
        }
    } else {
        // function pointer call
        PSSNode *op = getOperand(calledVal);
        PSSNode *call_funcptr = new PSSNode(pss::CALL_FUNCPTR, op);
        PSSNode *ret_call = new PSSNode(RETURN, nullptr);

        ret_call->setPairedNode(call_funcptr);
        call_funcptr->setPairedNode(ret_call);

        call_funcptr->addSuccessor(ret_call);
        addNode(CInst, call_funcptr);

        return std::make_pair(call_funcptr, ret_call);
    }
}

PSSNode *LLVMPSSBuilder::createAlloc(const llvm::Instruction *Inst)
{
    PSSNode *node = new PSSNode(pss::ALLOC);
    addNode(Inst, node);

    const llvm::AllocaInst *AI = llvm::dyn_cast<llvm::AllocaInst>(Inst);
    if (AI) {
        uint64_t size = getAllocatedSize(AI->getAllocatedType(), DL);
        node->setSize(size);
    }

    assert(node);
    return node;
}

PSSNode *LLVMPSSBuilder::createStore(const llvm::Instruction *Inst)
{
    const llvm::Value *valOp = Inst->getOperand(0);

    PSSNode *op1 = getOperand(valOp);
    PSSNode *op2 = getOperand(Inst->getOperand(1));

    PSSNode *node = new PSSNode(pss::STORE, op1, op2);
    addNode(Inst, node);

    assert(node);
    return node;
}

PSSNode *LLVMPSSBuilder::createLoad(const llvm::Instruction *Inst)
{
    const llvm::Value *op = Inst->getOperand(0);

    PSSNode *op1 = getOperand(op);
    PSSNode *node = new PSSNode(pss::LOAD, op1);

    addNode(Inst, node);

    assert(node);
    return node;
}

PSSNode *LLVMPSSBuilder::createGEP(const llvm::Instruction *Inst)
{
    using namespace llvm;

    const GetElementPtrInst *GEP = cast<GetElementPtrInst>(Inst);
    const Value *ptrOp = GEP->getPointerOperand();
    unsigned bitwidth = getPointerBitwidth(DL, ptrOp);
    APInt offset(bitwidth, 0);

    PSSNode *node = nullptr;
    PSSNode *op = getOperand(ptrOp);

    if (GEP->accumulateConstantOffset(*DL, offset)) {
        if (offset.isIntN(bitwidth))
            node = new PSSNode(pss::GEP, op, offset.getZExtValue());
        else
            errs() << "WARN: GEP offset greater than " << bitwidth << "-bit";
            // fall-through to UNKNOWN_OFFSET in this case
    }

    if (!node)
        node = new PSSNode(pss::GEP, op, UNKNOWN_OFFSET);

    addNode(Inst, node);

    assert(node);
    return node;
}

PSSNode *LLVMPSSBuilder::createSelect(const llvm::Instruction *Inst)
{
    // the value needs to be a pointer - we call this function only under
    // this condition
    assert(Inst->getType()->isPointerTy() && "BUG: This select is not a pointer");

    // select <cond> <op1> <op2>
    PSSNode *op1 = getOperand(Inst->getOperand(1));
    PSSNode *op2 = getOperand(Inst->getOperand(2));

    // select works as a PHI in points-to analysis
    PSSNode *node = new PSSNode(pss::PHI, op1, op2, nullptr);
    addNode(Inst, node);

    assert(node);
    return node;
}

PSSNode *LLVMPSSBuilder::createPHI(const llvm::Instruction *Inst)
{
    // we need a pointer
    assert(Inst->getType()->isPointerTy() && "BUG: This PHI is not a pointer");

    PSSNode *node = new PSSNode(pss::PHI, nullptr);
    addNode(Inst, node);

    // NOTE: we didn't add operands to PHI node here, but after building
    // the whole function, because some blocks may not have been built
    // when we were creating the phi node

    assert(node);
    return node;
}

void LLVMPSSBuilder::addPHIOperands(PSSNode *node, const llvm::PHINode *PHI)
{
    assert(PHI->getType()->isPointerTy() && "BUG: This PHI is not a pointer");

    for (int i = 0, e = PHI->getNumIncomingValues(); i < e; ++i) {
        PSSNode *op = getOperand(PHI->getIncomingValue(i));
        node->addOperand(op);
    }
}

void LLVMPSSBuilder::addPHIOperands(const llvm::Function &F)
{
    for (const llvm::BasicBlock& B : F) {
        for (const llvm::Instruction& I : B) {
            if (!I.getType()->isPointerTy())
                continue;

            const llvm::PHINode *PHI = llvm::dyn_cast<llvm::PHINode>(&I);
            if (PHI)
                addPHIOperands(getNode(PHI), PHI);
        }
    }
}

PSSNode *LLVMPSSBuilder::createCast(const llvm::Instruction *Inst)
{
    const llvm::Value *op = Inst->getOperand(0);
    PSSNode *op1 = getOperand(op);
    PSSNode *node = new PSSNode(pss::CAST, op1);

    addNode(Inst, node);

    assert(node);
    return node;
}

// ptrToInt work just as a bitcast
PSSNode *LLVMPSSBuilder::createPtrToInt(const llvm::Instruction *Inst)
{
    const llvm::Value *op = Inst->getOperand(0);

    PSSNode *op1 = getOperand(op);
    PSSNode *node = new PSSNode(pss::CAST, op1);

    addNode(Inst, node);

    assert(node);
    return node;
}

PSSNode *LLVMPSSBuilder::createIntToPtr(const llvm::Instruction *Inst)
{
    const llvm::Value *op = Inst->getOperand(0);
    PSSNode *op1 = nullptr;

    if (llvm::isa<llvm::Constant>(op))
        llvm::errs() << "PTA warning: IntToPtr with constant: " << *Inst << "\n";
    else
        op1 = getOperand(op);

    PSSNode *node = new PSSNode(pss::CAST, op1);

    addNode(Inst, node);

    assert(node);
    return node;
}

PSSNode *LLVMPSSBuilder::createReturn(const llvm::Instruction *Inst)
{
    PSSNode *op1 = nullptr;
    // is nullptr if this is 'ret void'
    llvm::Value *retVal = llvm::cast<llvm::ReturnInst>(Inst)->getReturnValue();

    // we create even void and non-pointer return nodes,
    // since these modify CFG (they won't bear any
    // points-to information though)
    // XXX is that needed?

    if (retVal && retVal->getType()->isPointerTy())
        op1 = getOperand(retVal);

    assert((op1 || !retVal || !retVal->getType()->isPointerTy())
           && "Don't have operand for ReturnInst with pointer");

    PSSNode *node = new PSSNode(pss::RETURN, op1, nullptr);
    addNode(Inst, node);

    return node;
}

static bool isRelevantCall(const llvm::Instruction *Inst)
{
    using namespace llvm;

    // we don't care about debugging stuff
    if (isa<DbgValueInst>(Inst))
        return false;

    const CallInst *CInst = cast<CallInst>(Inst);
    const Value *calledVal = CInst->getCalledValue()->stripPointerCasts();
    const Function *func = dyn_cast<Function>(calledVal);

    if (!func)
        // function pointer call - we need that in PSS
        return true;

    if (func->size() == 0) {
        if (getMemAllocationFunc(func))
            // we need memory allocations
            return true;

        if (func->isIntrinsic()) {
            switch (func->getIntrinsicID()) {
                case Intrinsic::memmove:
                case Intrinsic::memcpy:
                case Intrinsic::memset:
                case Intrinsic::vastart:
                case Intrinsic::stacksave:
                case Intrinsic::stackrestore:
                    return true;
                default:
                    return false;
            }
        }

        // returns pointer? We want that too - this is gonna be
        // an unknown pointer
        if (Inst->getType()->isPointerTy())
            return true;

        // XXX: what if undefined function takes as argument pointer
        // to memory with pointers? In that case to be really sound
        // we should make those pointers unknown. Another case is
        // what if the function returns a structure (is it possible in LLVM?)
        // It can return a structure containing a pointer - thus we should
        // make this pointer unknown

        // here we have: undefined function not returning a pointer
        // and not memory allocation: we don't need that
        return false;
    } else
        // we want defined function, since those can contain
        // pointer's manipulation and modify CFG
        return true;

    assert(0 && "We should not reach this");
}

// return first and last nodes of the block
std::pair<PSSNode *, PSSNode *>
LLVMPSSBuilder::buildPSSBlock(const llvm::BasicBlock& block)
{
    using namespace llvm;

    std::pair<PSSNode *, PSSNode *> ret(nullptr, nullptr);
    PSSNode *prev_node;
    PSSNode *node = nullptr;

    for (const Instruction& Inst : block) {
        prev_node = node;

        switch(Inst.getOpcode()) {
            case Instruction::Alloca:
                node = createAlloc(&Inst);
                break;
            case Instruction::Store:
                // create only nodes that store pointer to another
                // pointer. We don't care about stores of non-pointers.
                // The only exception are inttoptr nodes. We can recognize
                // them so that these are created in nodes_map[].
                // They are not of pointer types but getNode() returns them
                if (Inst.getOperand(0)->getType()->isPointerTy()
                    || getNode(Inst.getOperand(0)))
                    node = createStore(&Inst);
                break;
            case Instruction::Load:
                if (Inst.getType()->isPointerTy()
                    || getNode(Inst.getOperand(0)))
                    node = createLoad(&Inst);
                break;
            case Instruction::GetElementPtr:
                node = createGEP(&Inst);
                break;
            case Instruction::Select:
                if (Inst.getType()->isPointerTy())
                    node = createSelect(&Inst);
                break;
            case Instruction::PHI:
                if (Inst.getType()->isPointerTy())
                    node = createPHI(&Inst);
                break;
            case Instruction::BitCast:
                node = createCast(&Inst);
                break;
            case Instruction::PtrToInt:
                node = createPtrToInt(&Inst);
                break;
            case Instruction::IntToPtr:
                node = createIntToPtr(&Inst);
                break;
            case Instruction::Ret:
                    node = createReturn(&Inst);
                break;
            case Instruction::Call:
                if (!isRelevantCall(&Inst))
                    break;

                std::pair<PSSNode *, PSSNode *> subg = createCall(&Inst);
                if (prev_node)
                    prev_node->addSuccessor(subg.first);
                else
                    // graphs starts with function call?
                    ret.first = subg.first;

                // new nodes will connect to the return node
                node = prev_node = subg.second;

                break;
        }

        // first instruction
        if (node && !prev_node)
            ret.first = node;

        if (prev_node && prev_node != node)
            prev_node->addSuccessor(node);
    }

    // last node
    ret.second = node;

    return ret;
}

static size_t blockAddSuccessors(std::map<const llvm::BasicBlock *,
                                          std::pair<PSSNode *, PSSNode *>>& built_blocks,
                                 std::set<const llvm::BasicBlock *>& found_blocks,
                                 std::pair<PSSNode *, PSSNode *>& pssn,
                                 const llvm::BasicBlock& block)
{
    size_t num = 0;

    for (llvm::succ_const_iterator
         S = llvm::succ_begin(&block), SE = llvm::succ_end(&block); S != SE; ++S) {

         // we already processed this block? Then don't try to add the edges again
         if (!found_blocks.insert(*S).second)
            continue;

        std::pair<PSSNode *, PSSNode *>& succ = built_blocks[*S];
        assert((succ.first && succ.second) || (!succ.first && !succ.second));
        if (!succ.first) {
            // if we don't have this block built (there was no points-to
            // relevant instruction), we must pretend to be there for
            // control flow information. Thus instead of adding it as
            // successor, add its successors as successors
            num += blockAddSuccessors(built_blocks, found_blocks, pssn, *(*S));
        } else {
            // add successor to the last nodes
            pssn.second->addSuccessor(succ.first);
            ++num;
        }
    }

    return num;
}

std::pair<PSSNode *, PSSNode *>
LLVMPSSBuilder::buildArguments(const llvm::Function& F)
{
    // create PHI nodes for arguments of the function. These will be
    // successors of call-node
    std::pair<PSSNode *, PSSNode *> ret;
    int idx = 0;
    PSSNode *prev, *arg = nullptr;

    for (auto A = F.arg_begin(), E = F.arg_end(); A != E; ++A, ++idx) {
        if (A->getType()->isPointerTy()) {
            prev = arg;

            arg = new PSSNode(pss::PHI, nullptr);
            addNode(&*A, arg);

            if (prev)
                prev->addSuccessor(arg);
            else
                ret.first = arg;

        }
    }

    // if the function has variable arguments,
    // then create the node for it and make it the last node
    if (F.isVarArg()) {
        ret.second = new PSSNode(pss::PHI, nullptr);
        if (arg)
            arg->addSuccessor(ret.second);
        else
            // we don't have any other argument than '...',
            // so this is first and last arg
            ret.first = ret.second;
    } else
        ret.second = arg;

    assert((ret.first && ret.second) || (!ret.first && !ret.second));

    return ret;
}

// build pointer state subgraph for given graph
// \return   root node of the graph
PSSNode *LLVMPSSBuilder::buildLLVMPSS(const llvm::Function& F)
{
    PSSNode *lastNode = nullptr;

    // create root and (unified) return nodes of this subgraph. These are
    // just for our convenience when building the graph, they can be
    // optimized away later since they are noops
    // XXX: do we need entry type?
    PSSNode *root = new PSSNode(pss::ENTRY);
    PSSNode *ret = new PSSNode(pss::NOOP);


    // now build the arguments of the function - if it has any
    std::pair<PSSNode *, PSSNode *> args = buildArguments(F);

    // add record to built graphs here, so that subsequent call of this function
    // from buildPSSBlock won't get stuck in infinite recursive call when
    // this function is recursive
    subgraphs_map[&F] = Subgraph(root, ret, args);

    // make arguments the entry block of the subgraphs (if there
    // are any arguments)
    if (args.first) {
        root->addSuccessor(args.first);
        lastNode = args.second;
    } else
        lastNode = root;

    assert(lastNode);

    PSSNode *first = nullptr;
    for (const llvm::BasicBlock& block : F) {
        std::pair<PSSNode *, PSSNode *> nds = buildPSSBlock(block);

        if (!first) {
            // first block was not created at all? (it has not
            // pointer relevant instructions) - in that case
            // fake that the first block is the root itself
            if (!nds.first) {
                nds.first = nds.second = root;
                first = root;
            } else {
                first = nds.first;

                // add correct successors. If we have arguments,
                // then connect the first block after arguments.
                // Otherwise connect them after the root node
                lastNode->addSuccessor(first);
            }
        }

        built_blocks[&block] = nds;
    }

    std::vector<PSSNode *> rets;
    for (const llvm::BasicBlock& block : F) {
        std::pair<PSSNode *, PSSNode *>& pssn = built_blocks[&block];
        // if the block do not contain any points-to relevant instruction,
        // we returned (nullptr, nullptr)
        // FIXME: do not store such blocks at all
        assert((pssn.first && pssn.second) || (!pssn.first && !pssn.second));
        if (!pssn.first)
            continue;

        // add successors to this block (skipping the empty blocks).
        // To avoid infinite loops we use found_blocks container that will
        // server as a mark in BFS/DFS - the program should not contain
        // so many blocks that this could have some big overhead. If proven
        // otherwise later, we'll change this.
        std::set<const llvm::BasicBlock *> found_blocks;
        size_t succ_num = blockAddSuccessors(built_blocks, found_blocks, pssn, block);

        // if we have not added any successor, then the last node
        // of this block is a return node
        if (succ_num == 0)
            rets.push_back(pssn.second);
    }

    // add successors edges from every real return to our artificial ret node
    assert(!rets.empty() && "BUG: Did not find any return node in function");
    for (PSSNode *r : rets)
        r->addSuccessor(ret);

    // add arguments to PHI nodes. We need to do that after the graph is
    // entirely built, because during building the arguments may not
    // be built yet
    addPHIOperands(F);

    return root;
}

PSSNode *LLVMPSSBuilder::buildLLVMPSS()
{
    // get entry function
    llvm::Function *F = M->getFunction("main");
    if (!F) {
        llvm::errs() << "Need main function in module\n";
        abort();
    }

    // first we must build globals, because nodes can use them as operands
    std::pair<PSSNode *, PSSNode *> glob = buildGlobals();

    // now we can build rest of the graph
    PSSNode *root = buildLLVMPSS(*F);

    // do we have any globals at all? If so, insert them at the begining of the graph
    // FIXME: we do not need to process them later, should we do it somehow differently?
    // something like 'static nodes' in PSS...
    if (glob.first) {
        assert(glob.second && "Have the start but not the end");

        // this is a sequence of global nodes, make it the root of the graph
        glob.second->addSuccessor(root);
        root = glob.first;
    }

    return root;
}


PSSNode *
LLVMPSSBuilder::handleGlobalVariableInitializer(const llvm::Constant *C,
                                                PSSNode *node)
{
    using namespace llvm;
    PSSNode *last = node;

    // if the global is zero initialized, just set the zeroInitialized flag
    if (isa<ConstantPointerNull>(C)
        || isa<ConstantAggregateZero>(C)) {
        node->setZeroInitialized();
    } else if (C->getType()->isAggregateType()) {
        uint64_t off = 0;
        for (auto I = C->op_begin(), E = C->op_end(); I != E; ++I) {
            const Value *val = *I;
            Type *Ty = val->getType();

            if (Ty->isPointerTy()) {
                PSSNode *op = getOperand(val);
                PSSNode *target = new PSSNode(CONSTANT, node, off);
                // FIXME: we're leaking the target
                // NOTE: mabe we could do something like
                // CONSTANT_STORE that would take Pointer instead of node??
                // PSSNode(CONSTANT_STORE, op, Pointer(node, off)) or
                // PSSNode(COPY, op, Pointer(node, off))??
                PSSNode *store = new PSSNode(STORE, op, target);
                store->insertAfter(last);
                last = store;
            }

            off += DL->getTypeAllocSize(Ty);
        }
    } else if (isa<ConstantExpr>(C) || isa<Function>(C)) {
       if (C->getType()->isPointerTy()) {
           PSSNode *value = getOperand(C);
           assert(value->pointsTo.size() == 1 && "BUG: We should have constant");
           // FIXME: we're leaking the target
           PSSNode *store = new PSSNode(STORE, value, node);
           store->insertAfter(last);
           last = store;
       }
    } else if (!isa<ConstantInt>(C)) {
        llvm::errs() << *C << "\n";
        llvm::errs() << "ERROR: ^^^ global variable initializer not handled\n";
    }

    return last;
}

std::pair<PSSNode *, PSSNode *> LLVMPSSBuilder::buildGlobals()
{
    PSSNode *cur = nullptr, *prev, *first = nullptr;
    // create PSS nodes
    for (auto I = M->global_begin(), E = M->global_end(); I != E; ++I) {
        prev = cur;

        // every global node is like memory allocation
        cur = new PSSNode(pss::ALLOC);
        addNode(&*I, cur);

        if (prev)
            prev->addSuccessor(cur);
        else
            first = cur;
    }

    // only now handle the initializers - we need to have then
    // built, because they can point to each other
    for (auto I = M->global_begin(), E = M->global_end(); I != E; ++I) {
        // handle globals initialization
        const llvm::GlobalVariable *GV
                            = llvm::dyn_cast<llvm::GlobalVariable>(&*I);
        if (GV && GV->hasInitializer() && !GV->isExternallyInitialized()) {
            const llvm::Constant *C = GV->getInitializer();
            PSSNode *node = nodes_map[&*I];
            assert(node && "BUG: Do not have global variable");
            cur = handleGlobalVariableInitializer(C, node);
        }
    }

    assert((!first && !cur) || (first && cur));
    return std::pair<PSSNode *, PSSNode *>(first, cur);
}

} // namespace pss
} // namespace analysis
} // namespace dg