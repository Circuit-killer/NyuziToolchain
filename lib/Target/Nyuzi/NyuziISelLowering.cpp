//===-- NyuziISelLowering.cpp - Nyuzi DAG Lowering Implementation ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the interfaces that Nyuzi uses to lower LLVM code
// into a selection DAG.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "nyuzi-isel-lowering"

#include "NyuziISelLowering.h"
#include "MCTargetDesc/NyuziMCTargetDesc.h"
#include "NyuziMachineFunctionInfo.h"
#include "NyuziTargetMachine.h"
#include "NyuziTargetObjectFile.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#include "NyuziGenCallingConv.inc"

const NyuziTargetLowering *
NyuziTargetLowering::create(const NyuziTargetMachine &TM,
                            const NyuziSubtarget &STI) {
  return new NyuziTargetLowering(TM, STI);
}

SDValue
NyuziTargetLowering::LowerReturn(SDValue Chain, CallingConv::ID CallConv,
                                 bool IsVarArg,
                                 const SmallVectorImpl<ISD::OutputArg> &Outs,
                                 const SmallVectorImpl<SDValue> &OutVals,
                                 const SDLoc &DL, SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();

  // CCValAssign - represent the assignment of the return value to locations.
  SmallVector<CCValAssign, 16> RVLocs;

  // CCState - Info about the registers and stack slot.
  CCState CCInfo(CallConv, IsVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  // Analyze return values.
  CCInfo.AnalyzeReturn(Outs, RetCC_Nyuzi32);

  SDValue Flag;
  SmallVector<SDValue, 4> RetOps(1, Chain);

  // Copy the result values into the output registers.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    CCValAssign &VA = RVLocs[i];
    assert(VA.isRegLoc() && "Can only return in registers!");
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVals[i], Flag);

    // Guarantee that all emitted copies are stuck together with flags.
    Flag = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  if (MF.getFunction()->hasStructRetAttr()) {
    NyuziMachineFunctionInfo *VFI = MF.getInfo<NyuziMachineFunctionInfo>();
    unsigned Reg = VFI->getSRetReturnReg();
    if (!Reg)
      llvm_unreachable("sret virtual register not created in the entry block");

    SDValue Val =
        DAG.getCopyFromReg(Chain, DL, Reg, getPointerTy(DAG.getDataLayout()));
    Chain = DAG.getCopyToReg(Chain, DL, Nyuzi::S0, Val, Flag);
    Flag = Chain.getValue(1);
    RetOps.push_back(
        DAG.getRegister(Nyuzi::S0, getPointerTy(DAG.getDataLayout())));
  }

  RetOps[0] = Chain; // Update chain.

  // Add the flag if we have it.
  if (Flag.getNode())
    RetOps.push_back(Flag);

  return DAG.getNode(NyuziISD::RET_FLAG, DL, MVT::Other, RetOps);
}

SDValue NyuziTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();

  // Analyze operands of the call, assigning locations to each operand.
  // NyuziCallingConv.td will auto-generate CC_Nyuzi32, which
  // knows how to handle operands (what go in registers vs. stack, etc).
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), ArgLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_Nyuzi32);

  // Walk through each parameter and push into InVals
  int ParamEndOffset = 0;
  for (const auto &VA : ArgLocs) {
    if (VA.isRegLoc()) {
      // Argument is in register
      EVT RegVT = VA.getLocVT();
      const TargetRegisterClass *RC;

      if (RegVT == MVT::i32 || RegVT == MVT::f32)
        RC = &Nyuzi::GPR32RegClass;
      else if (RegVT == MVT::v16i32 || RegVT == MVT::v16f32)
        RC = &Nyuzi::VR512RegClass;
      else
        llvm_unreachable("Unsupported formal argument Type");

      unsigned VReg = RegInfo.createVirtualRegister(RC);
      MF.getRegInfo().addLiveIn(VA.getLocReg(), VReg);
      SDValue Arg = DAG.getCopyFromReg(Chain, DL, VReg, VA.getLocVT());
      InVals.push_back(Arg);
      continue;
    }

    // Otherwise this parameter is on the stack
    assert(VA.isMemLoc());
    int ParamSize = VA.getValVT().getSizeInBits() / 8;
    int ParamOffset = VA.getLocMemOffset();
    int FI = MF.getFrameInfo()->CreateFixedObject(ParamSize, ParamOffset, true);
    ParamEndOffset = ParamOffset + ParamSize;

    SDValue FIPtr = DAG.getFrameIndex(FI, getPointerTy(DAG.getDataLayout()));
    SDValue Load;
    if (VA.getValVT() == MVT::i32 || VA.getValVT() == MVT::f32 ||
        VA.getValVT() == MVT::v16i32) {
      // Primitive Types are loaded directly from the stack
      Load = DAG.getLoad(VA.getValVT(), DL, Chain, FIPtr, MachinePointerInfo(),
                         false, false, false, 0);
    } else {
      // This is a smaller Type (char, etc).  Sign extend.
      ISD::LoadExtType LoadOp = ISD::SEXTLOAD;
      unsigned Offset = 4 - std::max(1U, VA.getValVT().getSizeInBits() / 8);
      FIPtr = DAG.getNode(ISD::ADD, DL, MVT::i32, FIPtr,
                          DAG.getConstant(Offset, DL, MVT::i32));
      Load = DAG.getExtLoad(LoadOp, DL, MVT::i32, Chain, FIPtr,
                            MachinePointerInfo(), VA.getValVT(), false, false,
                            false, 0);
      Load = DAG.getNode(ISD::TRUNCATE, DL, VA.getValVT(), Load);
    }

    InVals.push_back(Load);
  }

  NyuziMachineFunctionInfo *VFI = MF.getInfo<NyuziMachineFunctionInfo>();

  if (isVarArg) {
    // Create a dummy object where the first parameter would start.  This will
    // be used
    // later to determine the start address of variable arguments.
    int FirstVarArg =
        MF.getFrameInfo()->CreateFixedObject(4, ParamEndOffset, false);
    VFI->setVarArgsFrameIndex(FirstVarArg);
  }

  if (MF.getFunction()->hasStructRetAttr()) {
    // When a function returns a structure, the address of the return value
    // is placed in the first physical register.
    unsigned Reg = VFI->getSRetReturnReg();
    if (!Reg) {
      Reg = MF.getRegInfo().createVirtualRegister(&Nyuzi::GPR32RegClass);
      VFI->setSRetReturnReg(Reg);
    }

    SDValue Copy = DAG.getCopyToReg(DAG.getEntryNode(), DL, Reg, InVals[0]);
    Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, Copy, Chain);
  }

  return Chain;
}

