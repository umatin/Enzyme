/*
 * TypeAnalysis.cpp - Type Analysis Detection Utilities
 *
 * Copyright (C) 2019 William S. Moses (enzyme@wsmoses.com) - All Rights Reserved
 *
 * For commercial use of this code please contact the author(s) above.
 *
 * For research use of the code please use the following citation.
 *
 * \misc{mosesenzyme,
    author = {William S. Moses, Tim Kaler},
    title = {Enzyme: LLVM Automatic Differentiation},
    year = {2019},
    howpublished = {\url{https://github.com/wsmoses/Enzyme/}},
    note = {commit xxxxxxx}
 */


#include <cstdint>
#include <deque>

#include <llvm/Config/llvm-config.h>

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

#include "llvm/IR/InstIterator.h"

#include "llvm/Support/raw_ostream.h"

#include "llvm/IR/InlineAsm.h"

#include "TypeAnalysis.h"
#include "Utils.h"

#include "TBAA.h"


//TODO keep type information that is striated
// e.g. if you have an i8* [0:Int, 8:Int] => i64* [0:Int, 1:Int]
// After a depth len into the index tree, prune any lookups that are not {0} or {-1}
// Todo handle {double}** to double** where there is a 0 removed
ValueData ValueData::KeepForCast(const llvm::DataLayout& dl, llvm::Type* from, llvm::Type* to) const {

    ValueData vd;

    for(auto &pair : mapping) {

        ValueData vd2;

        //llvm::errs() << " considering casting from " << *from << " to " << *to << " fromidx: " << to_string(pair.first) << " dt:" << pair.second.str() << " fromsize: " << fromsize << " tosize: " << tosize << "\n";

        if (pair.first.size() == 0) {
            vd2.insert(pair.first, pair.second);
            goto add;
        }
        {
        uint64_t fromsize = dl.getTypeSizeInBits(from) / 8;
        assert(fromsize > 0);
        uint64_t tosize = dl.getTypeSizeInBits(to) / 8;
        assert(tosize > 0);

        // If the sizes are the same, whatever the original one is okay [ since tomemory[ i*sizeof(from) ] indeed the start of an object of type to since tomemory is "aligned" to type to
        if (fromsize == tosize) {
            vd2.insert(pair.first, pair.second);
            goto add;
        }

        // If the offset doesn't leak into a later element, we're fine to include
        if (pair.first[0] != -1 && pair.first[0] < tosize) {
            vd2.insert(pair.first, pair.second);
            goto add;
        }

        if (pair.first[0] != -1) {
            vd.insert(pair.first, pair.second);
            goto add;
        } else {
            //pair.first[0] == -1

            if (fromsize < tosize) {
                if (tosize % fromsize == 0) {
                    //TODO should really be at each offset do a -1
                    vd.insert(pair.first, pair.second);
                    goto add;
                } else {
                    auto tmp(pair.first);
                    tmp[0] = 0;
                    vd.insert(tmp, pair.second);
                    goto add;
                }
            } else {
                //fromsize > tosize
                // TODO should really insert all indices which are multiples of fromsize
                auto tmp(pair.first);
                tmp[0] = 0;
                vd.insert(tmp, pair.second);
                goto add;
            }
        }
        }

        continue;
        add:;
        //llvm::errs() << " casting from " << *from << " to " << *to << " fromidx: " << to_string(pair.first) << " toidx: " << to_string(pair.first) << " dt:" << pair.second.str() << "\n";
        vd |= vd2;
    }
    return vd;
}


cl::opt<bool> printtype(
            "enzyme_printtype", cl::init(false), cl::Hidden,
            cl::desc("Print type detection algorithm"));

DataType parseTBAA(Instruction* inst) {
    auto typeNameStringRef = getAccessNameTBAA(inst, {"long long", "long", "int", "bool", "any pointer", "vtable pointer", "float", "double"});
    if (typeNameStringRef == "long long" || typeNameStringRef == "long" || typeNameStringRef == "int" || typeNameStringRef == "bool") {// || typeNameStringRef == "omnipotent char") {
        if (printtype) {
            llvm::errs() << "known tbaa " << *inst << " " << typeNameStringRef << "\n";
        }
        return DataType(IntType::Integer);
    } else if (typeNameStringRef == "any pointer" || typeNameStringRef == "vtable pointer") {// || typeNameStringRef == "omnipotent char") {
        if (printtype) {
            llvm::errs() << "known tbaa " << *inst << " " << typeNameStringRef << "\n";
        }
        return DataType(IntType::Pointer);
    } else if (typeNameStringRef == "float") {
        if (printtype)
            llvm::errs() << "known tbaa " << *inst << " " << typeNameStringRef << "\n";
        return Type::getFloatTy(inst->getContext());
    } else if (typeNameStringRef == "double") {
        if (printtype)
            llvm::errs() << "known tbaa " << *inst << " " << typeNameStringRef << "\n";
        return Type::getDoubleTy(inst->getContext());
    } else {
        return DataType(IntType::Unknown);
    }
}

TypeAnalyzer::TypeAnalyzer(Function* function, const NewFnTypeInfo& fn, TypeAnalysis& TA) : function(function), fntypeinfo(fn), interprocedural(TA), DT(*function) {
    for(auto &BB: *function) {
        for(auto &inst : BB) {
	        workList.push_back(&inst);
        }
    }
    for(auto &BB: *function) {
        for(auto &inst : BB) {
            for(auto& op : inst.operands()) {
                addToWorkList(op);
            }
        }
    }
}

ValueData TypeAnalyzer::getAnalysis(Value* val) {
	if (val->getType()->isIntegerTy() && cast<IntegerType>(val->getType())->getBitWidth() == 1) return ValueData(DataType(IntType::Integer));
    if (isa<ConstantData>(val)) {
		if (auto ci = dyn_cast<ConstantInt>(val)) {
			if (ci->getLimitedValue() >=1 && ci->getLimitedValue() <= 4096) {
				return ValueData(DataType(IntType::Integer));
			}
            if (ci->getType()->getBitWidth() == 8 && ci->getLimitedValue() == 0) {
				return ValueData(DataType(IntType::Integer));
            }
		}
		return ValueData(DataType(IntType::Anything));
	}

	Type* vt = val->getType();
	if (vt->isPointerTy()) {
		vt = cast<PointerType>(vt)->getElementType();
	}

    /*
	DataType dt = IntType::Unknown;

    if (vt->isPointerTy()) {
		dt = DataType(IntType::Pointer);
    }
    //if (vt->isFPOrFPVectorTy()) {
	//	dt = DataType(vt->getScalarType());
    //}
	if (dt.isKnown()) {
		if (val->getType()->isPointerTy()) {
			return ValueData(dt).Only({0});
		} else {
			return ValueData(dt);
		}
	}
    */

    if (isa<Argument>(val) || isa<Instruction>(val) || isa<ConstantExpr>(val)) return analysis[val];
    //TODO consider other things like globals perhaps?
    return ValueData();
}

