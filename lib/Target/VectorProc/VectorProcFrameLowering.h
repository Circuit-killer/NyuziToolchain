//===-- VectorProcFrameLowering.h - Define frame lowering for VectorProc --*-
//C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef VECTORPROCFRAMELOWERING_H
#define VECTORPROCFRAMELOWERING_H

#include "VectorProc.h"
#include "llvm/Target/TargetFrameLowering.h"

namespace llvm {
class VectorProcSubtarget;

class VectorProcFrameLowering : public TargetFrameLowering {
public:
  static const VectorProcFrameLowering *create(const VectorProcSubtarget &ST);

  explicit VectorProcFrameLowering(const VectorProcSubtarget &ST)
      : TargetFrameLowering(TargetFrameLowering::StackGrowsDown, 64, 0, 64) {}

  virtual void emitPrologue(MachineFunction &MF) const override;
  virtual void emitEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) const override;
  virtual void eliminateCallFramePseudoInstr(MachineFunction &MF,
                                     		 MachineBasicBlock &MBB,
                                    		 MachineBasicBlock::iterator I) const override;
  virtual void processFunctionBeforeCalleeSavedScan(MachineFunction &MF,
                                                    RegScavenger *RS) const override;
  virtual bool hasFP(const MachineFunction &MF) const override;
  virtual bool hasReservedCallFrame(const MachineFunction &MF) const override;

private:
  uint64_t getWorstCaseStackSize(const MachineFunction &MF) const;
};

} // End llvm namespace

#endif