// Generate code to call a function
SDValue NyuziTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                                       SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG = CLI.DAG;
  SDLoc &DL = CLI.DL;
  SmallVector<ISD::OutputArg, 32> &Outs = CLI.Outs;
  SmallVector<SDValue, 32> &OutVals = CLI.OutVals;
  SmallVector<ISD::InputArg, 32> &Ins = CLI.Ins;
  SDValue Chain = CLI.Chain;
  SDValue Callee = CLI.Callee;
  CallingConv::ID CallConv = CLI.CallConv;
  bool isVarArg = CLI.IsVarArg;

  // We do not support tail calls. This flag must be cleared in order
  // to indicate that to subsequent passes.
  CLI.IsTailCall = false;

  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  const TargetFrameLowering *TFL = MF.getSubtarget().getFrameLowering();

  // Analyze operands of the call, assigning locations to each operand.
  // NyuziCallingConv.td will auto-generate CC_Nyuzi32, which knows how to
  // handle operands (what go in registers vs. stack, etc).
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), ArgLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeCallOperands(Outs, CC_Nyuzi32);

  // Get the size of the outgoing arguments stack space requirement.
  // We always keep the stack pointer 64 byte aligned so we can use block
  // loads/stores for vector arguments
  unsigned ArgsSize =
      alignTo(CCInfo.getNextStackOffset(), TFL->getStackAlignment());

  // Create local copies for all arguments that are passed by value
  SmallVector<SDValue, 8> ByValArgs;
  for (unsigned i = 0, e = Outs.size(); i != e; ++i) {
    ISD::ArgFlagsTy Flags = Outs[i].Flags;
    if (!Flags.isByVal())
      continue;

    SDValue Arg = OutVals[i];
    unsigned Size = Flags.getByValSize();
    unsigned Align = Flags.getByValAlign();

    int FI = MFI->CreateStackObject(Size, Align, false);
    SDValue FIPtr = DAG.getFrameIndex(FI, getPointerTy(DAG.getDataLayout()));
    SDValue SizeNode = DAG.getConstant(Size, DL, MVT::i32);
    Chain = DAG.getMemcpy(Chain, DL, FIPtr, Arg, SizeNode, Align,
                          false,        // isVolatile,
                          (Size <= 32), // AlwaysInline if size <= 32
                          false, MachinePointerInfo(), MachinePointerInfo());

    ByValArgs.push_back(FIPtr);
  }

  // CALLSEQ_START will decrement the stack to reserve space
  Chain = DAG.getCALLSEQ_START(Chain, DAG.getIntPtrConstant(ArgsSize, DL, true),
                               DL);

  SmallVector<std::pair<unsigned, SDValue>, 8> RegsToPass;
  SmallVector<SDValue, 8> MemOpChains;

  // Walk through arguments, storing each one to the proper palce
  for (unsigned i = 0, realArgIdx = 0, byvalArgIdx = 0, e = ArgLocs.size();
       i != e; ++i, ++realArgIdx) {

    CCValAssign &VA = ArgLocs[i];
    SDValue Arg = OutVals[realArgIdx];

    ISD::ArgFlagsTy Flags = Outs[realArgIdx].Flags;

    // Use the local copy we created above if this is passed by value
    if (Flags.isByVal())
      Arg = ByValArgs[byvalArgIdx++];

    // Promote the value if needed.
    switch (VA.getLocInfo()) {
    case CCValAssign::Full:
      break;

    case CCValAssign::SExt:
      Arg = DAG.getNode(ISD::SIGN_EXTEND, DL, VA.getLocVT(), Arg);
      break;

    case CCValAssign::ZExt:
      Arg = DAG.getNode(ISD::ZERO_EXTEND, DL, VA.getLocVT(), Arg);
      break;

    case CCValAssign::AExt:
      Arg = DAG.getNode(ISD::ANY_EXTEND, DL, VA.getLocVT(), Arg);
      break;

    case CCValAssign::BCvt:
      Arg = DAG.getNode(ISD::BITCAST, DL, VA.getLocVT(), Arg);
      break;

    default:
      llvm_unreachable("Unknown loc info!");
    }

    // Arguments that can be passed on register must be kept at
    // RegsToPass vector
    if (VA.isRegLoc()) {
      RegsToPass.push_back(std::make_pair(VA.getLocReg(), Arg));
      continue;
    }

    // This needs to be pushed on the stack
    assert(VA.isMemLoc());

    // Create a store off the stack pointer for this argument.
    SDValue StackPtr = DAG.getRegister(Nyuzi::SP_REG, MVT::i32);
    SDValue PtrOff = DAG.getIntPtrConstant(VA.getLocMemOffset(), DL);
    PtrOff = DAG.getNode(ISD::ADD, DL, MVT::i32, StackPtr, PtrOff);
    MemOpChains.push_back(DAG.getStore(Chain, DL, Arg, PtrOff,
                                       MachinePointerInfo(), false, false, 0));
  }

  // Emit all stores, make sure the occur before any copies into physregs.
  if (!MemOpChains.empty()) {
    Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, MemOpChains);
  }

  // Build a sequence of copy-to-reg nodes chained together with token chain
  // and flag operands which copy the outgoing args into registers. The InFlag
  // in necessary since all emitted instructions must be stuck together.
  SDValue InFlag;
  for (const auto &Reg : RegsToPass) {
    Chain = DAG.getCopyToReg(Chain, DL, Reg.first, Reg.second, InFlag);
    InFlag = Chain.getValue(1);
  }

  // Get the function address.
  // If the callee is a GlobalAddress node (quite common, every direct call is)
  // turn it into a TargetGlobalAddress node so that legalize doesn't hack it.
  // Likewise ExternalSymbol -> TargetExternalSymbol.
  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), DL, MVT::i32);
  else if (ExternalSymbolSDNode *E = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(E->getSymbol(), MVT::i32);

  // Returns a chain & a flag for retval copy to use
  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);

  for (const auto &Reg : RegsToPass)
    Ops.push_back(DAG.getRegister(Reg.first, Reg.second.getValueType()));

  // Add a register mask operand representing the call-preserved registers.
  const TargetRegisterInfo *TRI = Subtarget.getRegisterInfo();
  const uint32_t *Mask = TRI->getCallPreservedMask(MF, CLI.CallConv);
  assert(Mask && "Missing call preserved mask for calling convention");
  Ops.push_back(CLI.DAG.getRegisterMask(Mask));

  if (InFlag.getNode())
    Ops.push_back(InFlag);

  Chain = DAG.getNode(NyuziISD::CALL, DL, NodeTys, Ops);
  InFlag = Chain.getValue(1);

  Chain = DAG.getCALLSEQ_END(Chain, DAG.getIntPtrConstant(ArgsSize, DL, true),
                             DAG.getIntPtrConstant(0, DL, true), InFlag, DL);
  InFlag = Chain.getValue(1);

  // The call has returned, handle return values
  SmallVector<CCValAssign, 16> RVLocs;
  CCState RVInfo(CallConv, isVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  RVInfo.AnalyzeCallResult(Ins, RetCC_Nyuzi32);

  // Copy all of the result registers out of their specified physreg.
  for (const auto &Loc : RVLocs) {
    Chain =
        DAG.getCopyFromReg(Chain, DL, Loc.getLocReg(), Loc.getValVT(), InFlag)
            .getValue(1);
    InFlag = Chain.getValue(2);
    InVals.push_back(Chain.getValue(0));
  }

  return Chain;
}

unsigned NyuziTargetLowering::getJumpTableEncoding() const {
  return MachineJumpTableInfo::EK_Inline;
}