void TypeAnalyzer::updateAnalysis(Value* val, IntType data, Value* origin) {
	updateAnalysis(val, ValueData(DataType(data)), origin);
}

void TypeAnalyzer::addToWorkList(Value* val) {
	if (!isa<Instruction>(val) && !isa<Argument>(val) && !isa<ConstantExpr>(val)) return;
    //llvm::errs() << " - adding to work list: " << *val << "\n";
    if (std::find(workList.begin(), workList.end(), val) != workList.end()) return;

    if (auto inst = dyn_cast<Instruction>(val)) {
        if (function != inst->getParent()->getParent()) {
                llvm::errs() << "function: " << *function << "\n";
                llvm::errs() << "instf: " << *inst->getParent()->getParent() << "\n";
                llvm::errs() << "inst: " << *inst << "\n";
        }
        assert(function == inst->getParent()->getParent());
    }
    if (auto arg = dyn_cast<Argument>(val))
        assert(function == arg->getParent());

    //llvm::errs() << " - - true add : " << *val << "\n";
	workList.push_back(val);
}

void TypeAnalyzer::updateAnalysis(Value* val, ValueData data, Value* origin) {
    if (isa<ConstantData>(val) || isa<Function>(val)) {
        return;
    }

    if (printtype) {
		llvm::errs() << "updating analysis of val: " << *val << " current: " << analysis[val].str() << " new " << data.str();
		if (origin) llvm::errs() << " from " << *origin;
		llvm::errs() << "\n";
	}

    if (auto inst = dyn_cast<Instruction>(val)) {
        if (function != inst->getParent()->getParent()) {
                llvm::errs() << "function: " << *function << "\n";
                llvm::errs() << "instf: " << *inst->getParent()->getParent() << "\n";
                llvm::errs() << "inst: " << *inst << "\n";
        }
        assert(function == inst->getParent()->getParent());
    }
    if (auto arg = dyn_cast<Argument>(val))
        assert(function == arg->getParent());

    if (isa<GetElementPtrInst>(val) && data[{}] == IntType::Integer) {
        llvm::errs () << "illegal gep update\n";
        assert(0 && "illegal gep update");
    }

    if (val->getType()->isPointerTy() && data[{}] == IntType::Integer) {
        llvm::errs () << "illegal gep update\n";
        assert(0 && "illegal gep update");
    }

    /*
    dump();
    if (origin)
    llvm::errs() << "origin: " << *origin << "\n";
    llvm::errs() << "val: " << *val << "\n";
    llvm::errs() << " + old: " << analysis[val].str() << "\n";
    llvm::errs() << " + tomerge: " << data.str() << "\n";
    */
    if (analysis[val] |= data) {
    	//Add val so it can explicitly propagate this new info, if able to
    	if (val != origin)
    		addToWorkList(val);

    	//Add users and operands of the value so they can update from the new operand/use
        for (User* use : val->users()) {
            if (use != origin) {

                if (auto inst = dyn_cast<Instruction>(use)) {
                    if (function != inst->getParent()->getParent()) {
                        continue;
                    }
                }

                addToWorkList(use);
            }
        }

        if (User* me = dyn_cast<User>(val)) {
            for (Value* op : me->operands()) {
                if (op != origin) {
                    addToWorkList(op);
                }
            }
        }
    }
}

void TypeAnalyzer::prepareArgs() {
    for(auto &pair: fntypeinfo.first) {
        assert(pair.first->getParent() == function);
        updateAnalysis(pair.first, pair.second, nullptr);
    }

    for(auto &arg : function->args()) {
    	//Get type and other information about argument
        updateAnalysis(&arg, getAnalysis(&arg), &arg);
    }

    //Propagate return value type information
    for(auto &BB: *function) {
        for(auto &inst : BB) {
            if (auto ri = dyn_cast<ReturnInst>(&inst)) {
                if (auto rv = ri->getReturnValue()) {
                    updateAnalysis(rv, fntypeinfo.second, nullptr);
                }
            }
        }
    }
}

void TypeAnalyzer::considerTBAA() {
    for(auto &BB: *function) {
        for(auto &inst : BB) {
            auto dt = parseTBAA(&inst);
            if (!dt.isKnown()) continue;

            ValueData vdptr = ValueData(dt).Only({0});
            vdptr |= ValueData(IntType::Pointer);

            if (auto call = dyn_cast<CallInst>(&inst)) {
                if (call->getCalledFunction() && (call->getCalledFunction()->getIntrinsicID() == Intrinsic::memcpy || call->getCalledFunction()->getIntrinsicID() == Intrinsic::memmove)) {
                    if (auto ci = fntypeinfo.isConstantInt(call->getOperand(2))) {
                        for(int i=0; i<(int)ci->getLimitedValue(); i++) {
                            ValueData iptr = ValueData(dt).Only({i});
                            iptr |= ValueData(IntType::Pointer);

                            updateAnalysis(call->getOperand(0), iptr, call);
                            updateAnalysis(call->getOperand(1), iptr, call);
                        }
                        continue;
                    }
                    updateAnalysis(call->getOperand(0), vdptr, call);
                    updateAnalysis(call->getOperand(1), vdptr, call);
                } else if (call->getType()->isPointerTy()) {
                    updateAnalysis(call, ValueData(dt).Only({-1}), call);
                } else {
                    assert(0 && "unknown tbaa call instruction user");
                }
            } else if (auto si = dyn_cast<StoreInst>(&inst)) {
                //TODO why?
                if (dt == IntType::Pointer) continue;
                updateAnalysis(si->getPointerOperand(), vdptr, si);
                updateAnalysis(si->getValueOperand(), ValueData(dt), si);
            } else if (auto li = dyn_cast<LoadInst>(&inst)) {
                updateAnalysis(li->getPointerOperand(), vdptr, li);
                updateAnalysis(li, ValueData(dt), li);
            } else {
                assert(0 && "unknown tbaa instruction user");
            }
        }
    }
}


