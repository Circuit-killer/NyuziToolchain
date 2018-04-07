//===- GCNIterativeScheduler.cpp ------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "GCNIterativeScheduler.h"
#include "AMDGPUSubtarget.h"
#include "GCNRegPressure.h"
#include "GCNSchedStrategy.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/RegisterPressure.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "machine-scheduler"

namespace llvm {

std::vector<const SUnit *> makeMinRegSchedule(ArrayRef<const SUnit *> TopRoots,
                                              const ScheduleDAG &DAG);

  std::vector<const SUnit*> makeGCNILPScheduler(ArrayRef<const SUnit*> BotRoots,
    const ScheduleDAG &DAG);
}

// shim accessors for different order containers
static inline MachineInstr *getMachineInstr(MachineInstr *MI) {
  return MI;
}
static inline MachineInstr *getMachineInstr(const SUnit *SU) {
  return SU->getInstr();
}
static inline MachineInstr *getMachineInstr(const SUnit &SU) {
  return SU.getInstr();
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD
static void printRegion(raw_ostream &OS,
                        MachineBasicBlock::iterator Begin,
                        MachineBasicBlock::iterator End,
                        const LiveIntervals *LIS,
                        unsigned MaxInstNum =
                          std::numeric_limits<unsigned>::max()) {
  auto BB = Begin->getParent();
  OS << BB->getParent()->getName() << ":" << printMBBReference(*BB) << ' '
     << BB->getName() << ":\n";
  auto I = Begin;
  MaxInstNum = std::max(MaxInstNum, 1u);
  for (; I != End && MaxInstNum; ++I, --MaxInstNum) {
    if (!I->isDebugValue() && LIS)
      OS << LIS->getInstructionIndex(*I);
    OS << '\t' << *I;
  }
  if (I != End) {
    OS << "\t...\n";
    I = std::prev(End);
    if (!I->isDebugValue() && LIS)
      OS << LIS->getInstructionIndex(*I);
    OS << '\t' << *I;
  }
  if (End != BB->end()) { // print boundary inst if present
    OS << "----\n";
    if (LIS) OS << LIS->getInstructionIndex(*End) << '\t';
    OS << *End;
  }
}

LLVM_DUMP_METHOD
static void printLivenessInfo(raw_ostream &OS,
                              MachineBasicBlock::iterator Begin,
                              MachineBasicBlock::iterator End,
                              const LiveIntervals *LIS) {
  const auto BB = Begin->getParent();
  const auto &MRI = BB->getParent()->getRegInfo();

  const auto LiveIns = getLiveRegsBefore(*Begin, *LIS);
  OS << "LIn RP: ";
  getRegPressure(MRI, LiveIns).print(OS);

  const auto BottomMI = End == BB->end() ? std::prev(End) : End;
  const auto LiveOuts = getLiveRegsAfter(*BottomMI, *LIS);
  OS << "LOt RP: ";
  getRegPressure(MRI, LiveOuts).print(OS);
}

LLVM_DUMP_METHOD
void GCNIterativeScheduler::printRegions(raw_ostream &OS) const {
  const auto &ST = MF.getSubtarget<SISubtarget>();
  for (const auto R : Regions) {
    OS << "Region to schedule ";
    printRegion(OS, R->Begin, R->End, LIS, 1);
    printLivenessInfo(OS, R->Begin, R->End, LIS);
    OS << "Max RP: ";
    R->MaxPressure.print(OS, &ST);
  }
}

LLVM_DUMP_METHOD
void GCNIterativeScheduler::printSchedResult(raw_ostream &OS,
                                             const Region *R,
                                             const GCNRegPressure &RP) const {
  OS << "\nAfter scheduling ";
  printRegion(OS, R->Begin, R->End, LIS);
  printSchedRP(OS, R->MaxPressure, RP);
  OS << '\n';
}

LLVM_DUMP_METHOD
void GCNIterativeScheduler::printSchedRP(raw_ostream &OS,
                                         const GCNRegPressure &Before,
                                         const GCNRegPressure &After) const {
  const auto &ST = MF.getSubtarget<SISubtarget>();
  OS << "RP before: ";
  Before.print(OS, &ST);
  OS << "RP after:  ";
  After.print(OS, &ST);
}
#endif

// DAG builder helper
class GCNIterativeScheduler::BuildDAG {
  GCNIterativeScheduler &Sch;
  SmallVector<SUnit *, 8> TopRoots;

  SmallVector<SUnit*, 8> BotRoots;
public:
  BuildDAG(const Region &R, GCNIterativeScheduler &_Sch)
    : Sch(_Sch) {
    auto BB = R.Begin->getParent();
    Sch.BaseClass::startBlock(BB);
    Sch.BaseClass::enterRegion(BB, R.Begin, R.End, R.NumRegionInstrs);

    Sch.buildSchedGraph(Sch.AA, nullptr, nullptr, nullptr,
                        /*TrackLaneMask*/true);
    Sch.Topo.InitDAGTopologicalSorting();
    Sch.findRootsAndBiasEdges(TopRoots, BotRoots);
  }

  ~BuildDAG() {
    Sch.BaseClass::exitRegion();
    Sch.BaseClass::finishBlock();
  }

  ArrayRef<const SUnit *> getTopRoots() const {
    return TopRoots;
  }
  ArrayRef<SUnit*> getBottomRoots() const {
    return BotRoots;
  }
};

class GCNIterativeScheduler::OverrideLegacyStrategy {
  GCNIterativeScheduler &Sch;
  Region &Rgn;
  std::unique_ptr<MachineSchedStrategy> SaveSchedImpl;
  GCNRegPressure SaveMaxRP;

public:
  OverrideLegacyStrategy(Region &R,
                         MachineSchedStrategy &OverrideStrategy,
                         GCNIterativeScheduler &_Sch)
    : Sch(_Sch)
    , Rgn(R)
    , SaveSchedImpl(std::move(_Sch.SchedImpl))
    , SaveMaxRP(R.MaxPressure) {
    Sch.SchedImpl.reset(&OverrideStrategy);
    auto BB = R.Begin->getParent();
    Sch.BaseClass::startBlock(BB);
    Sch.BaseClass::enterRegion(BB, R.Begin, R.End, R.NumRegionInstrs);
  }

  ~OverrideLegacyStrategy() {
    Sch.BaseClass::exitRegion();
    Sch.BaseClass::finishBlock();
    Sch.SchedImpl.release();
    Sch.SchedImpl = std::move(SaveSchedImpl);
  }

  void schedule() {
    assert(Sch.RegionBegin == Rgn.Begin && Sch.RegionEnd == Rgn.End);
    DEBUG(dbgs() << "\nScheduling ";
      printRegion(dbgs(), Rgn.Begin, Rgn.End, Sch.LIS, 2));
    Sch.BaseClass::schedule();

    // Unfortunatelly placeDebugValues incorrectly modifies RegionEnd, restore
    Sch.RegionEnd = Rgn.End;
    //assert(Rgn.End == Sch.RegionEnd);
    Rgn.Begin = Sch.RegionBegin;
    Rgn.MaxPressure.clear();
  }

  void restoreOrder() {
    assert(Sch.RegionBegin == Rgn.Begin && Sch.RegionEnd == Rgn.End);
    // DAG SUnits are stored using original region's order
    // so just use SUnits as the restoring schedule
    Sch.scheduleRegion(Rgn, Sch.SUnits, SaveMaxRP);
  }
};

namespace {

// just a stub to make base class happy
class SchedStrategyStub : public MachineSchedStrategy {
public:
  bool shouldTrackPressure() const override { return false; }
  bool shouldTrackLaneMasks() const override { return false; }
  void initialize(ScheduleDAGMI *DAG) override {}
  SUnit *pickNode(bool &IsTopNode) override { return nullptr; }
  void schedNode(SUnit *SU, bool IsTopNode) override {}
  void releaseTopNode(SUnit *SU) override {}
  void releaseBottomNode(SUnit *SU) override {}
};

} // end anonymous namespace

GCNIterativeScheduler::GCNIterativeScheduler(MachineSchedContext *C,
                                             StrategyKind S)
  : BaseClass(C, llvm::make_unique<SchedStrategyStub>())
  , Context(C)
  , Strategy(S)
  , UPTracker(*LIS) {
}

// returns max pressure for a region
GCNRegPressure
GCNIterativeScheduler::getRegionPressure(MachineBasicBlock::iterator Begin,
                                         MachineBasicBlock::iterator End)
  const {
  // For the purpose of pressure tracking bottom inst of the region should
  // be also processed. End is either BB end, BB terminator inst or sched
  // boundary inst.
  auto const BBEnd = Begin->getParent()->end();
  auto const BottomMI = End == BBEnd ? std::prev(End) : End;

  // scheduleRegions walks bottom to top, so its likely we just get next
  // instruction to track
  auto AfterBottomMI = std::next(BottomMI);
  if (AfterBottomMI == BBEnd ||
      &*AfterBottomMI != UPTracker.getLastTrackedMI()) {
    UPTracker.reset(*BottomMI);
  } else {
    assert(UPTracker.isValid());
  }

  for (auto I = BottomMI; I != Begin; --I)
    UPTracker.recede(*I);

  UPTracker.recede(*Begin);

  assert(UPTracker.isValid() ||
         (dbgs() << "Tracked region ",
          printRegion(dbgs(), Begin, End, LIS), false));
  return UPTracker.moveMaxPressure();
}

// returns max pressure for a tentative schedule
template <typename Range> GCNRegPressure
GCNIterativeScheduler::getSchedulePressure(const Region &R,
                                           Range &&Schedule) const {
  auto const BBEnd = R.Begin->getParent()->end();
  GCNUpwardRPTracker RPTracker(*LIS);
  if (R.End != BBEnd) {
    // R.End points to the boundary instruction but the
    // schedule doesn't include it
    RPTracker.reset(*R.End);
    RPTracker.recede(*R.End);
  } else {
    // R.End doesn't point to the boundary instruction
    RPTracker.reset(*std::prev(BBEnd));
  }
  for (auto I = Schedule.end(), B = Schedule.begin(); I != B;) {
    RPTracker.recede(*getMachineInstr(*--I));
  }
  return RPTracker.moveMaxPressure();
}

void GCNIterativeScheduler::enterRegion(MachineBasicBlock *BB, // overriden
                                        MachineBasicBlock::iterator Begin,
                                        MachineBasicBlock::iterator End,
                                        unsigned NumRegionInstrs) {
  BaseClass::enterRegion(BB, Begin, End, NumRegionInstrs);
  if (NumRegionInstrs > 2) {
    Regions.push_back(
      new (Alloc.Allocate())
      Region { Begin, End, NumRegionInstrs,
               getRegionPressure(Begin, End), nullptr });
  }
}

void GCNIterativeScheduler::schedule() { // overriden
  // do nothing
  DEBUG(
    printLivenessInfo(dbgs(), RegionBegin, RegionEnd, LIS);
    if (!Regions.empty() && Regions.back()->Begin == RegionBegin) {
      dbgs() << "Max RP: ";
      Regions.back()->MaxPressure.print(dbgs(), &MF.getSubtarget<SISubtarget>());
    }
    dbgs() << '\n';
  );
}

void GCNIterativeScheduler::finalizeSchedule() { // overriden
  if (Regions.empty())
    return;
  switch (Strategy) {
  case SCHEDULE_MINREGONLY: scheduleMinReg(); break;
  case SCHEDULE_MINREGFORCED: scheduleMinReg(true); break;
  case SCHEDULE_LEGACYMAXOCCUPANCY: scheduleLegacyMaxOccupancy(); break;
  case SCHEDULE_ILP: scheduleILP(false); break;
  }
}

// Detach schedule from SUnits and interleave it with debug values.
// Returned schedule becomes independent of DAG state.
std::vector<MachineInstr*>
GCNIterativeScheduler::detachSchedule(ScheduleRef Schedule) const {
  std::vector<MachineInstr*> Res;
  Res.reserve(Schedule.size() * 2);

  if (FirstDbgValue)
    Res.push_back(FirstDbgValue);

  const auto DbgB = DbgValues.begin(), DbgE = DbgValues.end();
  for (auto SU : Schedule) {
    Res.push_back(SU->getInstr());
    const auto &D = std::find_if(DbgB, DbgE, [SU](decltype(*DbgB) &P) {
      return P.second == SU->getInstr();
    });
    if (D != DbgE)
      Res.push_back(D->first);
  }
  return Res;
}

void GCNIterativeScheduler::setBestSchedule(Region &R,
                                            ScheduleRef Schedule,
                                            const GCNRegPressure &MaxRP) {
  R.BestSchedule.reset(
    new TentativeSchedule{ detachSchedule(Schedule), MaxRP });
}

void GCNIterativeScheduler::scheduleBest(Region &R) {
  assert(R.BestSchedule.get() && "No schedule specified");
  scheduleRegion(R, R.BestSchedule->Schedule, R.BestSchedule->MaxPressure);
  R.BestSchedule.reset();
}

// minimal required region scheduler, works for ranges of SUnits*,
// SUnits or MachineIntrs*
template <typename Range>
void GCNIterativeScheduler::scheduleRegion(Region &R, Range &&Schedule,
                                           const GCNRegPressure &MaxRP) {
  assert(RegionBegin == R.Begin && RegionEnd == R.End);
  assert(LIS != nullptr);
#ifndef NDEBUG
  const auto SchedMaxRP = getSchedulePressure(R, Schedule);
#endif
  auto BB = R.Begin->getParent();
  auto Top = R.Begin;
  for (const auto &I : Schedule) {
    auto MI = getMachineInstr(I);
    if (MI != &*Top) {
      BB->remove(MI);
      BB->insert(Top, MI);
      if (!MI->isDebugValue())
        LIS->handleMove(*MI, true);
    }
    if (!MI->isDebugValue()) {
      // Reset read - undef flags and update them later.
      for (auto &Op : MI->operands())
        if (Op.isReg() && Op.isDef())
          Op.setIsUndef(false);

      RegisterOperands RegOpers;
      RegOpers.collect(*MI, *TRI, MRI, /*ShouldTrackLaneMasks*/true,
                                       /*IgnoreDead*/false);
      // Adjust liveness and add missing dead+read-undef flags.
      auto SlotIdx = LIS->getInstructionIndex(*MI).getRegSlot();
      RegOpers.adjustLaneLiveness(*LIS, MRI, SlotIdx, MI);
    }
    Top = std::next(MI->getIterator());
  }
  RegionBegin = getMachineInstr(Schedule.front());

  // Schedule consisting of MachineInstr* is considered 'detached'
  // and already interleaved with debug values
  if (!std::is_same<decltype(*Schedule.begin()), MachineInstr*>::value) {
    placeDebugValues();
    // Unfortunatelly placeDebugValues incorrectly modifies RegionEnd, restore
    //assert(R.End == RegionEnd);
    RegionEnd = R.End;
  }

  R.Begin = RegionBegin;
  R.MaxPressure = MaxRP;

#ifndef NDEBUG
  const auto RegionMaxRP = getRegionPressure(R);
  const auto &ST = MF.getSubtarget<SISubtarget>();
#endif
  assert((SchedMaxRP == RegionMaxRP && (MaxRP.empty() || SchedMaxRP == MaxRP))
  || (dbgs() << "Max RP mismatch!!!\n"
                "RP for schedule (calculated): ",
      SchedMaxRP.print(dbgs(), &ST),
      dbgs() << "RP for schedule (reported): ",
      MaxRP.print(dbgs(), &ST),
      dbgs() << "RP after scheduling: ",
      RegionMaxRP.print(dbgs(), &ST),
      false));
}

// Sort recorded regions by pressure - highest at the front
void GCNIterativeScheduler::sortRegionsByPressure(unsigned TargetOcc) {
  const auto &ST = MF.getSubtarget<SISubtarget>();
  llvm::sort(Regions.begin(), Regions.end(),
    [&ST, TargetOcc](const Region *R1, const Region *R2) {
    return R2->MaxPressure.less(ST, R1->MaxPressure, TargetOcc);
  });
}

///////////////////////////////////////////////////////////////////////////////
// Legacy MaxOccupancy Strategy

// Tries to increase occupancy applying minreg scheduler for a sequence of
// most demanding regions. Obtained schedules are saved as BestSchedule for a
// region.
// TargetOcc is the best achievable occupancy for a kernel.
// Returns better occupancy on success or current occupancy on fail.
// BestSchedules aren't deleted on fail.
unsigned GCNIterativeScheduler::tryMaximizeOccupancy(unsigned TargetOcc) {
  // TODO: assert Regions are sorted descending by pressure
  const auto &ST = MF.getSubtarget<SISubtarget>();
  const auto Occ = Regions.front()->MaxPressure.getOccupancy(ST);
  DEBUG(dbgs() << "Trying to improve occupancy, target = " << TargetOcc
               << ", current = " << Occ << '\n');

  auto NewOcc = TargetOcc;
  for (auto R : Regions) {
    if (R->MaxPressure.getOccupancy(ST) >= NewOcc)
      break;

    DEBUG(printRegion(dbgs(), R->Begin, R->End, LIS, 3);
          printLivenessInfo(dbgs(), R->Begin, R->End, LIS));

    BuildDAG DAG(*R, *this);
    const auto MinSchedule = makeMinRegSchedule(DAG.getTopRoots(), *this);
    const auto MaxRP = getSchedulePressure(*R, MinSchedule);
    DEBUG(dbgs() << "Occupancy improvement attempt:\n";
          printSchedRP(dbgs(), R->MaxPressure, MaxRP));

    NewOcc = std::min(NewOcc, MaxRP.getOccupancy(ST));
    if (NewOcc <= Occ)
      break;

    setBestSchedule(*R, MinSchedule, MaxRP);
  }
  DEBUG(dbgs() << "New occupancy = " << NewOcc
               << ", prev occupancy = " << Occ << '\n');
  return std::max(NewOcc, Occ);
}

void GCNIterativeScheduler::scheduleLegacyMaxOccupancy(
  bool TryMaximizeOccupancy) {
  const auto &ST = MF.getSubtarget<SISubtarget>();
  auto TgtOcc = ST.getOccupancyWithLocalMemSize(MF);

  sortRegionsByPressure(TgtOcc);
  auto Occ = Regions.front()->MaxPressure.getOccupancy(ST);

  if (TryMaximizeOccupancy && Occ < TgtOcc)
    Occ = tryMaximizeOccupancy(TgtOcc);

  // This is really weird but for some magic scheduling regions twice
  // gives performance improvement
  const int NumPasses = Occ < TgtOcc ? 2 : 1;

  TgtOcc = std::min(Occ, TgtOcc);
  DEBUG(dbgs() << "Scheduling using default scheduler, "
                  "target occupancy = " << TgtOcc << '\n');
  GCNMaxOccupancySchedStrategy LStrgy(Context);

  for (int I = 0; I < NumPasses; ++I) {
    // running first pass with TargetOccupancy = 0 mimics previous scheduling
    // approach and is a performance magic
    LStrgy.setTargetOccupancy(I == 0 ? 0 : TgtOcc);
    for (auto R : Regions) {
      OverrideLegacyStrategy Ovr(*R, LStrgy, *this);

      Ovr.schedule();
      const auto RP = getRegionPressure(*R);
      DEBUG(printSchedRP(dbgs(), R->MaxPressure, RP));

      if (RP.getOccupancy(ST) < TgtOcc) {
        DEBUG(dbgs() << "Didn't fit into target occupancy O" << TgtOcc);
        if (R->BestSchedule.get() &&
            R->BestSchedule->MaxPressure.getOccupancy(ST) >= TgtOcc) {
          DEBUG(dbgs() << ", scheduling minimal register\n");
          scheduleBest(*R);
        } else {
          DEBUG(dbgs() << ", restoring\n");
          Ovr.restoreOrder();
          assert(R->MaxPressure.getOccupancy(ST) >= TgtOcc);
        }
      }
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// Minimal Register Strategy

void GCNIterativeScheduler::scheduleMinReg(bool force) {
  const auto &ST = MF.getSubtarget<SISubtarget>();
  const auto TgtOcc = ST.getOccupancyWithLocalMemSize(MF);
  sortRegionsByPressure(TgtOcc);

  auto MaxPressure = Regions.front()->MaxPressure;
  for (auto R : Regions) {
    if (!force && R->MaxPressure.less(ST, MaxPressure, TgtOcc))
      break;

    BuildDAG DAG(*R, *this);
    const auto MinSchedule = makeMinRegSchedule(DAG.getTopRoots(), *this);

    const auto RP = getSchedulePressure(*R, MinSchedule);
    DEBUG(if (R->MaxPressure.less(ST, RP, TgtOcc)) {
      dbgs() << "\nWarning: Pressure becomes worse after minreg!";
      printSchedRP(dbgs(), R->MaxPressure, RP);
    });

    if (!force && MaxPressure.less(ST, RP, TgtOcc))
      break;

    scheduleRegion(*R, MinSchedule, RP);
    DEBUG(printSchedResult(dbgs(), R, RP));

    MaxPressure = RP;
  }
}

///////////////////////////////////////////////////////////////////////////////
// ILP scheduler port

void GCNIterativeScheduler::scheduleILP(
  bool TryMaximizeOccupancy) {
  const auto &ST = MF.getSubtarget<SISubtarget>();
  auto TgtOcc = std::min(ST.getOccupancyWithLocalMemSize(MF),
                         ST.getWavesPerEU(MF.getFunction()).second);

  sortRegionsByPressure(TgtOcc);
  auto Occ = Regions.front()->MaxPressure.getOccupancy(ST);

  if (TryMaximizeOccupancy && Occ < TgtOcc)
    Occ = tryMaximizeOccupancy(TgtOcc);

  TgtOcc = std::min(Occ, TgtOcc);
  DEBUG(dbgs() << "Scheduling using default scheduler, "
    "target occupancy = " << TgtOcc << '\n');

  for (auto R : Regions) {
    BuildDAG DAG(*R, *this);
    const auto ILPSchedule = makeGCNILPScheduler(DAG.getBottomRoots(), *this);

    const auto RP = getSchedulePressure(*R, ILPSchedule);
    DEBUG(printSchedRP(dbgs(), R->MaxPressure, RP));

    if (RP.getOccupancy(ST) < TgtOcc) {
      DEBUG(dbgs() << "Didn't fit into target occupancy O" << TgtOcc);
      if (R->BestSchedule.get() &&
        R->BestSchedule->MaxPressure.getOccupancy(ST) >= TgtOcc) {
        DEBUG(dbgs() << ", scheduling minimal register\n");
        scheduleBest(*R);
      }
    } else {
      scheduleRegion(*R, ILPSchedule, RP);
      DEBUG(printSchedResult(dbgs(), R, RP));
    }
  }
}