NyuziTargetLowering::NyuziTargetLowering(const TargetMachine &TM,
                                         const NyuziSubtarget &STI)
    : TargetLowering(TM), Subtarget(STI) {

  // Set up the register classes.
  addRegisterClass(MVT::i32, &Nyuzi::GPR32RegClass);
  addRegisterClass(MVT::f32, &Nyuzi::GPR32RegClass);
  addRegisterClass(MVT::v16i32, &Nyuzi::VR512RegClass);
  addRegisterClass(MVT::v16f32, &Nyuzi::VR512RegClass);

  setOperationAction(ISD::BUILD_VECTOR, MVT::v16f32, Custom);
  setOperationAction(ISD::BUILD_VECTOR, MVT::v16i32, Custom);
  setOperationAction(ISD::INSERT_VECTOR_ELT, MVT::v16f32, Custom);
  setOperationAction(ISD::INSERT_VECTOR_ELT, MVT::v16i32, Custom);
  setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v16i32, Custom);
  setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v16f32, Custom);
  setOperationAction(ISD::SCALAR_TO_VECTOR, MVT::v16i32, Custom);
  setOperationAction(ISD::SCALAR_TO_VECTOR, MVT::v16f32, Custom);
  setOperationAction(ISD::GlobalAddress, MVT::i32, Custom);
  setOperationAction(ISD::GlobalAddress, MVT::f32, Custom);
  setOperationAction(ISD::ConstantPool, MVT::i32, Custom);
  setOperationAction(ISD::ConstantPool, MVT::f32, Custom);
  setOperationAction(ISD::Constant, MVT::i32, Custom);
  setOperationAction(ISD::BlockAddress, MVT::i32, Custom);
  setOperationAction(ISD::SELECT_CC, MVT::i32, Custom);
  setOperationAction(ISD::SELECT_CC, MVT::f32, Custom);
  setOperationAction(ISD::SELECT_CC, MVT::v16i32, Custom);
  setOperationAction(ISD::SELECT_CC, MVT::v16f32, Custom);
  setOperationAction(ISD::FDIV, MVT::f32, Custom);
  setOperationAction(ISD::FDIV, MVT::v16f32, Custom);
  setOperationAction(ISD::BR_JT, MVT::Other, Custom);
  setOperationAction(ISD::FNEG, MVT::f32, Custom);
  setOperationAction(ISD::FNEG, MVT::v16f32, Custom);
  setOperationAction(ISD::SETCC, MVT::f32, Custom);
  setOperationAction(ISD::CTLZ_ZERO_UNDEF, MVT::i32, Custom);
  setOperationAction(ISD::CTTZ_ZERO_UNDEF, MVT::i32, Custom);
  setOperationAction(ISD::UINT_TO_FP, MVT::i32, Custom);
  setOperationAction(ISD::UINT_TO_FP, MVT::v16i32, Custom);
  setOperationAction(ISD::FRAMEADDR, MVT::i32, Custom);
  setOperationAction(ISD::RETURNADDR, MVT::i32, Custom);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::v16i1, Custom);
  setOperationAction(ISD::VASTART, MVT::Other, Custom);
  setOperationAction(ISD::FABS, MVT::f32, Custom);
  setOperationAction(ISD::FABS, MVT::v16f32, Custom);
  setOperationAction(ISD::BR_CC, MVT::i32, Expand);
  setOperationAction(ISD::BR_CC, MVT::f32, Expand);
  setOperationAction(ISD::BRCOND, MVT::i32, Expand);
  setOperationAction(ISD::BRCOND, MVT::f32, Expand);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1, Expand);
  setOperationAction(ISD::CTPOP, MVT::i32, Expand);
  setOperationAction(ISD::SELECT, MVT::i32, Expand);
  setOperationAction(ISD::SELECT, MVT::f32, Expand);
  setOperationAction(ISD::ROTL, MVT::i32, Expand);
  setOperationAction(ISD::ROTR, MVT::i32, Expand);
  setOperationAction(ISD::ROTL, MVT::v16i32, Expand);
  setOperationAction(ISD::ROTR, MVT::v16i32, Expand);
  setOperationAction(ISD::UDIVREM, MVT::i32, Expand);
  setOperationAction(ISD::SDIVREM, MVT::i32, Expand);
  setOperationAction(ISD::UMUL_LOHI, MVT::i32, Expand);
  setOperationAction(ISD::SMUL_LOHI, MVT::i32, Expand);
  setOperationAction(ISD::FP_TO_UINT, MVT::i32, Expand);
  setOperationAction(ISD::FP_TO_UINT, MVT::v16i32, Expand);
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i32, Expand);
  setOperationAction(ISD::STACKSAVE, MVT::Other, Expand);
  setOperationAction(ISD::STACKRESTORE, MVT::Other, Expand);
  setOperationAction(ISD::BSWAP, MVT::i32, Expand);
  setOperationAction(ISD::ADDC, MVT::i32, Expand);
  setOperationAction(ISD::ADDE, MVT::i32, Expand);
  setOperationAction(ISD::SUBC, MVT::i32, Expand);
  setOperationAction(ISD::SUBE, MVT::i32, Expand);
  setOperationAction(ISD::SRA_PARTS, MVT::i32, Expand);
  setOperationAction(ISD::SRL_PARTS, MVT::i32, Expand);
  setOperationAction(ISD::SHL_PARTS, MVT::i32, Expand);
  setOperationAction(ISD::VAARG, MVT::Other, Expand);
  setOperationAction(ISD::VACOPY, MVT::Other, Expand);
  setOperationAction(ISD::VAEND, MVT::Other, Expand);
  setOperationAction(ISD::ATOMIC_LOAD, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD, MVT::i64, Expand);
  setOperationAction(ISD::ATOMIC_STORE, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_STORE, MVT::i64, Expand);

  setCondCodeAction(ISD::SETO, MVT::f32, Custom);
  setCondCodeAction(ISD::SETUO, MVT::f32, Custom);
  setCondCodeAction(ISD::SETUEQ, MVT::f32, Custom);
  setCondCodeAction(ISD::SETUNE, MVT::f32, Custom);
  setCondCodeAction(ISD::SETUGT, MVT::f32, Custom);
  setCondCodeAction(ISD::SETUGE, MVT::f32, Custom);
  setCondCodeAction(ISD::SETULT, MVT::f32, Custom);
  setCondCodeAction(ISD::SETULE, MVT::f32, Custom);
  setCondCodeAction(ISD::SETUEQ, MVT::v16f32, Custom);
  setCondCodeAction(ISD::SETUNE, MVT::v16f32, Custom);
  setCondCodeAction(ISD::SETUGT, MVT::v16f32, Custom);
  setCondCodeAction(ISD::SETUGE, MVT::v16f32, Custom);
  setCondCodeAction(ISD::SETULT, MVT::v16f32, Custom);
  setCondCodeAction(ISD::SETULE, MVT::v16f32, Custom);

  setOperationAction(ISD::FCOPYSIGN, MVT::f32, Expand);
  setOperationAction(ISD::FFLOOR, MVT::f32, Expand);
  setOperationAction(ISD::FFLOOR, MVT::v16f32, Expand);

  // Hardware does not have an integer divider, so convert these to
  // library calls
  setOperationAction(ISD::UDIV, MVT::i32, Expand); // __udivsi3
  setOperationAction(ISD::UREM, MVT::i32, Expand); // __umodsi3
  setOperationAction(ISD::SDIV, MVT::i32, Expand); // __divsi3
  setOperationAction(ISD::SREM, MVT::i32, Expand); // __modsi3

  setOperationAction(ISD::FSQRT, MVT::f32, Expand); // sqrtf
  setOperationAction(ISD::FSIN, MVT::f32, Expand);  // sinf
  setOperationAction(ISD::FCOS, MVT::f32, Expand);  // cosf
  setOperationAction(ISD::FSINCOS, MVT::f32, Expand);

  setStackPointerRegisterToSaveRestore(Nyuzi::SP_REG);
  setMinFunctionAlignment(2);
  setSelectIsExpensive(); // Because there is no CMOV

  computeRegisterProperties(Subtarget.getRegisterInfo());
}

const char *NyuziTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case NyuziISD::CALL:
    return "NyuziISD::CALL";
  case NyuziISD::RET_FLAG:
    return "NyuziISD::RET_FLAG";
  case NyuziISD::SPLAT:
    return "NyuziISD::SPLAT";
  case NyuziISD::SEL_COND_RESULT:
    return "NyuziISD::SEL_COND_RESULT";
  case NyuziISD::RECIPROCAL_EST:
    return "NyuziISD::RECIPROCAL_EST";
  case NyuziISD::BR_JT:
    return "NyuziISD::BR_JT";
  case NyuziISD::JT_WRAPPER:
    return "NyuziISD::JT_WRAPPER";
  default:
    return nullptr;
  }
}

// Global addresses are stored in the per-function constant pool.
SDValue NyuziTargetLowering::LowerGlobalAddress(SDValue Op,
                                                SelectionDAG &DAG) const {
  SDLoc DL(Op);
  const GlobalValue *GV = cast<GlobalAddressSDNode>(Op)->getGlobal();
  SDValue CPIdx = DAG.getTargetConstantPool(GV, MVT::i32);
  return DAG.getLoad(
      MVT::i32, DL, DAG.getEntryNode(), CPIdx,
      MachinePointerInfo::getConstantPool(DAG.getMachineFunction()), false,
      false, false, 4);
}

SDValue NyuziTargetLowering::LowerConstantPool(SDValue Op,
                                               SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT PtrVT = Op.getValueType();
  ConstantPoolSDNode *CP = cast<ConstantPoolSDNode>(Op);
  SDValue Res;
  if (CP->isMachineConstantPoolEntry()) {
    Res = DAG.getTargetConstantPool(CP->getMachineCPVal(), PtrVT,
                                    CP->getAlignment());
  } else {
    Res =
        DAG.getTargetConstantPool(CP->getConstVal(), PtrVT, CP->getAlignment());
  }

  return Res;
}

//
// XXX The intent of this function is to check if the immediate will fit in the
// instruction or needs to be loaded from the constant pool. However, other
// backends don't seem to need to do this. This may be overkill (it also
// doens't work quite correctly, since we don't know which instruction this
// will be used for, and thus don't know the capacity).
//
SDValue NyuziTargetLowering::LowerConstant(SDValue Op,
                                           SelectionDAG &DAG) const {
  SDLoc DL(Op);
  ConstantSDNode *C = cast<ConstantSDNode>(Op);

  const int kMaxImmediateSize = 13;

  if (C->getAPIntValue().abs().ult((1 << (kMaxImmediateSize - 1)) - 1)) {
    // Don't need to convert to constant pool reference.  This will fit in
    // the immediate field of a single instruction, sign extended.
    return Op;
  }

  // XXX New versions of LLVM will expand to a constant pool as the expand
  // action, so this is probably unnecessary.
  SDValue CPIdx = DAG.getConstantPool(C->getConstantIntValue(), MVT::i32);
  return DAG.getLoad(
      MVT::i32, DL, DAG.getEntryNode(), CPIdx,
      MachinePointerInfo::getConstantPool(DAG.getMachineFunction()), false,
      false, false, 4);
}