std::set<int64_t> couldBeZero(Value* val, std::map<Value*, std::set<int64_t>>& intseen, const NewFnTypeInfo& info, DominatorTree& DT) {
    if (intseen.find(val) != intseen.end()) return intseen[val];
    //todo what to insert to intseen

    intseen[val] = {};

    if (auto ci = info.isConstantInt(val)) {
        return intseen[val] = { ci->getSExtValue() };
    }

    if (auto ci = dyn_cast<CastInst>(val)) {
        return intseen[val] = couldBeZero(ci->getOperand(0), intseen, info, DT);
    }

    if (auto pn = dyn_cast<PHINode>(val)) {

        for(unsigned i=0; i<pn->getNumIncomingValues(); i++) {
            auto a = pn->getIncomingValue(i);
            auto b = pn->getIncomingBlock(i);

            //do not consider loop incoming edges
            if (pn->getParent() == b || DT.dominates(pn, b)) {
                continue;
            }

            auto inset = couldBeZero(a, intseen, info, DT);
            //TODO this here is not fully justified yet
            for(auto pval : inset) {
                if (pval < 20 && pval > -20) {
                    intseen[val].insert(pval);
                }
            }

            // if we are an iteration variable, suppose that it could be zero in that range
            // TODO: could actually check the range intercepts 0
            if (auto bo = dyn_cast<BinaryOperator>(a)) {
                if (bo->getOperand(0) == pn || bo->getOperand(1) == pn) {
                    if (bo->getOpcode() == BinaryOperator::Add || bo->getOpcode() == BinaryOperator::Sub) {
                        intseen[val].insert(0);
                    }
                }
            }
        }
        return intseen[val];
    }

    if (auto bo = dyn_cast<BinaryOperator>(val)) {
        if (bo->getOpcode() == BinaryOperator::Mul) {
            auto inset0 = couldBeZero(bo->getOperand(0), intseen, info, DT);
            auto inset1 = couldBeZero(bo->getOperand(1), intseen, info, DT);

            if (auto ci = info.isConstantInt(bo->getOperand(0))) {
                for(auto pval : inset1) {
                    intseen[val].insert(ci->getSExtValue() * pval);
                }
            }
            if (auto ci = info.isConstantInt(bo->getOperand(1))) {
                for(auto pval : inset0) {
                    intseen[val].insert(pval * ci->getSExtValue());
                }
            }
            if (inset0.count(0) || inset1.count(0)) {
                intseen[val].insert(0);
            }
        }

        if (bo->getOpcode() == BinaryOperator::Add) {
            if (auto ci = info.isConstantInt(bo->getOperand(0))) {
                auto inset = couldBeZero(bo->getOperand(1), intseen, info, DT);
                for(auto pval : inset) {
                    intseen[val].insert(ci->getSExtValue() + pval);
                }
            }
            if (auto ci = info.isConstantInt(bo->getOperand(1))) {
                auto inset = couldBeZero(bo->getOperand(0), intseen, info, DT);
                for(auto pval : inset) {
                    intseen[val].insert(pval + ci->getSExtValue());
                }
            }
        }
        if (bo->getOpcode() == BinaryOperator::Sub) {
            if (auto ci = info.isConstantInt(bo->getOperand(0))) {
                auto inset = couldBeZero(bo->getOperand(1), intseen, info, DT);
                for(auto pval : inset) {
                    intseen[val].insert(ci->getSExtValue() - pval);
                }
            }
            if (auto ci = info.isConstantInt(bo->getOperand(1))) {
                auto inset = couldBeZero(bo->getOperand(0), intseen, info, DT);
                for(auto pval : inset) {
                    intseen[val].insert(pval - ci->getSExtValue());
                }
            }
        }

        if (bo->getOpcode() == BinaryOperator::Shl) {
            if (auto ci = info.isConstantInt(bo->getOperand(0))) {
                auto inset = couldBeZero(bo->getOperand(1), intseen, info, DT);
                for(auto pval : inset) {
                    intseen[val].insert(ci->getSExtValue() << pval);
                }
            }
            if (auto ci = info.isConstantInt(bo->getOperand(1))) {
                auto inset = couldBeZero(bo->getOperand(0), intseen, info, DT);
                for(auto pval : inset) {
                    intseen[val].insert(pval << ci->getSExtValue());
                }
            }
        }

        //TOOD note C++ doesnt guarantee behavior of >> being arithmetic or logical
        //     and should replace with llvm apint internal
        if (bo->getOpcode() == BinaryOperator::AShr || bo->getOpcode() == BinaryOperator::LShr) {
            if (auto ci = info.isConstantInt(bo->getOperand(0))) {
                auto inset = couldBeZero(bo->getOperand(1), intseen, info, DT);
                for(auto pval : inset) {
                    intseen[val].insert(ci->getSExtValue() >> pval);
                }
            }
            if (auto ci = info.isConstantInt(bo->getOperand(1))) {
                auto inset = couldBeZero(bo->getOperand(0), intseen, info, DT);
                for(auto pval : inset) {
                    intseen[val].insert(pval >> ci->getSExtValue());
                }
            }
        }

    }

    return intseen[val];
}

bool hasAnyUse(TypeAnalyzer& TAZ, Value* val, std::map<Value*, bool>& intseen, bool* sawReturn/*if sawReturn != nullptr, we can ignore uses of returninst, setting the bool to true if we see one*/) {
    if (intseen.find(val) != intseen.end()) return intseen[val];
    //todo what to insert to intseen

    bool unknownUse = false;
    intseen[val] = false;

    for(User* use: val->users()) {
        if (auto ci = dyn_cast<CastInst>(use)) {
            unknownUse |= hasAnyUse(TAZ, ci, intseen, sawReturn);
            continue;
        }

        if (auto pn = dyn_cast<PHINode>(use)) {
            unknownUse |= hasAnyUse(TAZ, pn, intseen, sawReturn);
            continue;
        }

        if (auto seli = dyn_cast<SelectInst>(use)) {
            unknownUse |= hasAnyUse(TAZ, seli, intseen, sawReturn);
            continue;
        }

        if (auto call = dyn_cast<CallInst>(use)) {
            if (Function* ci = call->getCalledFunction()) {
                //These function calls are known uses that do not potentially have an inactive use
                if (ci->getName() == "__cxa_guard_acquire" || ci->getName() == "__cxa_guard_release" || ci->getName() == "__cxa_guard_abort" || ci->getName() == "printf" || ci->getName() == "fprintf") {
                    continue;
                }

                //TODO recursive fns

                if (!ci->empty()) {
                    auto a = ci->arg_begin();

                    bool shouldHandleReturn=false;

                    for(size_t i=0; i<call->getNumArgOperands(); i++) {
                        if (call->getArgOperand(i) == val) {
                            if(hasAnyUse(TAZ, a, intseen, &shouldHandleReturn)) {
                                return intseen[val] = unknownUse = true;
                            }
                        }
                        a++;
                    }

                    if (shouldHandleReturn) {
                        if(hasAnyUse(TAZ, call, intseen, sawReturn)) {
                            return intseen[val] = unknownUse = true;
                        }
                    }
                    continue;
                }
            }
        }

        if (sawReturn && isa<ReturnInst>(use)) {
            *sawReturn = true;
            continue;
        }

        unknownUse = true;
        //llvm::errs() << "unknown use : " << *use << " of v: " << *v << "\n";
        continue;
    }

    return intseen[val] = unknownUse;
}

bool hasNonIntegralUse(TypeAnalyzer& TAZ, Value* val, std::map<Value*, bool>& intseen, bool* sawReturn/*if sawReturn != nullptr, we can ignore uses of returninst, setting the bool to true if we see one*/) {
    if (intseen.find(val) != intseen.end()) return intseen[val];
    //todo what to insert to intseen

    bool unknownUse = false;
    intseen[val] = false;

    for(User* use: val->users()) {
        if (auto ci = dyn_cast<CastInst>(use)) {

            if (isa<SIToFPInst>(use) || isa<UIToFPInst>(use)) {
                continue;
            }

            if (isa<FPToSIInst>(use) || isa<FPToUIInst>(use)) {
                continue;
            }

            if (ci->getDestTy()->isPointerTy()) {
                unknownUse = true;
                break;
            }

            unknownUse |= hasNonIntegralUse(TAZ, ci, intseen, sawReturn);
            continue;
        }

        if (auto bi = dyn_cast<BinaryOperator>(use)) {

            unknownUse |= hasNonIntegralUse(TAZ, bi, intseen, sawReturn);
            continue;
        }

        if (auto pn = dyn_cast<PHINode>(use)) {
            unknownUse |= hasNonIntegralUse(TAZ, pn, intseen, sawReturn);
            continue;
        }

        if (auto seli = dyn_cast<SelectInst>(use)) {
            unknownUse |= hasNonIntegralUse(TAZ, seli, intseen, sawReturn);
            continue;
        }

        if (auto gep = dyn_cast<GetElementPtrInst>(use)) {
            if (gep->getPointerOperand() == val) {
                unknownUse = true;
                break;
            }

            //this assumes that the original value doesnt propagate out through the pointer
            continue;
        }

        if (auto call = dyn_cast<CallInst>(use)) {
            if (Function* ci = call->getCalledFunction()) {
                //These function calls are known uses that do not potentially have an inactive use
                if (ci->getName() == "__cxa_guard_acquire" || ci->getName() == "__cxa_guard_release" || ci->getName() == "__cxa_guard_abort" || ci->getName() == "printf" || ci->getName() == "fprintf") {
                    continue;
                }

                //TODO recursive fns

                if (!ci->empty()) {
                    auto a = ci->arg_begin();

                    bool shouldHandleReturn=false;

                    for(size_t i=0; i<call->getNumArgOperands(); i++) {
                        if (call->getArgOperand(i) == val) {
                            if(hasNonIntegralUse(TAZ, a, intseen, &shouldHandleReturn)) {
                                return intseen[val] = unknownUse = true;
                            }
                        }
                        a++;
                    }

                    if (shouldHandleReturn) {
                        if(hasNonIntegralUse(TAZ, call, intseen, sawReturn)) {
                            return intseen[val] = unknownUse = true;
                        }
                    }
                    continue;
                }
            }
        }

        if (isa<AllocaInst>(use)) {
            continue;
        }

        if (isa<CmpInst>(use)) continue;
        if (isa<SwitchInst>(use)) continue;
        if (isa<BranchInst>(use)) continue;

        if (sawReturn && isa<ReturnInst>(use)) {
            *sawReturn = true;
            continue;
        }

        unknownUse = true;
        //llvm::errs() << "unknown use : " << *use << " of v: " << *v << "\n";
        continue;
    }

    return intseen[val] = unknownUse;
}

bool TypeAnalyzer::runUnusedChecks() {
    //NOTE explicitly NOT doing arguments here
    //  this is done to prevent information being propagated up via IPO that is incorrect (since there may be other uses in the caller)
    bool changed = false;
    std::vector<Value*> todo;

    std::map<Value*, bool> anyseen;
    std::map<Value*, bool> intseen;

    for(auto &BB: *function) {
        for(auto &inst : BB) {
            auto analysis = getAnalysis(&inst);
            if (analysis[{}] != IntType::Unknown) continue;

            if (!inst.getType()->isIntOrIntVectorTy()) continue;

            //This deals with integers representing floats or pointers with no use (and thus can be anything)
            {
                if(!hasAnyUse(*this, &inst, anyseen, nullptr)) {
                    updateAnalysis(&inst, IntType::Anything, &inst);
                    changed = true;
                }
            }

            //This deals with integers with no use
            {
                if(!hasNonIntegralUse(*this, &inst, intseen, nullptr)) {
                    updateAnalysis(&inst, IntType::Integer, &inst);
                    changed = true;
                }
            }
        }
    }

    return changed;
}

void TypeAnalyzer::run() {
	std::deque<CallInst*> pendingCalls;

	do {

    while (workList.size()) {
        auto todo = workList.front();
        workList.pop_front();
        if (auto ci = dyn_cast<CallInst>(todo)) {
        	pendingCalls.push_back(ci);
        	continue;
        }
        visitValue(*todo);
    }

    if (pendingCalls.size() > 0) {
    	auto todo = pendingCalls.front();
    	pendingCalls.pop_front();
    	visitValue(*todo);
    	continue;
    } else break;

	}while(1);

    runUnusedChecks();

    do {

    while (workList.size()) {
        auto todo = workList.front();
        workList.pop_front();
        if (auto ci = dyn_cast<CallInst>(todo)) {
            pendingCalls.push_back(ci);
            continue;
        }
        visitValue(*todo);
    }

    if (pendingCalls.size() > 0) {
        auto todo = pendingCalls.front();
        pendingCalls.pop_front();
        visitValue(*todo);
        continue;
    } else break;

    }while(1);
}

void TypeAnalyzer::visitValue(Value& val) {
    if (isa<ConstantData>(&val)) {
        return;
    }

    if (auto ce = dyn_cast<ConstantExpr>(&val)) {
        auto ae = ce->getAsInstruction();
        ae->insertBefore(function->getEntryBlock().getTerminator());
        analysis[ae] = getAnalysis(ce);
        visit(*ae);
        for(auto& a : workList) {
            if (a == ae) {
                a = ce;
            }
        }
        updateAnalysis(ce, analysis[ae], ce);
        analysis.erase(ae);
        ae->eraseFromParent();
        return;
    }

    if (!isa<Argument>(&val) && !isa<Instruction>(&val)) return;

    //TODO add no users integral here

    if (auto inst = dyn_cast<Instruction>(&val)) {
		visit(*inst);
	}
}

void TypeAnalyzer::visitAllocaInst(AllocaInst &I) {
    updateAnalysis(I.getArraySize(), IntType::Integer, &I);
    //todo consider users
}

void TypeAnalyzer::visitLoadInst(LoadInst &I) {
    auto ptr = getAnalysis(&I).Only({0});
    ptr |= ValueData(IntType::Pointer);
    updateAnalysis(I.getOperand(0), ptr, &I);
    updateAnalysis(&I, getAnalysis(I.getOperand(0)).Lookup({0}), &I);
}