SDValue NyuziTargetLowering::LowerBlockAddress(SDValue Op,
                                               SelectionDAG &DAG) const {
  SDLoc DL(Op);
  const BlockAddress *BA = cast<BlockAddressSDNode>(Op)->getBlockAddress();
  SDValue CPIdx = DAG.getTargetConstantPool(BA, MVT::i32);
  return DAG.getLoad(
      MVT::i32, DL, DAG.getEntryNode(), CPIdx,
      MachinePointerInfo::getConstantPool(DAG.getMachineFunction()), false,
      false, false, 4);
}

SDValue NyuziTargetLowering::LowerVASTART(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  MachineFunction &MF = DAG.getMachineFunction();
  NyuziMachineFunctionInfo *VFI = MF.getInfo<NyuziMachineFunctionInfo>();
  SDValue FI = DAG.getFrameIndex(VFI->getVarArgsFrameIndex(),
                                 getPointerTy(DAG.getDataLayout()));
  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
  return DAG.getStore(Op.getOperand(0), DL, FI, Op.getOperand(1),
                      MachinePointerInfo(SV), false, false, 0);
}

// Mask off the sign bit
SDValue NyuziTargetLowering::LowerFABS(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  MVT ResultVT = Op.getValueType().getSimpleVT();
  MVT IntermediateVT = ResultVT.isVector() ? MVT::v16i32 : MVT::i32;

  SDValue rhs = DAG.getConstant(0x7fffffff, DL, MVT::i32);
  SDValue iconv;
  if (ResultVT.isVector())
    rhs = DAG.getNode(NyuziISD::SPLAT, DL, MVT::v16i32, rhs);

  iconv = DAG.getNode(ISD::BITCAST, DL, IntermediateVT, Op.getOperand(0));
  SDValue flipped = DAG.getNode(ISD::AND, DL, IntermediateVT, iconv, rhs);
  return DAG.getNode(ISD::BITCAST, DL, ResultVT, flipped);
}

// isSplatVector - Returns true if N is a BUILD_VECTOR node whose elements are
// all the same.
static bool isSplatVector(SDNode *N) {
  SDValue SplatValue = N->getOperand(0);
  for (auto Lane : N->op_values())
    if (Lane != SplatValue)
      return false;

  return true;
}

SDValue NyuziTargetLowering::LowerBUILD_VECTOR(SDValue Op,
                                               SelectionDAG &DAG) const {
  MVT VT = Op.getValueType().getSimpleVT();
  SDLoc DL(Op);

  if (isSplatVector(Op.getNode())) {
    // This is a constant node that is duplicated to all lanes.
    // Convert it to a SPLAT node.
    return DAG.getNode(NyuziISD::SPLAT, DL, VT, Op.getOperand(0));
  }

  return SDValue(); // Expand
}

// SCALAR_TO_VECTOR loads the scalar register into lane 0 of the register.
// The rest of the lanes are undefined.  For simplicity, we just load the same
// value into all lanes.
SDValue NyuziTargetLowering::LowerSCALAR_TO_VECTOR(SDValue Op,
                                                   SelectionDAG &DAG) const {
  MVT VT = Op.getValueType().getSimpleVT();
  SDLoc DL(Op);
  return DAG.getNode(NyuziISD::SPLAT, DL, VT, Op.getOperand(0));
}

// (VECTOR, VAL, IDX)
// Convert to a move with a mask (0x8000 >> IDX) and a splatted scalar operand.
SDValue NyuziTargetLowering::LowerINSERT_VECTOR_ELT(SDValue Op,
                                                    SelectionDAG &DAG) const {
  MVT VT = Op.getValueType().getSimpleVT();
  SDLoc DL(Op);

  // This could also be (1 << (15 - index)), which avoids the load of 0x8000
  // but requires more operations.
  SDValue Mask =
      DAG.getNode(ISD::SRL, DL, MVT::i32, DAG.getConstant(0x8000, DL, MVT::i32),
                  Op.getOperand(2));
  SDValue Splat = DAG.getNode(NyuziISD::SPLAT, DL, VT, Op.getOperand(1));
  return DAG.getNode(
      ISD::INTRINSIC_WO_CHAIN, DL, VT,
      DAG.getConstant(Intrinsic::nyuzi_vector_mixi, DL, MVT::i32), Mask, Splat,
      Op.getOperand(0));
}

// This is called to determine if a VECTOR_SHUFFLE should be lowered by this
// file.
bool NyuziTargetLowering::isShuffleMaskLegal(const SmallVectorImpl<int> &M,
                                             EVT VT) const {
  if (M.size() != 16)
    return false;

  for (int val : M) {
    if (val > 31)
      return false;
  }

  return true;
}

// VECTOR_SHUFFLE(vec1, vec2, shuffle_indices)
SDValue NyuziTargetLowering::LowerVECTOR_SHUFFLE(SDValue Op,
                                                 SelectionDAG &DAG) const {
  MVT VT = Op.getValueType().getSimpleVT();
  SDLoc DL(Op);
  ShuffleVectorSDNode *ShuffleNode = dyn_cast<ShuffleVectorSDNode>(Op);

  // Check if this builds a splat (all elements of vector are the same)
  // using shufflevector like this:
  // %vector = shufflevector <16 x i32> %single, <16 x i32> (don't care),
  //                         <16 x i32> zeroinitializer
  // %single = insertelement <16 x i32> (don't care), i32 %value, i32 0
  if (ShuffleNode->isSplat() &&
      Op.getOperand(0).getOpcode() == ISD::INSERT_VECTOR_ELT &&
      ShuffleNode->getSplatIndex() ==
          dyn_cast<ConstantSDNode>(Op.getOperand(0).getOperand(2))
              ->getSExtValue())
    return DAG.getNode(NyuziISD::SPLAT, DL, VT, Op.getOperand(0).getOperand(1));

  // scalar_to_vector loads a scalar element into the lowest lane of the vector.
  // The higher lanes are undefined (which means we can load the same value into
  // them using splat).
  // %single = scalar_to_vector i32 %b
  if (Op.getOperand(0).getOpcode() == ISD::SCALAR_TO_VECTOR)
    return DAG.getNode(NyuziISD::SPLAT, DL, VT, Op.getOperand(0).getOperand(0));

  if (ShuffleNode->isSplat()) {
    // This is a splat where the element is taken from another vector that
    // we don't know the value of. First extract element, then broadcast it.
    int SplatIndex = ShuffleNode->getSplatIndex();
    SDValue SourceVector =
        SplatIndex < 16 ? Op.getOperand(0) : Op.getOperand(1);
    SDValue LaneIndexValue = DAG.getConstant(SplatIndex % 16, DL, MVT::i32);
    SDValue LaneValue = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::i32,
                                    SourceVector, LaneIndexValue);
    return DAG.getNode(NyuziISD::SPLAT, DL, VT, LaneValue);
  }

  SDValue NativeShuffleIntr =
      DAG.getConstant(Intrinsic::nyuzi_shufflei, DL, MVT::i32);
  SDValue MixIntr = DAG.getConstant(Intrinsic::nyuzi_vector_mixi, DL, MVT::i32);

  // Analyze the vector indices.
  unsigned int Mask = 0;
  bool IsIdentityShuffle = true;
  Constant *ShuffleIndexValues[16];

  for (unsigned int SourceLane = 0; SourceLane < 16; SourceLane++) {
    unsigned int DestLaneIndex = ShuffleNode->getMaskElt(SourceLane);
    Mask <<= 1;
    if (DestLaneIndex > 15)
      Mask |= 1;

    if ((DestLaneIndex & 15) != SourceLane)
      IsIdentityShuffle = false;

    ShuffleIndexValues[SourceLane] = ConstantInt::get(
        Type::getInt32Ty(*DAG.getContext()), DestLaneIndex & 15);
  }

  Constant *ShuffleConstVector = ConstantVector::get(ShuffleIndexValues);
  SDValue ShuffleVectorCP =
      DAG.getTargetConstantPool(ShuffleConstVector, MVT::v16i32);
  SDValue ShuffleVector =
      DAG.getLoad(MVT::v16i32, DL, DAG.getEntryNode(), ShuffleVectorCP,
                  MachinePointerInfo::getConstantPool(DAG.getMachineFunction()),
                  false, false, false, 64);

  // Check if the operands are equal
  if (Op.getOperand(0) == Op.getOperand(1))
    Mask = 0;

  if (Mask == 0xffff || Mask == 0) {
    // Only one of the vectors is referenced.
    SDValue ShuffleSource = Mask == 0 ? Op.getOperand(0) : Op.getOperand(1);

    if (IsIdentityShuffle)
      return ShuffleSource; // Is just a copy
    else
      return DAG.getNode(ISD::INTRINSIC_WO_CHAIN, DL, MVT::v16i32,
                         NativeShuffleIntr, ShuffleSource, ShuffleVector);
  } else if (IsIdentityShuffle) {
    // This is just a mix
    SDValue MaskVal = DAG.getConstant(Mask, DL, MVT::i32);
    return DAG.getNode(ISD::INTRINSIC_WO_CHAIN, DL, MVT::v16i32, MixIntr,
                       MaskVal, Op.getOperand(1), Op.getOperand(0));
  } else {
    // Need to shuffle both vectors and mix
    SDValue MaskVal = DAG.getConstant(Mask, DL, MVT::i32);
    SDValue Shuffled0 =
        DAG.getNode(ISD::INTRINSIC_WO_CHAIN, DL, MVT::v16i32, NativeShuffleIntr,
                    Op.getOperand(0), ShuffleVector);
    SDValue Shuffled1 =
        DAG.getNode(ISD::INTRINSIC_WO_CHAIN, DL, MVT::v16i32, NativeShuffleIntr,
                    Op.getOperand(1), ShuffleVector);
    return DAG.getNode(ISD::INTRINSIC_WO_CHAIN, DL, MVT::v16i32, MixIntr,
                       MaskVal, Shuffled1, Shuffled0);
  }
}

// This architecture does not support conditional moves for scalar registers.
// We must convert this into a set of conditional branches. We do this by
// creating a pseudo-instruction SEL_COND_RESULT, which will later be
// transformed.
SDValue NyuziTargetLowering::LowerSELECT_CC(SDValue Op,
                                            SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT Ty = Op.getOperand(0).getValueType();
  SDValue Pred =
      DAG.getNode(ISD::SETCC, DL, getSetCCResultType(DAG.getDataLayout(),
                                                     *DAG.getContext(), Ty),
                  Op.getOperand(0), Op.getOperand(1), Op.getOperand(4));

  return DAG.getNode(NyuziISD::SEL_COND_RESULT, DL, Op.getValueType(), Pred,
                     Op.getOperand(2), Op.getOperand(3));
}

// There is no native floating point division, but we can convert this to a
// reciprocal/multiply operation.  If the first parameter is constant 1.0, then
// just a reciprocal will suffice.
SDValue NyuziTargetLowering::LowerFDIV(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);

  EVT Type = Op.getOperand(1).getValueType();

  SDValue Two = DAG.getConstantFP(2.0, DL, Type);
  SDValue Denominator = Op.getOperand(1);
  SDValue Estimate =
      DAG.getNode(NyuziISD::RECIPROCAL_EST, DL, Type, Denominator);

  // Perform a series of Newton Raphson refinements to determine 1/divisor. Each
  // iteration doubles the precision of the result. The initial estimate has 6
  // bits of precision, so two iterations results in 24 bits, which is larger
  // than the (23 bit) significand.

  for (int i = 0; i < 2; i++) {
    // Trial = x * Estimate (our target is for x * 1/x to be 1.0)
    // Error = 2.0 - Trial
    // Estimate = Estimate * Error
    SDValue Trial = DAG.getNode(ISD::FMUL, DL, Type, Estimate, Denominator);
    SDValue Error = DAG.getNode(ISD::FSUB, DL, Type, Two, Trial);
    Estimate = DAG.getNode(ISD::FMUL, DL, Type, Estimate, Error);
  }

  // Check if the first parameter is constant 1.0.  If so, we don't need
  // to multiply by the dividend.
  bool IsOne = false;
  if (Type.isVector()) {
    if (isSplatVector(Op.getOperand(0).getNode())) {
      ConstantFPSDNode *C =
          dyn_cast<ConstantFPSDNode>(Op.getOperand(0).getOperand(0));
      IsOne = C && C->isExactlyValue(1.0);
    }
  } else {
    ConstantFPSDNode *C = dyn_cast<ConstantFPSDNode>(Op.getOperand(0));
    IsOne = C && C->isExactlyValue(1.0);
  }

  if (!IsOne)
    Estimate = DAG.getNode(ISD::FMUL, DL, Type, Op.getOperand(0), Estimate);

  return Estimate;
}

// Branch using jump table (used for switch statements)
SDValue NyuziTargetLowering::LowerBR_JT(SDValue Op, SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDValue Table = Op.getOperand(1);
  SDValue Index = Op.getOperand(2);
  SDLoc DL(Op);
  EVT PTy = getPointerTy(DAG.getDataLayout());
  JumpTableSDNode *JT = cast<JumpTableSDNode>(Table);
  SDValue JTI = DAG.getTargetJumpTable(JT->getIndex(), PTy);
  SDValue TableWrapper = DAG.getNode(NyuziISD::JT_WRAPPER, DL, PTy, JTI);
  SDValue TableMul =
      DAG.getNode(ISD::MUL, DL, PTy, Index, DAG.getConstant(4, DL, PTy));
  SDValue JTAddr = DAG.getNode(ISD::ADD, DL, PTy, TableWrapper, TableMul);
  return DAG.getNode(NyuziISD::BR_JT, DL, MVT::Other, Chain, JTAddr, JTI);
}

// There is no native FNEG instruction, so we emulate it by XORing with
// 0x80000000
SDValue NyuziTargetLowering::LowerFNEG(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  MVT ResultVT = Op.getValueType().getSimpleVT();
  MVT IntermediateVT = ResultVT.isVector() ? MVT::v16i32 : MVT::i32;

  SDValue rhs = DAG.getConstant(0x80000000, DL, MVT::i32);
  SDValue iconv;
  if (ResultVT.isVector())
    rhs = DAG.getNode(NyuziISD::SPLAT, DL, MVT::v16i32, rhs);

  iconv = DAG.getNode(ISD::BITCAST, DL, IntermediateVT, Op.getOperand(0));
  SDValue flipped = DAG.getNode(ISD::XOR, DL, IntermediateVT, iconv, rhs);
  return DAG.getNode(ISD::BITCAST, DL, ResultVT, flipped);
}

namespace {

SDValue morphSETCCNode(SDValue Op, ISD::CondCode code, SelectionDAG &DAG) {
  SDLoc DL(Op);
  return DAG.getNode(ISD::SETCC, DL, Op.getValueType().getSimpleVT(),
                     Op.getOperand(0), Op.getOperand(1), DAG.getCondCode(code));
}
}