void TypeAnalyzer::visitStoreInst(StoreInst &I) {
    auto purged = getAnalysis(I.getValueOperand()).PurgeAnything();
    auto ptr = ValueData(IntType::Pointer);

    auto storeSize = I.getParent()->getParent()->getParent()->getDataLayout().getTypeSizeInBits(I.getValueOperand()->getType()) / 8;
    storeSize = 1;
    for(unsigned i=0; i<storeSize; i++) {
        ptr |= purged.Only({(int)i});
    }

    //llvm::errs() << "considering si: " << I << "\n";
    //llvm::errs() << " prevanalysis: " << getAnalysis(I.getPointerOperand()).str() << "\n";
    //llvm::errs() << " new: " << ptr.str() << "\n";

    updateAnalysis(I.getPointerOperand(), ptr, &I);
    updateAnalysis(I.getValueOperand(), getAnalysis(I.getPointerOperand()).Lookup({0}), &I);
}

template<typename T>
std::set<std::vector<T>> getSet(const std::vector<std::set<T>> &todo, size_t idx) {
    std::set<std::vector<T>> out;
    if (idx == 0) {
        for(auto val : todo[0]) {
            out.insert({val});
        }
        return out;
    }

    auto old = getSet(todo, idx-1);
    for(const auto& oldv : old) {
        for(auto val : todo[idx]) {
            auto nex = oldv;
            nex.push_back(val);
            out.insert(nex);
        }
    }
    return out;
}

void TypeAnalyzer::visitGetElementPtrInst(GetElementPtrInst &gep) {


    auto pointerAnalysis = getAnalysis(gep.getPointerOperand());
    updateAnalysis(&gep, pointerAnalysis.KeepMinusOne(), &gep);

    std::vector<std::set<Value*>> idnext;

    std::map<Value*, std::set<int64_t>> intseen;

    // If we know that the pointer operand is indeed a pointer, then the indicies must be integers
    // Note that we can't do this if we don't know the pointer operand is a pointer since doing 1[pointer] is legal
    //  sadly this still may not work since (nullptr)[fn] => fn where fn is pointer and not int (whereas nullptr is a pointer)
    //  However if we are inbounds you are only allowed to have nullptr[0] or nullptr[nullptr], making this valid
    // TODO note that we don't force the inttype::pointer (commented below) assuming nullptr[nullptr] doesn't occur in practice
    //if (gep.isInBounds() && pointerAnalysis[{}] == IntType::Pointer) {
    if (gep.isInBounds()) {
        //llvm::errs() << "gep: " << gep << "\n";
        for(auto& ind : gep.indices()) {
           //llvm::errs() << " + ind: " << *ind << " - prev - " << getAnalysis(ind).str() << "\n";
            updateAnalysis(ind, IntType::Integer, &gep);
        }
    }



    for(auto& a : gep.indices()) {
        auto iset = couldBeZero(a, intseen, fntypeinfo, DT);
        std::set<Value*> vset;
        for(auto i : iset) {
            vset.insert(ConstantInt::get(a->getType(), i));
        }
        idnext.push_back(vset);
        if (idnext.back().size() == 0) return;
    }

    for (auto vec : getSet(idnext, idnext.size()-1)) {
        auto g2 = GetElementPtrInst::Create(nullptr, gep.getOperand(0), vec);
        #if LLVM_VERSION_MAJOR > 6
        APInt ai(function->getParent()->getDataLayout().getIndexSizeInBits(gep.getPointerAddressSpace()), 0);
        #else
        APInt ai(function->getParent()->getDataLayout().getPointerSize(gep.getPointerAddressSpace()) * 8, 0);
        #endif
        g2->accumulateConstantOffset(function->getParent()->getDataLayout(), ai);
        delete g2;//->eraseFromParent();

        int off = (int)ai.getLimitedValue();

        //TODO also allow negative offsets
        if (off < 0) continue;

        int maxSize = -1;
        if (cast<ConstantInt>(vec[0])->getLimitedValue() == 0) {
            maxSize = function->getParent()->getDataLayout().getTypeAllocSizeInBits(cast<PointerType>(gep.getType())->getElementType())/8;
        }

        /*
        if (gep.getName() == "arrayidx.i132") {
            llvm::errs() << "HERE: off:" << off << " maxSize: " << maxSize << " vec: [";
            for(auto v: vec) llvm::errs() << *v << ", ";
            llvm::errs() << "] " << "\n";
        }

        if (gep.getName() == "m_inputImpl.i") {
            dump();
            llvm::errs() << *gep.getParent()->getParent() << "\n";
            llvm::errs() << "GEP: " << gep << " - " << getAnalysis(&gep).str() << " - off=" << off << "\n";
            llvm::errs() << "  + pa: " << *gep.getPointerOperand() << " - " << pointerAnalysis.str() << "\n";
            llvm::errs() << "  + pa unmerge: " << pointerAnalysis.UnmergeIndices(off, maxSize).str() << "\n";
        }
        */

        updateAnalysis(&gep, pointerAnalysis.UnmergeIndices(off, maxSize), &gep);

        auto merged = getAnalysis(&gep).MergeIndices(off);

        //llvm::errs()  << " + prevanalysis: " << getAnalysis(gep.getPointerOperand()).str() << " merged: " << merged.str() << " g2:[";

        updateAnalysis(gep.getPointerOperand(), merged, &gep);
    }
}

void TypeAnalyzer::visitPHINode(PHINode& phi) {
    for(auto& op : phi.incoming_values()) {
        updateAnalysis(op, getAnalysis(&phi), &phi);
    }

    assert(phi.getNumIncomingValues() > 0);
	//TODO phi needs reconsidering here
    ValueData vd;
    bool set = false;

    auto consider = [&](ValueData&& newData) {
        if (set) {
            vd &= newData;
        } else {
            set = true;
            vd = newData;
        }
    };

    //llvm::errs() << "phi: " << phi << "\n";

    //TODO generalize this (and for recursive, etc)
    std::deque<Value*> vals;
    std::set<Value*> seen { &phi };
    for(auto& op : phi.incoming_values()) {
        vals.push_back(op);
    }

    while(vals.size()) {
        Value* todo = vals.front();
        vals.pop_front();

        if (seen.count(todo)) continue;
        seen.insert(todo);

        if (auto nphi = dyn_cast<PHINode>(todo)) {
            for(auto& op : nphi->incoming_values()) {
                vals.push_back(op);
            }
            continue;
        }

        //llvm::errs() << " + " << vd.str() << " ga: " << getAnalysis(op).str() << "\n";
        consider(getAnalysis(todo));
    }

    updateAnalysis(&phi, vd, &phi);
}

void TypeAnalyzer::visitTruncInst(TruncInst &I) {
	updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
	updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
}

void TypeAnalyzer::visitZExtInst(ZExtInst &I) {
	updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
	updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
}

void TypeAnalyzer::visitSExtInst(SExtInst &I) {
	updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
	updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
}

void TypeAnalyzer::visitAddrSpaceCastInst(AddrSpaceCastInst &I) {
	updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
	updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
}