// Convert unordered or don't-care floating point comparisions to ordered
// - Two comparison values are ordered if neither operand is NaN, otherwise they
//   are unordered.
// - An ordered comparison *operation* is always false if either operand is NaN.
//   Unordered is always true if either operand is NaN.
// - The hardware implements ordered comparisons.
// - Clang usually emits ordered comparisons.
SDValue NyuziTargetLowering::LowerSETCC(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(2))->get();
  ISD::CondCode ComplementCompare;
  switch (CC) {
  // Return this node unchanged
  default:
    return Op;

  // These are "don't care" values. Convert them to ordered, which
  // are natively supported
  case ISD::SETGT:
    return morphSETCCNode(Op, ISD::SETOGT, DAG);
  case ISD::SETGE:
    return morphSETCCNode(Op, ISD::SETOGE, DAG);
  case ISD::SETLT:
    return morphSETCCNode(Op, ISD::SETOLT, DAG);
  case ISD::SETLE:
    return morphSETCCNode(Op, ISD::SETOLE, DAG);
  case ISD::SETEQ:
    return morphSETCCNode(Op, ISD::SETOEQ, DAG);
  case ISD::SETNE:
    return morphSETCCNode(Op, ISD::SETONE, DAG);

  // Check for ordered and unordered values by using ordered equality
  // (which will only be false if the values are unordered)
  case ISD::SETO:
  case ISD::SETUO: {
    SDValue Op0 = Op.getOperand(0);
    SDValue IsOrdered =
        DAG.getNode(ISD::SETCC, DL, Op.getValueType().getSimpleVT(), Op0, Op0,
                    DAG.getCondCode(ISD::SETOEQ));
    if (CC == ISD::SETO)
      return IsOrdered;

    // SETUO
    return DAG.getNode(ISD::XOR, DL, Op.getValueType().getSimpleVT(), IsOrdered,
                       DAG.getConstant(0xffff, DL, MVT::i32));
  }

  // Convert unordered comparisions to ordered by explicitly checking for NaN
  case ISD::SETUEQ:
    ComplementCompare = ISD::SETONE;
    break;
  case ISD::SETUGT:
    ComplementCompare = ISD::SETOLE;
    break;
  case ISD::SETUGE:
    ComplementCompare = ISD::SETOLT;
    break;
  case ISD::SETULT:
    ComplementCompare = ISD::SETOGE;
    break;
  case ISD::SETULE:
    ComplementCompare = ISD::SETOGT;
    break;
  case ISD::SETUNE:
    ComplementCompare = ISD::SETOEQ;
    break;
  }

  // Take the complementary comparision and invert the result. This will
  // be the same for ordered values, but will always be true for unordered
  // values.
  SDValue Comp2 = morphSETCCNode(Op, ComplementCompare, DAG);
  return DAG.getNode(ISD::XOR, DL, Op.getValueType().getSimpleVT(), Comp2,
                     DAG.getConstant(0xffff, DL, MVT::i32));
}

SDValue NyuziTargetLowering::LowerCTLZ_ZERO_UNDEF(SDValue Op,
                                                  SelectionDAG &DAG) const {
  SDLoc DL(Op);
  return DAG.getNode(ISD::CTLZ, DL, Op.getValueType(), Op.getOperand(0));
}

SDValue NyuziTargetLowering::LowerCTTZ_ZERO_UNDEF(SDValue Op,
                                                  SelectionDAG &DAG) const {
  SDLoc DL(Op);
  return DAG.getNode(ISD::CTTZ, DL, Op.getValueType(), Op.getOperand(0));
}

//
// The architecture only supports signed integer to floating point.  If the
// source value is negative (when treated as signed), then add UINT_MAX to the
// resulting floating point value to adjust it.
// This is a simpler version of SelectionDAGLegalize::ExpandLegalINT_TO_FP.
// I've done a custom expansion because the default version uses arithmetic
// with constant pool symbols, and that gets clobbered (see comments in
// NyuziMCInstLower::Lower)
//

SDValue NyuziTargetLowering::LowerUINT_TO_FP(SDValue Op,
                                             SelectionDAG &DAG) const {
  SDLoc DL(Op);

  MVT ResultVT = Op.getValueType().getSimpleVT();
  SDValue RVal = Op.getOperand(0);
  SDValue SignedVal = DAG.getNode(ISD::SINT_TO_FP, DL, ResultVT, RVal);

  // Load constant offset to adjust
  Constant *AdjustConst =
      ConstantInt::get(Type::getInt32Ty(*DAG.getContext()),
                       0x4f800000); // UINT_MAX in float format
  SDValue CPIdx = DAG.getConstantPool(AdjustConst, MVT::f32);

  // XXX is this necessary, or will codegen call LowerConstantPool to convert
  // to a load?
  SDValue AdjustReg =
      DAG.getLoad(MVT::f32, DL, DAG.getEntryNode(), CPIdx,
                  MachinePointerInfo::getConstantPool(DAG.getMachineFunction()),
                  false, false, false, 4);
  if (ResultVT.isVector()) {
    // Vector Result
    SDValue ZeroVec = DAG.getNode(NyuziISD::SPLAT, DL, MVT::v16i32,
                                  DAG.getConstant(0, DL, MVT::i32));
    SDValue LtIntrinsic =
        DAG.getConstant(Intrinsic::nyuzi_mask_cmpi_slt, DL, MVT::i32);
    SDValue IsNegativeMask = DAG.getNode(ISD::INTRINSIC_WO_CHAIN, DL, MVT::i32,
                                         LtIntrinsic, RVal, ZeroVec);
    SDValue AdjustVec =
        DAG.getNode(NyuziISD::SPLAT, DL, MVT::v16f32, AdjustReg);
    SDValue Adjusted =
        DAG.getNode(ISD::FADD, DL, ResultVT, SignedVal, AdjustVec);
    return DAG.getNode(
        ISD::INTRINSIC_WO_CHAIN, DL, ResultVT,
        DAG.getConstant(Intrinsic::nyuzi_vector_mixf, DL, MVT::i32),
        IsNegativeMask, Adjusted, SignedVal);
  } else {
    // Scalar Result.  If the result is negative, add UINT_MASK to make it
    // positive
    SDValue IsNegative =
        DAG.getSetCC(DL, getSetCCResultType(DAG.getDataLayout(),
                                            *DAG.getContext(), MVT::i32),
                     RVal, DAG.getConstant(0, DL, MVT::i32), ISD::SETLT);
    SDValue Adjusted =
        DAG.getNode(ISD::FADD, DL, MVT::f32, SignedVal, AdjustReg);
    return DAG.getNode(NyuziISD::SEL_COND_RESULT, DL, MVT::f32, IsNegative,
                       Adjusted, SignedVal);
  }
}

SDValue NyuziTargetLowering::LowerFRAMEADDR(SDValue Op,
                                            SelectionDAG &DAG) const {
  assert((cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue() == 0) &&
         "Frame address can only be determined for current frame.");

  SDLoc DL(Op);
  MachineFrameInfo *MFI = DAG.getMachineFunction().getFrameInfo();
  MFI->setFrameAddressIsTaken(true);
  EVT VT = Op.getValueType();
  return DAG.getCopyFromReg(DAG.getEntryNode(), DL, Nyuzi::FP_REG, VT);
}

SDValue NyuziTargetLowering::LowerRETURNADDR(SDValue Op,
                                             SelectionDAG &DAG) const {
  if (verifyReturnAddressArgumentIsConstant(Op, DAG))
    return SDValue();

  // check the depth
  assert((cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue() == 0) &&
         "Return address can be determined only for current frame.");

  SDLoc DL(Op);
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  MVT VT = Op.getSimpleValueType();
  MFI->setReturnAddressIsTaken(true);

  // Return RA, which contains the return address. Mark it an implicit live-in.
  unsigned Reg = MF.addLiveIn(Nyuzi::RA_REG, getRegClassFor(VT));
  return DAG.getCopyFromReg(DAG.getEntryNode(), DL, Reg, VT);
}

static Intrinsic::ID intrinsicForVectorCompare(ISD::CondCode CC, bool isFloat) {
  if (isFloat) {
    switch (CC) {
    case ISD::SETOEQ:
    case ISD::SETUEQ:
    case ISD::SETEQ:
      return Intrinsic::nyuzi_mask_cmpf_eq;

    case ISD::SETONE:
    case ISD::SETUNE:
    case ISD::SETNE:
      return Intrinsic::nyuzi_mask_cmpf_ne;

    case ISD::SETOGT:
    case ISD::SETUGT:
    case ISD::SETGT:
      return Intrinsic::nyuzi_mask_cmpf_gt;

    case ISD::SETOGE:
    case ISD::SETUGE:
    case ISD::SETGE:
      return Intrinsic::nyuzi_mask_cmpf_ge;

    case ISD::SETOLT:
    case ISD::SETULT:
    case ISD::SETLT:
      return Intrinsic::nyuzi_mask_cmpf_lt;

    case ISD::SETOLE:
    case ISD::SETULE:
    case ISD::SETLE:
      return Intrinsic::nyuzi_mask_cmpf_le;

    default:; // falls through
    }
  } else {
    switch (CC) {
    case ISD::SETUEQ:
    case ISD::SETEQ:
      return Intrinsic::nyuzi_mask_cmpi_eq;

    case ISD::SETUNE:
    case ISD::SETNE:
      return Intrinsic::nyuzi_mask_cmpi_ne;

    case ISD::SETUGT:
      return Intrinsic::nyuzi_mask_cmpi_ugt;

    case ISD::SETGT:
      return Intrinsic::nyuzi_mask_cmpi_sgt;

    case ISD::SETUGE:
      return Intrinsic::nyuzi_mask_cmpi_uge;

    case ISD::SETGE:
      return Intrinsic::nyuzi_mask_cmpi_sge;

    case ISD::SETULT:
      return Intrinsic::nyuzi_mask_cmpi_ult;

    case ISD::SETLT:
      return Intrinsic::nyuzi_mask_cmpi_slt;

    case ISD::SETULE:
      return Intrinsic::nyuzi_mask_cmpi_ule;

    case ISD::SETLE:
      return Intrinsic::nyuzi_mask_cmpi_sle;

    default:; // falls through
    }
  }

  llvm_unreachable("unhandled compare code");
}

//
// This may be used to expand a vector comparison result into a vector.
// Normally, vector compare results are a bitmask, so we need to do a
// predicated transfer to expand it.
// Note that clang seems to assume a vector lane should have 0xffffffff when the
// result is true when folding constants, so we use that value here to be
// consistent, even though that is not what a scalar compare would do.
//
SDValue NyuziTargetLowering::LowerSIGN_EXTEND_INREG(SDValue Op,
                                                    SelectionDAG &DAG) const {
  SDValue SetCcOp = Op.getOperand(0);
  if (SetCcOp.getOpcode() != ISD::SETCC)
    return SDValue();

  SDLoc DL(Op);
  Intrinsic::ID intrinsic = intrinsicForVectorCompare(
      cast<CondCodeSDNode>(SetCcOp.getOperand(2))->get(),
      SetCcOp.getOperand(0).getValueType().getSimpleVT().isFloatingPoint());

  SDValue FalseVal = DAG.getNode(NyuziISD::SPLAT, DL, MVT::v16i32,
                                 DAG.getConstant(0, DL, MVT::i32));
  SDValue TrueVal = DAG.getNode(NyuziISD::SPLAT, DL, MVT::v16i32,
                                DAG.getConstant(0xffffffff, DL, MVT::i32));
  SDValue Mask = DAG.getNode(ISD::INTRINSIC_WO_CHAIN, DL, MVT::i32,
                             DAG.getConstant(intrinsic, DL, MVT::i32),
                             SetCcOp.getOperand(0), SetCcOp.getOperand(1));
  return DAG.getNode(
      ISD::INTRINSIC_WO_CHAIN, DL, MVT::v16i32,
      DAG.getConstant(Intrinsic::nyuzi_vector_mixi, DL, MVT::i32), Mask,
      TrueVal, FalseVal);
}

SDValue NyuziTargetLowering::LowerOperation(SDValue Op,
                                            SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  case ISD::VECTOR_SHUFFLE:
    return LowerVECTOR_SHUFFLE(Op, DAG);
  case ISD::BUILD_VECTOR:
    return LowerBUILD_VECTOR(Op, DAG);
  case ISD::INSERT_VECTOR_ELT:
    return LowerINSERT_VECTOR_ELT(Op, DAG);
  case ISD::SCALAR_TO_VECTOR:
    return LowerSCALAR_TO_VECTOR(Op, DAG);
  case ISD::GlobalAddress:
    return LowerGlobalAddress(Op, DAG);
  case ISD::BlockAddress:
    return LowerBlockAddress(Op, DAG);
  case ISD::ConstantPool:
    return LowerConstantPool(Op, DAG);
  case ISD::Constant:
    return LowerConstant(Op, DAG);
  case ISD::SELECT_CC:
    return LowerSELECT_CC(Op, DAG);
  case ISD::FDIV:
    return LowerFDIV(Op, DAG);
  case ISD::BR_JT:
    return LowerBR_JT(Op, DAG);
  case ISD::FNEG:
    return LowerFNEG(Op, DAG);
  case ISD::SETCC:
    return LowerSETCC(Op, DAG);
  case ISD::CTLZ_ZERO_UNDEF:
    return LowerCTLZ_ZERO_UNDEF(Op, DAG);
  case ISD::CTTZ_ZERO_UNDEF:
    return LowerCTTZ_ZERO_UNDEF(Op, DAG);
  case ISD::UINT_TO_FP:
    return LowerUINT_TO_FP(Op, DAG);
  case ISD::FRAMEADDR:
    return LowerFRAMEADDR(Op, DAG);
  case ISD::RETURNADDR:
    return LowerRETURNADDR(Op, DAG);
  case ISD::SIGN_EXTEND_INREG:
    return LowerSIGN_EXTEND_INREG(Op, DAG);
  case ISD::VASTART:
    return LowerVASTART(Op, DAG);
  case ISD::FABS:
    return LowerFABS(Op, DAG);
  default:
    llvm_unreachable("Should not custom lower this!");
  }
}

EVT NyuziTargetLowering::getSetCCResultType(const DataLayout &,
                                            LLVMContext &Context,
                                            EVT VT) const {
  if (!VT.isVector())
    return MVT::i32;

  return VT.changeVectorElementTypeToInteger();
}

MachineBasicBlock *
NyuziTargetLowering::EmitInstrWithCustomInserter(MachineInstr *MI,
                                                 MachineBasicBlock *BB) const {
  switch (MI->getOpcode()) {
  case Nyuzi::SELECTI:
  case Nyuzi::SELECTF:
  case Nyuzi::SELECTVI:
  case Nyuzi::SELECTVF:
    return EmitSelectCC(MI, BB);

  case Nyuzi::ATOMIC_LOAD_ADDR:
    return EmitAtomicBinary(MI, BB, Nyuzi::ADDISSS);

  case Nyuzi::ATOMIC_LOAD_ADDI:
    return EmitAtomicBinary(MI, BB, Nyuzi::ADDISSI);

  case Nyuzi::ATOMIC_LOAD_SUBR:
    return EmitAtomicBinary(MI, BB, Nyuzi::SUBISSS);

  case Nyuzi::ATOMIC_LOAD_SUBI:
    return EmitAtomicBinary(MI, BB, Nyuzi::SUBISSI);

  case Nyuzi::ATOMIC_LOAD_ANDR:
    return EmitAtomicBinary(MI, BB, Nyuzi::ANDSSS);

  case Nyuzi::ATOMIC_LOAD_ANDI:
    return EmitAtomicBinary(MI, BB, Nyuzi::ANDSSI);

  case Nyuzi::ATOMIC_LOAD_ORR:
    return EmitAtomicBinary(MI, BB, Nyuzi::ORSSS);

  case Nyuzi::ATOMIC_LOAD_ORI:
    return EmitAtomicBinary(MI, BB, Nyuzi::ORSSI);

  case Nyuzi::ATOMIC_LOAD_XORR:
    return EmitAtomicBinary(MI, BB, Nyuzi::XORSSS);

  case Nyuzi::ATOMIC_LOAD_XORI:
    return EmitAtomicBinary(MI, BB, Nyuzi::XORSSI);

  // XXX ATOMIC_LOAD_NAND is not supported

  case Nyuzi::ATOMIC_SWAP:
    return EmitAtomicBinary(MI, BB, 0);

  case Nyuzi::ATOMIC_CMP_SWAP:
    return EmitAtomicCmpSwap(MI, BB);

  default:
    llvm_unreachable("unknown atomic operation");
  }
}

MachineBasicBlock *
NyuziTargetLowering::EmitSelectCC(MachineInstr *MI,
                                  MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  DebugLoc DL = MI->getDebugLoc();

  // The instruction we are replacing is SELECTI (Dest, predicate, trueval,
  // falseval)

  // To "insert" a SELECT_CC instruction, we actually have to rewrite it into a
  // diamond control-flow pattern.  The incoming instruction knows the
  // destination vreg to set, the condition code register to branch on, the
  // true/false values to select between, and a branch opcode to use.
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator It = BB->getIterator();
  ++It;

  //  ThisMBB:
  //  ...
  //   TrueVal = ...
  //   setcc r1, r2, r3
  //   if r1 goto copy1MBB
  //   fallthrough --> Copy0MBB
  MachineBasicBlock *ThisMBB = BB;
  MachineFunction *F = BB->getParent();
  MachineBasicBlock *Copy0MBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *SinkMBB = F->CreateMachineBasicBlock(LLVM_BB);
  F->insert(It, Copy0MBB);
  F->insert(It, SinkMBB);

  // Transfer the remainder of BB and its successor edges to SinkMBB.
  SinkMBB->splice(SinkMBB->begin(), BB,
                  std::next(MachineBasicBlock::iterator(MI)), BB->end());
  SinkMBB->transferSuccessorsAndUpdatePHIs(BB);

  // Next, add the true and fallthrough blocks as its successors.
  BB->addSuccessor(Copy0MBB);
  BB->addSuccessor(SinkMBB);

  BuildMI(BB, DL, TII->get(Nyuzi::BTRUE))
      .addReg(MI->getOperand(1).getReg())
      .addMBB(SinkMBB);

  //  Copy0MBB:
  //   %FalseValue = ...
  //   # fallthrough to SinkMBB
  BB = Copy0MBB;

  // Update machine-CFG edges
  BB->addSuccessor(SinkMBB);

  //  SinkMBB:
  //   %Result = phi [ %TrueValue, ThisMBB ], [ %FalseValue, Copy0MBB ]
  //  ...
  BB = SinkMBB;

  BuildMI(*BB, BB->begin(), DL, TII->get(Nyuzi::PHI),
          MI->getOperand(0).getReg())
      .addReg(MI->getOperand(2).getReg())
      .addMBB(ThisMBB)
      .addReg(MI->getOperand(3).getReg())
      .addMBB(Copy0MBB);

  MI->eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}