void TypeAnalyzer::visitFPToUIInst(FPToUIInst &I) {
	updateAnalysis(&I, IntType::Integer, &I);
}

void TypeAnalyzer::visitFPToSIInst(FPToSIInst &I) {
	updateAnalysis(&I, IntType::Integer, &I);
}

void TypeAnalyzer::visitUIToFPInst(UIToFPInst &I) {
	updateAnalysis(I.getOperand(0), IntType::Integer, &I);
}

void TypeAnalyzer::visitSIToFPInst(SIToFPInst &I) {
	updateAnalysis(I.getOperand(0), IntType::Integer, &I);
}

void TypeAnalyzer::visitPtrToIntInst(PtrToIntInst &I) {
	updateAnalysis(&I, IntType::Pointer, &I);
}

void TypeAnalyzer::visitIntToPtrInst(IntToPtrInst &I) {
	updateAnalysis(I.getOperand(0), IntType::Pointer, &I);
}

void TypeAnalyzer::visitBitCastInst(BitCastInst &I) {
  if (I.getType()->isIntOrIntVectorTy() || I.getType()->isFPOrFPVectorTy()) {
	updateAnalysis(&I, getAnalysis(I.getOperand(0)), &I);
	updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
	return;
  }

  if (I.getType()->isPointerTy() && I.getOperand(0)->getType()->isPointerTy()) {
    Type* et1 = cast<PointerType>(I.getType())->getElementType();
    Type* et2 = cast<PointerType>(I.getOperand(0)->getType())->getElementType();

    //I.getParent()->getParent()->dump();
    //dump();
    //llvm::errs() << "I: " << I << "\n";
    //llvm::errs() << " + keep for cast: " << getAnalysis(I.getOperand(0)).KeepForCast(function->getParent()->getDataLayout(), et2, et1).str() << "\n";
	updateAnalysis(&I, getAnalysis(I.getOperand(0)).KeepForCast(function->getParent()->getDataLayout(), et2, et1), &I);
	updateAnalysis(I.getOperand(0), getAnalysis(&I).KeepForCast(function->getParent()->getDataLayout(), et1, et2), &I);
  }
}

void TypeAnalyzer::visitSelectInst(SelectInst &I) {
    //dump();
    //llvm::errs() << *I.getParent()->getParent() << "\n";
    //llvm::errs() << "SI: " << I << " analysis: " << getAnalysis(&I).str() << "\n";
    //llvm::errs() << " +  " << *I.getTrueValue() << " analysis: " << getAnalysis(I.getTrueValue()).str() << "\n";
    //llvm::errs() << " +  " << *I.getFalseValue() << " analysis: " << getAnalysis(I.getFalseValue()).str() << "\n";
    updateAnalysis(I.getTrueValue(), getAnalysis(&I), &I);
    updateAnalysis(I.getFalseValue(), getAnalysis(&I), &I);

    ValueData vd = getAnalysis(I.getTrueValue());
	vd &= getAnalysis(I.getFalseValue());

    updateAnalysis(&I, vd, &I);
}

void TypeAnalyzer::visitExtractElementInst(ExtractElementInst &I) {
	updateAnalysis(I.getIndexOperand(), IntType::Integer, &I);

	//int idx = -1;
    //if (auto ci = dyn_cast<ConstantInt>(I.getIndexOperand())) {
    // 	idx = (int)ci->getLimitedValue();
	//}

	//updateAnalysis(I.getVectorOperand(), getAnalysis(&I).Only({idx}), Direction::Both);
    //updateAnalysis(&I, getAnalysis(I.getVectorOperand()).Lookup({idx}), Direction::Both);
	updateAnalysis(I.getVectorOperand(), getAnalysis(&I), &I);
    updateAnalysis(&I, getAnalysis(I.getVectorOperand()), &I);
}

void TypeAnalyzer::visitInsertElementInst(InsertElementInst &I) {
	updateAnalysis(I.getOperand(2), IntType::Integer, &I);

	//int idx = -1;
	//if (auto ci = dyn_cast<ConstantInt>(I.getOperand(2))) {
    //	idx = (int)ci->getLimitedValue();
	//}

    //if we are inserting into undef/etc the anything should not be propagated
	auto res = getAnalysis(I.getOperand(0)).PurgeAnything();

	res |= getAnalysis(I.getOperand(1));
	//res |= getAnalysis(I.getOperand(1)).Only({idx});
	res |= getAnalysis(&I);

    updateAnalysis(I.getOperand(0), res, &I);
    updateAnalysis(&I, res, &I);
	updateAnalysis(I.getOperand(1), res, &I);
	//updateAnalysis(I.getOperand(1), res.Lookup({idx}), Direction::Both);
}

void TypeAnalyzer::visitShuffleVectorInst(ShuffleVectorInst &I) {
    updateAnalysis(I.getOperand(0), getAnalysis(&I), &I);
    updateAnalysis(I.getOperand(1), getAnalysis(&I), &I);

    ValueData vd = getAnalysis(I.getOperand(0));
	vd &= getAnalysis(I.getOperand(1));

    updateAnalysis(&I, vd, &I);
}

void TypeAnalyzer::visitExtractValueInst(ExtractValueInst &I) {
	//TODO aggregate flow

    if (auto call = dyn_cast<CallInst>(I.getOperand(0))) {
        if (auto iasm = dyn_cast<InlineAsm>(call->getCalledValue())) {
            if (iasm->getAsmString() == "cpuid") {
                updateAnalysis(&I, ValueData(IntType::Integer), &I);
            }
        }
    }
}

void TypeAnalyzer::visitInsertValueInst(InsertValueInst &I) {
	//TODO aggregate flow
}

void TypeAnalyzer::dump() {
    llvm::errs() << "<analysis>\n";
    for(auto& pair : analysis) {
        llvm::errs() << *pair.first << ": " << pair.second.str() << "\n";
    }
    llvm::errs() << "</analysis>\n";
}

void TypeAnalyzer::visitBinaryOperator(BinaryOperator &I) {
    if (I.getOpcode() == BinaryOperator::FAdd || I.getOpcode() == BinaryOperator::FSub ||
            I.getOpcode() == BinaryOperator::FMul || I.getOpcode() == BinaryOperator::FDiv ||
            I.getOpcode() == BinaryOperator::FRem) {
        auto ty = I.getType()->getScalarType();
        assert(ty->isFloatingPointTy());
        DataType dt(ty);
        updateAnalysis(I.getOperand(0), dt, &I);
        updateAnalysis(I.getOperand(1), dt, &I);
        updateAnalysis(&I, dt, &I);
    } else {

        auto analysis = getAnalysis(&I);
        switch(I.getOpcode()) {
            case BinaryOperator::Sub:
                //TODO propagate this info
                // ptr - ptr => int and int - int => int; thus int = a - b says only that these are equal
                // ptr - int => ptr and int - ptr => ptr; thus
                analysis = DataType(IntType::Unknown);
                break;

            case BinaryOperator::Add:
            case BinaryOperator::Mul:
                // if a + b or a * b == int, then a and b must be ints
                analysis = analysis.JustInt();
                break;

            case BinaryOperator::UDiv:
            case BinaryOperator::SDiv:
            case BinaryOperator::URem:
            case BinaryOperator::SRem:
            case BinaryOperator::And:
            case BinaryOperator::Or:
            case BinaryOperator::Xor:
            case BinaryOperator::Shl:
            case BinaryOperator::AShr:
            case BinaryOperator::LShr:
                analysis = DataType(IntType::Unknown);
                break;
            default:
                llvm_unreachable("unknown binary operator");
        }
		updateAnalysis(I.getOperand(0), analysis, &I);
		updateAnalysis(I.getOperand(1), analysis, &I);

		ValueData vd = getAnalysis(I.getOperand(0));
		vd.pointerIntMerge(getAnalysis(I.getOperand(1)), I.getOpcode());

        //llvm::errs() << "vd: " << vd.str() << "\n";
        //llvm::errs() << "op(0): " << getAnalysis(I.getOperand(0)).str() << "\n";
        //llvm::errs() << "op(1): " << getAnalysis(I.getOperand(1)).str() << "\n";

        if (I.getOpcode() == BinaryOperator::And) {
            for(int i=0; i<2; i++)
            if (auto ci = fntypeinfo.isConstantInt(I.getOperand(i))) {
                if (ci->getLimitedValue() <= 16 && ci->getLimitedValue() >= 0) {

                    //I.getParent()->getParent()->dump();
                    //dump();
                    //llvm::errs() << "I: " << I << "\n";

                    vd |= ValueData(IntType::Integer);
                }
            }
        }
		updateAnalysis(&I, vd, &I);
    }
}

void TypeAnalyzer::visitCallInst(CallInst &call) {
	if (auto iasm = dyn_cast<InlineAsm>(call.getCalledValue())) {
		if (iasm->getAsmString() == "cpuid") {
			updateAnalysis(&call, ValueData(IntType::Integer), &call);
            for(unsigned i=0; i<call.getNumArgOperands(); i++) {
                updateAnalysis(call.getArgOperand(i), ValueData(IntType::Integer), &call);
            }
		}
	}

	if (Function* ci = call.getCalledFunction()) {

		if (ci->getName() == "malloc") {
			updateAnalysis(call.getArgOperand(0), IntType::Integer, &call);
		}

		//If memcpy / memmove of pointer, we can propagate type information from src to dst up to the length and vice versa
		if (ci->getIntrinsicID() == Intrinsic::memcpy || ci->getIntrinsicID() == Intrinsic::memmove) {
            //TODO length enforcement
            int sz = 1;
            if (auto ci = fntypeinfo.isConstantInt(call.getArgOperand(2))) {
                sz = (int)ci->getLimitedValue();
            }

			ValueData res = getAnalysis(call.getArgOperand(0)).AtMost(sz);
            ValueData res2 = getAnalysis(call.getArgOperand(1)).AtMost(sz);
            //llvm::errs() << " memcpy: " << call << " res1: " << res.str() << " res2: " << res2.str() << "\n";
            res |= res2;

			updateAnalysis(call.getArgOperand(0), res, &call);
			updateAnalysis(call.getArgOperand(1), res, &call);

            //call.getParent()->getParent()->dump();
            //dump();
            //llvm::errs() << "call: " << call << "\n";

			for(unsigned i=2; i<call.getNumArgOperands(); i++) {
				updateAnalysis(call.getArgOperand(i), IntType::Integer, &call);
			}
		}

		//TODO we should handle calls interprocedurally, allowing better propagation of type information
		if (!ci->empty()) {
			visitIPOCall(call, *ci);
		}
	}

}

ValueData TypeAnalyzer::getReturnAnalysis() {
    bool set = false;
    ValueData vd;
    for(auto &BB: *function) {
        for(auto &inst : BB) {
		    if (auto ri = dyn_cast<ReturnInst>(&inst)) {
			    if (auto rv = ri->getReturnValue()) {
                    if (set == false) {
                        set = true;
                        vd = getAnalysis(rv);
                        continue;
                    }
                    vd &= getAnalysis(rv);
                }
            }
        }
    }
    return vd;
}


llvm::Constant* NewFnTypeInfo::isConstant(llvm::Value* val) const {
    if (auto constant = dyn_cast<Constant>(val)) {
        return constant;
    }
    if (auto arg = dyn_cast<llvm::Argument>(val)) {
        auto found = third.find(arg);
        if (found == third.end()) {
            for(const auto& pair : third) {
                llvm::errs() << " third[" << *pair.first << "]=" << pair.second << " - " << pair.first->getParent()->getName() << "\n";
            }
            llvm::errs() << " arg: " << *arg << " - " << arg->getParent()->getName() << "\n";
        }
        assert(found != third.end());
        if (found->second) return found->second;
    }
    return nullptr;
}

llvm::ConstantInt* NewFnTypeInfo::isConstantInt(llvm::Value* val) const {
    if (auto ci = dyn_cast_or_null<ConstantInt>(isConstant(val))) {
        return ci;
    }
    return nullptr;
}

void TypeAnalyzer::visitIPOCall(CallInst& call, Function& fn) {
	NewFnTypeInfo typeInfo;

    int argnum = 0;
	for(auto &arg : fn.args()) {
		auto dt = getAnalysis(call.getArgOperand(argnum));
		typeInfo.first.insert(std::pair<Argument*, ValueData>(&arg, dt));
        typeInfo.third.insert(std::pair<Argument*, Constant*>(&arg, fntypeinfo.isConstant(call.getArgOperand(argnum))));

		argnum++;
	}

    typeInfo.second = getAnalysis(&call);


	auto a = fn.arg_begin();
	for(size_t i=0; i<call.getNumArgOperands(); i++) {
		auto dt = interprocedural.query(a, typeInfo);
		updateAnalysis(call.getArgOperand(i), dt, &call);
		a++;
	}

	ValueData vd = interprocedural.getReturnAnalysis(typeInfo, &fn);
	updateAnalysis(&call, vd, &call);
}

TypeResults TypeAnalysis::analyzeFunction(const NewFnTypeInfo& fn, Function* function) {
    if (analyzedFunctions.find(fn) != analyzedFunctions.end()) return TypeResults(*this, fn, function);

	auto res = analyzedFunctions.emplace(fn, TypeAnalyzer(function, fn, *this));
	auto& analysis = res.first->second;

	if (printtype) {
	    llvm::errs() << "analyzing function " << function->getName() << "\n";
	    for(auto &pair : fn.first) {
	        llvm::errs() << " + knowndata: " << *pair.first << " : " << pair.second.str();
            auto found = fn.third.find(pair.first);
            if (found != fn.third.end() && found->second) {
                llvm::errs() << " - " << *found->second;
            }
            llvm::errs() << "\n";
	    }
        llvm::errs() << " + retdata: " << fn.second.str() << "\n";
	}

    analysis.prepareArgs();
	analysis.considerTBAA();
	analysis.run();
	return TypeResults(*this, fn, function);
}