MachineBasicBlock *
NyuziTargetLowering::EmitAtomicBinary(MachineInstr *MI, MachineBasicBlock *BB,
                                      unsigned Opcode) const {
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();

  unsigned Dest = MI->getOperand(0).getReg();
  unsigned Ptr = MI->getOperand(1).getReg();
  DebugLoc DL = MI->getDebugLoc();
  MachineRegisterInfo &MRI = BB->getParent()->getRegInfo();
  unsigned OldValue = MRI.createVirtualRegister(&Nyuzi::GPR32RegClass);
  unsigned Success = MRI.createVirtualRegister(&Nyuzi::GPR32RegClass);

  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction *MF = BB->getParent();
  MachineFunction::iterator It = BB->getIterator();
  ++It;

  MachineBasicBlock *LoopMBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *ExitMBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MF->insert(It, LoopMBB);
  MF->insert(It, ExitMBB);

  // Transfer the remainder of BB and its successor edges to ExitMBB.
  ExitMBB->splice(ExitMBB->begin(), BB,
                  std::next(MachineBasicBlock::iterator(MI)), BB->end());
  ExitMBB->transferSuccessorsAndUpdatePHIs(BB);

  //  ThisMBB:
  BB->addSuccessor(LoopMBB);

  //  LoopMBB:
  BB = LoopMBB;
  BuildMI(BB, DL, TII->get(Nyuzi::LOAD_SYNC), OldValue).addReg(Ptr).addImm(0);
  BuildMI(BB, DL, TII->get(Nyuzi::MOVESS), Dest).addReg(OldValue);

  unsigned NewValue;
  if (Opcode != 0) {
    // Perform an operation
    NewValue = MRI.createVirtualRegister(&Nyuzi::GPR32RegClass);
    if (MI->getOperand(2).getType() == MachineOperand::MO_Register)
      BuildMI(BB, DL, TII->get(Opcode), NewValue)
          .addReg(OldValue)
          .addReg(MI->getOperand(2).getReg());
    else if (MI->getOperand(2).getType() == MachineOperand::MO_Immediate)
      BuildMI(BB, DL, TII->get(Opcode), NewValue)
          .addReg(OldValue)
          .addImm(MI->getOperand(2).getImm());
    else
      llvm_unreachable("Unknown operand type");
  } else
    NewValue = OldValue; // This is just swap: use old value

  BuildMI(BB, DL, TII->get(Nyuzi::STORE_SYNC), Success)
      .addReg(NewValue)
      .addReg(Ptr)
      .addImm(0);
  BuildMI(BB, DL, TII->get(Nyuzi::BFALSE)).addReg(Success).addMBB(LoopMBB);
  BB->addSuccessor(LoopMBB);
  BB->addSuccessor(ExitMBB);

  //  ExitMBB:
  BB = ExitMBB;

  MI->eraseFromParent(); // The instruction is gone now.

  return BB;
}

MachineBasicBlock *
NyuziTargetLowering::EmitAtomicCmpSwap(MachineInstr *MI,
                                       MachineBasicBlock *BB) const {
  MachineFunction *MF = BB->getParent();
  MachineRegisterInfo &RegInfo = MF->getRegInfo();
  const TargetRegisterClass *RC = getRegClassFor(MVT::i32);
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  DebugLoc DL = MI->getDebugLoc();

  unsigned Dest = MI->getOperand(0).getReg();
  unsigned Ptr = MI->getOperand(1).getReg();
  unsigned OldVal = MI->getOperand(2).getReg();
  unsigned NewVal = MI->getOperand(3).getReg();

  unsigned Success = RegInfo.createVirtualRegister(RC);
  unsigned CmpResult = RegInfo.createVirtualRegister(RC);

  // insert new blocks after the current block
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineBasicBlock *Loop1MBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *Loop2MBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *ExitMBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineFunction::iterator It = BB->getIterator();
  ++It;
  MF->insert(It, Loop1MBB);
  MF->insert(It, Loop2MBB);
  MF->insert(It, ExitMBB);

  // Transfer the remainder of BB and its successor edges to ExitMBB.
  ExitMBB->splice(ExitMBB->begin(), BB,
                  std::next(MachineBasicBlock::iterator(MI)), BB->end());
  ExitMBB->transferSuccessorsAndUpdatePHIs(BB);

  //  ThisMBB:
  //    ...
  //    fallthrough --> Loop1MBB
  BB->addSuccessor(Loop1MBB);
  Loop1MBB->addSuccessor(ExitMBB);
  Loop1MBB->addSuccessor(Loop2MBB);
  Loop2MBB->addSuccessor(Loop1MBB);
  Loop2MBB->addSuccessor(ExitMBB);

  // Loop1MBB:
  //   load.sync Dest, 0(Ptr)
  //   setne cmpresult, Dest, oldval
  //   btrue cmpresult, ExitMBB
  BB = Loop1MBB;
  BuildMI(BB, DL, TII->get(Nyuzi::LOAD_SYNC), Dest).addReg(Ptr).addImm(0);
  BuildMI(BB, DL, TII->get(Nyuzi::SNESISS), CmpResult)
      .addReg(Dest)
      .addReg(OldVal);
  BuildMI(BB, DL, TII->get(Nyuzi::BTRUE)).addReg(CmpResult).addMBB(ExitMBB);

  // Loop2MBB:
  //   move success, newval			; need a temporary because
  //   store.sync success, 0(Ptr)	; store.sync will clobber success
  //   bfalse success, Loop1MBB
  BB = Loop2MBB;
  BuildMI(BB, DL, TII->get(Nyuzi::STORE_SYNC), Success)
      .addReg(NewVal)
      .addReg(Ptr)
      .addImm(0);
  BuildMI(BB, DL, TII->get(Nyuzi::BFALSE)).addReg(Success).addMBB(Loop1MBB);

  MI->eraseFromParent(); // The instruction is gone now.

  return ExitMBB;
}

//===----------------------------------------------------------------------===//
//                         Nyuzi Inline Assembly Support
//===----------------------------------------------------------------------===//

/// getConstraintType - Given a constraint letter, return the Type of
/// constraint it is for this target.
NyuziTargetLowering::ConstraintType
NyuziTargetLowering::getConstraintType(StringRef Constraint) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 's':
    case 'r':
    case 'v':
      return C_RegisterClass;
    default:
      break;
    }
  }

  return TargetLowering::getConstraintType(Constraint);
}

std::pair<unsigned, const TargetRegisterClass *>
NyuziTargetLowering::getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                                                  StringRef Constraint,
                                                  MVT VT) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 's':
    case 'r':
      return std::make_pair(0U, &Nyuzi::GPR32RegClass);

    case 'v':
      return std::make_pair(0U, &Nyuzi::VR512RegClass);
    }
  }

  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

bool NyuziTargetLowering::isOffsetFoldingLegal(
    const GlobalAddressSDNode *GA) const {
  // The Nyuzi target isn't yet aware of offsets.
  return false;
}

bool NyuziTargetLowering::isIntDivCheap(EVT, AttributeSet) const {
  return false;
}

bool NyuziTargetLowering::shouldInsertFencesForAtomic(
    const Instruction *I) const {
  return true;
}