ValueData TypeAnalysis::query(Value* val, const NewFnTypeInfo& fn) {
    assert(val);
    assert(val->getType());

	if (isa<Constant>(val)) {
		if (auto ci = dyn_cast<ConstantInt>(val)) {
			if (ci->getLimitedValue() >=1 && ci->getLimitedValue() <= 4096) {
				return ValueData(DataType(IntType::Integer));
			}
            if (ci->getType()->getBitWidth() == 8 && ci->getLimitedValue() == 0) {
				return ValueData(DataType(IntType::Integer));
            }
		}
		return ValueData(DataType(IntType::Anything));
	}
	Function* func = nullptr;
	if (auto arg = dyn_cast<Argument>(val)) func = arg->getParent();
	if (auto inst = dyn_cast<Instruction>(val)) func = inst->getParent()->getParent();

	if (func == nullptr) return ValueData();

    analyzeFunction(fn, func);
	return analyzedFunctions.find(fn)->second.getAnalysis(val);
}

DataType TypeAnalysis::intType(Value* val, const NewFnTypeInfo& fn, bool errIfNotFound) {
    assert(val);
    assert(val->getType());
    assert(val->getType()->isIntOrIntVectorTy());
    auto q = query(val, fn);
    auto dt = q[{}];
    //llvm::errs() << " intType for val: " << *val << " q: " << q.str() << " dt: " << dt.str() << "\n";
	if (errIfNotFound && (!dt.isKnown() || dt.typeEnum == IntType::Anything) ) {
		if (auto inst = dyn_cast<Instruction>(val)) {
			llvm::errs() << *inst->getParent()->getParent() << "\n";
			for(auto &pair : analyzedFunctions.find(fn)->second.analysis) {
				llvm::errs() << "val: " << *pair.first << " - " << pair.second.str() << "\n";
			}
		}
		llvm::errs() << "could not deduce type of integer " << *val << "\n";
		assert(0 && "could not deduce type of integer");
	}
	return dt;
}

DataType TypeAnalysis::firstPointer(size_t num, Value* val, const NewFnTypeInfo& fn, bool errIfNotFound, bool pointerIntSame) {
    assert(val);
    assert(val->getType());
    assert(val->getType()->isPointerTy());
	auto q = query(val, fn);
	auto dt = q[{0}];
	dt.mergeIn(q[{-1}], pointerIntSame);
    for(size_t i=1; i<num; i++) {
        dt.mergeIn(q[{(int)i}], pointerIntSame);
    }

    /*
    if (auto inst = dyn_cast<Instruction>(val)) {
        assert(fn.first.begin()->first->getParent() == inst->getParent()->getParent());
        llvm::errs() << *inst->getParent()->getParent() << "\n";
        for(auto &pair : analyzedFunctions.find(fn)->second.analysis) {
            if (auto in = dyn_cast<Instruction>(pair.first)) {
                if (in->getParent()->getParent() != inst->getParent()->getParent()) {
                    llvm::errs() << "inf: " << *in->getParent()->getParent() << "\n";
                    llvm::errs() << "instf: " << *inst->getParent()->getParent() << "\n";
                    llvm::errs() << "in: " << *in << "\n";
                    llvm::errs() << "inst: " << *inst << "\n";
                }
                assert(in->getParent()->getParent() == inst->getParent()->getParent());
            }
            llvm::errs() << "val: " << *pair.first << " - " << pair.second.str() << "\n";
        }
    }
    */

	if (errIfNotFound && (!dt.isKnown() || dt.typeEnum == IntType::Anything) ) {
		if (auto inst = dyn_cast<Instruction>(val)) {
			llvm::errs() << *inst->getParent()->getParent() << "\n";
			for(auto &pair : analyzedFunctions.find(fn)->second.analysis) {
                if (auto in = dyn_cast<Instruction>(pair.first)) {
                    if (in->getParent()->getParent() != inst->getParent()->getParent()) {
                        llvm::errs() << "inf: " << *in->getParent()->getParent() << "\n";
                        llvm::errs() << "instf: " << *inst->getParent()->getParent() << "\n";
                        llvm::errs() << "in: " << *in << "\n";
                        llvm::errs() << "inst: " << *inst << "\n";
                    }
                    assert(in->getParent()->getParent() == inst->getParent()->getParent());
                }
				llvm::errs() << "val: " << *pair.first << " - " << pair.second.str() << "\n";
			}
		}
        if (auto arg = dyn_cast<Argument>(val)) {
            llvm::errs() << *arg->getParent() << "\n";
            for(auto &pair : analyzedFunctions.find(fn)->second.analysis) {
                if (auto in = dyn_cast<Instruction>(pair.first))
                    assert(in->getParent()->getParent() == arg->getParent());
                llvm::errs() << "val: " << *pair.first << " - " << pair.second.str() << "\n";
            }
        }
		llvm::errs() << "could not deduce type of integer " << *val << " num:" << num << " q:" << q.str() << " \n";
		assert(0 && "could not deduce type of integer");
	}
	return dt;
}

TypeResults::TypeResults(TypeAnalysis &analysis, const NewFnTypeInfo& fn, Function* function) : analysis(analysis), info(fn), function(function) {}


NewFnTypeInfo TypeResults::getAnalyzedTypeInfo() {
	NewFnTypeInfo res;
	for(auto &arg : function->args()) {
		res.first.insert(std::pair<Argument*, ValueData>(&arg, analysis.query(&arg, info)));
	}
    res.second = getReturnAnalysis();
    res.third = info.third;
	return res;
}

ValueData TypeResults::query(Value* val) {
    if (auto inst = dyn_cast<Instruction>(val)) {
        assert(inst->getParent()->getParent() == function);
    }
    if (auto arg = dyn_cast<Argument>(val)) {
        assert(arg->getParent() == function);
    }
    return analysis.query(val, info);
}


void TypeResults::dump() {
    assert(analysis.analyzedFunctions.find(info) != analysis.analyzedFunctions.end());
    analysis.analyzedFunctions.find(info)->second.dump();
}

DataType TypeResults::intType(Value* val, bool errIfNotFound) {
	return analysis.intType(val, info, errIfNotFound);
}

DataType TypeResults::firstPointer(size_t num, Value* val, bool errIfNotFound, bool pointerIntSame) {
    return analysis.firstPointer(num, val, info, errIfNotFound, pointerIntSame);
}

ValueData TypeResults::getReturnAnalysis() {
    return analysis.getReturnAnalysis(info, function);
}