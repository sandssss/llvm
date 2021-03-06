//===-- RegisterPressure.cpp - Dynamic Register Pressure ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the RegisterPressure class which can be used to track
// MachineInstr level register pressure.
//
//===----------------------------------------------------------------------===//

#include "RegisterClassInfo.h"
#include "RegisterPressure.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

/// Increase register pressure for each set impacted by this register class.
static void increaseSetPressure(std::vector<unsigned> &CurrSetPressure,
                                std::vector<unsigned> &MaxSetPressure,
                                const TargetRegisterClass *RC,
                                const TargetRegisterInfo *TRI) {
  unsigned Weight = TRI->getRegClassWeight(RC).RegWeight;
  for (const int *PSet = TRI->getRegClassPressureSets(RC);
       *PSet != -1; ++PSet) {
    CurrSetPressure[*PSet] += Weight;
    if (CurrSetPressure[*PSet] > MaxSetPressure[*PSet])
      MaxSetPressure[*PSet] = CurrSetPressure[*PSet];
  }
}

/// Decrease register pressure for each set impacted by this register class.
static void decreaseSetPressure(std::vector<unsigned> &CurrSetPressure,
                                const TargetRegisterClass *RC,
                                const TargetRegisterInfo *TRI) {
  unsigned Weight = TRI->getRegClassWeight(RC).RegWeight;
  for (const int *PSet = TRI->getRegClassPressureSets(RC);
       *PSet != -1; ++PSet) {
    assert(CurrSetPressure[*PSet] >= Weight && "register pressure underflow");
    CurrSetPressure[*PSet] -= Weight;
  }
}

/// Directly increase pressure only within this RegisterPressure result.
void RegisterPressure::increase(const TargetRegisterClass *RC,
                                const TargetRegisterInfo *TRI) {
  increaseSetPressure(MaxSetPressure, MaxSetPressure, RC, TRI);
}

/// Directly decrease pressure only within this RegisterPressure result.
void RegisterPressure::decrease(const TargetRegisterClass *RC,
                                const TargetRegisterInfo *TRI) {
  decreaseSetPressure(MaxSetPressure, RC, TRI);
}

/// Increase the current pressure as impacted by these physical registers and
/// bump the high water mark if needed.
void RegPressureTracker::increasePhysRegPressure(ArrayRef<unsigned> Regs) {
  for (unsigned I = 0, E = Regs.size(); I != E; ++I)
    increaseSetPressure(CurrSetPressure, P.MaxSetPressure,
                        TRI->getMinimalPhysRegClass(Regs[I]), TRI);
}

/// Simply decrease the current pressure as impacted by these physcial
/// registers.
void RegPressureTracker::decreasePhysRegPressure(ArrayRef<unsigned> Regs) {
  for (unsigned I = 0, E = Regs.size(); I != E; ++I)
    decreaseSetPressure(CurrSetPressure, TRI->getMinimalPhysRegClass(Regs[I]),
                        TRI);
}

/// Increase the current pressure as impacted by these virtual registers and
/// bump the high water mark if needed.
void RegPressureTracker::increaseVirtRegPressure(ArrayRef<unsigned> Regs) {
  for (unsigned I = 0, E = Regs.size(); I != E; ++I)
    increaseSetPressure(CurrSetPressure, P.MaxSetPressure,
                        MRI->getRegClass(Regs[I]), TRI);
}

/// Simply decrease the current pressure as impacted by these virtual registers.
void RegPressureTracker::decreaseVirtRegPressure(ArrayRef<unsigned> Regs) {
  for (unsigned I = 0, E = Regs.size(); I != E; ++I)
    decreaseSetPressure(CurrSetPressure, MRI->getRegClass(Regs[I]), TRI);
}

/// Clear the result so it can be used for another round of pressure tracking.
void IntervalPressure::reset() {
  TopIdx = BottomIdx = SlotIndex();
  MaxSetPressure.clear();
  LiveInRegs.clear();
  LiveOutRegs.clear();
}

/// Clear the result so it can be used for another round of pressure tracking.
void RegionPressure::reset() {
  TopPos = BottomPos = MachineBasicBlock::const_iterator();
  MaxSetPressure.clear();
  LiveInRegs.clear();
  LiveOutRegs.clear();
}

/// If the current top is not less than or equal to the next index, open it.
/// We happen to need the SlotIndex for the next top for pressure update.
void IntervalPressure::openTop(SlotIndex NextTop) {
  if (TopIdx <= NextTop)
    return;
  TopIdx = SlotIndex();
  LiveInRegs.clear();
}

/// If the current top is the previous instruction (before receding), open it.
void RegionPressure::openTop(MachineBasicBlock::const_iterator PrevTop) {
  if (TopPos != PrevTop)
    return;
  TopPos = MachineBasicBlock::const_iterator();
  LiveInRegs.clear();
}

/// If the current bottom is not greater than the previous index, open it.
void IntervalPressure::openBottom(SlotIndex PrevBottom) {
  if (BottomIdx > PrevBottom)
    return;
  BottomIdx = SlotIndex();
  LiveInRegs.clear();
}

/// If the current bottom is the previous instr (before advancing), open it.
void RegionPressure::openBottom(MachineBasicBlock::const_iterator PrevBottom) {
  if (BottomPos != PrevBottom)
    return;
  BottomPos = MachineBasicBlock::const_iterator();
  LiveInRegs.clear();
}

/// Setup the RegPressureTracker.
///
/// TODO: Add support for pressure without LiveIntervals.
void RegPressureTracker::init(const MachineFunction *mf,
                              const RegisterClassInfo *rci,
                              const LiveIntervals *lis,
                              const MachineBasicBlock *mbb,
                              MachineBasicBlock::const_iterator pos)
{
  MF = mf;
  TRI = MF->getTarget().getRegisterInfo();
  RCI = rci;
  MRI = &MF->getRegInfo();
  MBB = mbb;

  if (RequireIntervals) {
    assert(lis && "IntervalPressure requires LiveIntervals");
    LIS = lis;
  }

  CurrPos = pos;
  while (CurrPos != MBB->end() && CurrPos->isDebugValue())
    ++CurrPos;

  CurrSetPressure.assign(TRI->getNumRegPressureSets(), 0);

  if (RequireIntervals)
    static_cast<IntervalPressure&>(P).reset();
  else
    static_cast<RegionPressure&>(P).reset();
  P.MaxSetPressure = CurrSetPressure;

  LivePhysRegs.clear();
  LivePhysRegs.setUniverse(TRI->getNumRegs());
  LiveVirtRegs.clear();
  LiveVirtRegs.setUniverse(MRI->getNumVirtRegs());
}

/// Does this pressure result have a valid top position and live ins.
bool RegPressureTracker::isTopClosed() const {
  if (RequireIntervals)
    return static_cast<IntervalPressure&>(P).TopIdx.isValid();
  return (static_cast<RegionPressure&>(P).TopPos ==
          MachineBasicBlock::const_iterator());
}

/// Does this pressure result have a valid bottom position and live outs.
bool RegPressureTracker::isBottomClosed() const {
  if (RequireIntervals)
    return static_cast<IntervalPressure&>(P).BottomIdx.isValid();
  return (static_cast<RegionPressure&>(P).BottomPos ==
          MachineBasicBlock::const_iterator());
}

/// Set the boundary for the top of the region and summarize live ins.
void RegPressureTracker::closeTop() {
  if (RequireIntervals)
    static_cast<IntervalPressure&>(P).TopIdx =
      LIS->getInstructionIndex(CurrPos).getRegSlot();
  else
    static_cast<RegionPressure&>(P).TopPos = CurrPos;

  assert(P.LiveInRegs.empty() && "inconsistent max pressure result");
  P.LiveInRegs.reserve(LivePhysRegs.size() + LiveVirtRegs.size());
  P.LiveInRegs.append(LivePhysRegs.begin(), LivePhysRegs.end());
  for (SparseSet<unsigned>::const_iterator I =
         LiveVirtRegs.begin(), E = LiveVirtRegs.end(); I != E; ++I)
    P.LiveInRegs.push_back(*I);
  std::sort(P.LiveInRegs.begin(), P.LiveInRegs.end());
  P.LiveInRegs.erase(std::unique(P.LiveInRegs.begin(), P.LiveInRegs.end()),
                     P.LiveInRegs.end());
}

/// Set the boundary for the bottom of the region and summarize live outs.
void RegPressureTracker::closeBottom() {
  if (RequireIntervals)
    if (CurrPos == MBB->end())
      static_cast<IntervalPressure&>(P).BottomIdx = LIS->getMBBEndIdx(MBB);
    else
      static_cast<IntervalPressure&>(P).BottomIdx =
        LIS->getInstructionIndex(CurrPos).getRegSlot();
  else
    static_cast<RegionPressure&>(P).BottomPos = CurrPos;

  assert(P.LiveOutRegs.empty() && "inconsistent max pressure result");
  P.LiveOutRegs.reserve(LivePhysRegs.size() + LiveVirtRegs.size());
  P.LiveOutRegs.append(LivePhysRegs.begin(), LivePhysRegs.end());
  for (SparseSet<unsigned>::const_iterator I =
         LiveVirtRegs.begin(), E = LiveVirtRegs.end(); I != E; ++I)
    P.LiveOutRegs.push_back(*I);
  std::sort(P.LiveOutRegs.begin(), P.LiveOutRegs.end());
  P.LiveOutRegs.erase(std::unique(P.LiveOutRegs.begin(), P.LiveOutRegs.end()),
                      P.LiveOutRegs.end());
}

/// Finalize the region boundaries and record live ins and live outs.
void RegPressureTracker::closeRegion() {
  if (!isTopClosed() && !isBottomClosed()) {
    assert(LivePhysRegs.empty() && LiveVirtRegs.empty() &&
           "no region boundary");
    return;
  }
  if (!isBottomClosed())
    closeBottom();
  else if (!isTopClosed())
    closeTop();
  // If both top and bottom are closed, do nothing.
}

/// Return true if Reg aliases a register in Regs SparseSet.
static bool hasRegAlias(unsigned Reg, SparseSet<unsigned> &Regs,
                        const TargetRegisterInfo *TRI) {
  assert(!TargetRegisterInfo::isVirtualRegister(Reg) && "only for physregs");
  for (const uint16_t *Alias = TRI->getOverlaps(Reg); *Alias; ++Alias) {
    if (Regs.count(*Alias))
      return true;
  }
  return false;
}

/// Return true if Reg aliases a register in unsorted Regs SmallVector.
/// This is only valid for physical registers.
static SmallVectorImpl<unsigned>::iterator
findRegAlias(unsigned Reg, SmallVectorImpl<unsigned> &Regs,
             const TargetRegisterInfo *TRI) {
  for (const uint16_t *Alias = TRI->getOverlaps(Reg); *Alias; ++Alias) {
    SmallVectorImpl<unsigned>::iterator I =
      std::find(Regs.begin(), Regs.end(), *Alias);
    if (I != Regs.end())
      return I;
  }
  return Regs.end();
}

/// Return true if Reg can be inserted into Regs SmallVector. For virtual
/// register, do a linear search. For physical registers check for aliases.
static SmallVectorImpl<unsigned>::iterator
findReg(unsigned Reg, bool isVReg, SmallVectorImpl<unsigned> &Regs,
        const TargetRegisterInfo *TRI) {
  if(isVReg)
    return std::find(Regs.begin(), Regs.end(), Reg);
  return findRegAlias(Reg, Regs, TRI);
}

/// Collect this instruction's unique uses and defs into SmallVectors for
/// processing defs and uses in order.
template<bool isVReg>
struct RegisterOperands {
  SmallVector<unsigned, 8> Uses;
  SmallVector<unsigned, 8> Defs;
  SmallVector<unsigned, 8> DeadDefs;

  /// Push this operand's register onto the correct vector.
  void collect(const MachineOperand &MO, const TargetRegisterInfo *TRI) {
    if (MO.readsReg()) {
      if (findReg(MO.getReg(), isVReg, Uses, TRI) == Uses.end())
      Uses.push_back(MO.getReg());
    }
    if (MO.isDef()) {
      if (MO.isDead()) {
        if (findReg(MO.getReg(), isVReg, DeadDefs, TRI) == DeadDefs.end())
          DeadDefs.push_back(MO.getReg());
      }
      else {
        if (findReg(MO.getReg(), isVReg, Defs, TRI) == Defs.end())
          Defs.push_back(MO.getReg());
      }
    }
  }
};
typedef RegisterOperands<false> PhysRegOperands;
typedef RegisterOperands<true> VirtRegOperands;

/// Collect physical and virtual register operands.
static void collectOperands(const MachineInstr *MI,
                            PhysRegOperands &PhysRegOpers,
                            VirtRegOperands &VirtRegOpers,
                            const TargetRegisterInfo *TRI,
                            const RegisterClassInfo *RCI) {
  for(ConstMIBundleOperands OperI(MI); OperI.isValid(); ++OperI) {
    const MachineOperand &MO = *OperI;
    if (!MO.isReg() || !MO.getReg())
      continue;

    if (TargetRegisterInfo::isVirtualRegister(MO.getReg()))
      VirtRegOpers.collect(MO, TRI);
    else if (RCI->isAllocatable(MO.getReg()))
      PhysRegOpers.collect(MO, TRI);
  }
  // Remove redundant physreg dead defs.
  for (unsigned i = PhysRegOpers.DeadDefs.size(); i > 0; --i) {
    unsigned Reg = PhysRegOpers.DeadDefs[i-1];
    if (findRegAlias(Reg, PhysRegOpers.Defs, TRI) != PhysRegOpers.Defs.end())
      PhysRegOpers.DeadDefs.erase(&PhysRegOpers.DeadDefs[i-1]);
  }
}

/// Add PhysReg to the live in set and increase max pressure.
void RegPressureTracker::discoverPhysLiveIn(unsigned Reg) {
  assert(!LivePhysRegs.count(Reg) && "avoid bumping max pressure twice");
  if (findRegAlias(Reg, P.LiveInRegs, TRI) == P.LiveInRegs.end())
    return;

  // At live in discovery, unconditionally increase the high water mark.
  P.LiveInRegs.push_back(Reg);
  P.increase(TRI->getMinimalPhysRegClass(Reg), TRI);
}

/// Add PhysReg to the live out set and increase max pressure.
void RegPressureTracker::discoverPhysLiveOut(unsigned Reg) {
  assert(!LivePhysRegs.count(Reg) && "avoid bumping max pressure twice");
  if (findRegAlias(Reg, P.LiveOutRegs, TRI) == P.LiveOutRegs.end())
    return;

  // At live out discovery, unconditionally increase the high water mark.
  P.LiveOutRegs.push_back(Reg);
  P.increase(TRI->getMinimalPhysRegClass(Reg), TRI);
}

/// Add VirtReg to the live in set and increase max pressure.
void RegPressureTracker::discoverVirtLiveIn(unsigned Reg) {
  assert(!LiveVirtRegs.count(Reg) && "avoid bumping max pressure twice");
  if (std::find(P.LiveInRegs.begin(), P.LiveInRegs.end(), Reg) !=
      P.LiveInRegs.end())
    return;

  // At live in discovery, unconditionally increase the high water mark.
  P.LiveInRegs.push_back(Reg);
  P.increase(MRI->getRegClass(Reg), TRI);
}

/// Add VirtReg to the live out set and increase max pressure.
void RegPressureTracker::discoverVirtLiveOut(unsigned Reg) {
  assert(!LiveVirtRegs.count(Reg) && "avoid bumping max pressure twice");
  if (std::find(P.LiveOutRegs.begin(), P.LiveOutRegs.end(), Reg) !=
      P.LiveOutRegs.end())
    return;

  // At live out discovery, unconditionally increase the high water mark.
  P.LiveOutRegs.push_back(Reg);
  P.increase(MRI->getRegClass(Reg), TRI);
}

/// Recede across the previous instruction.
bool RegPressureTracker::recede() {
  // Check for the top of the analyzable region.
  if (CurrPos == MBB->begin()) {
    closeRegion();
    return false;
  }
  if (!isBottomClosed())
    closeBottom();

  // Open the top of the region using block iterators.
  if (!RequireIntervals && isTopClosed())
    static_cast<RegionPressure&>(P).openTop(CurrPos);

  // Find the previous instruction.
  do
    --CurrPos;
  while (CurrPos != MBB->begin() && CurrPos->isDebugValue());

  if (CurrPos->isDebugValue()) {
    closeRegion();
    return false;
  }
  SlotIndex SlotIdx;
  if (RequireIntervals)
    SlotIdx = LIS->getInstructionIndex(CurrPos).getRegSlot();

  // Open the top of the region using slot indexes.
  if (RequireIntervals && isTopClosed())
    static_cast<IntervalPressure&>(P).openTop(SlotIdx);

  PhysRegOperands PhysRegOpers;
  VirtRegOperands VirtRegOpers;
  collectOperands(CurrPos, PhysRegOpers, VirtRegOpers, TRI, RCI);

  // Boost pressure for all dead defs together.
  increasePhysRegPressure(PhysRegOpers.DeadDefs);
  increaseVirtRegPressure(VirtRegOpers.DeadDefs);
  decreasePhysRegPressure(PhysRegOpers.DeadDefs);
  decreaseVirtRegPressure(VirtRegOpers.DeadDefs);

  // Kill liveness at live defs.
  // TODO: consider earlyclobbers?
  for (unsigned i = 0; i < PhysRegOpers.Defs.size(); ++i) {
    unsigned Reg = PhysRegOpers.Defs[i];
    if (LivePhysRegs.erase(Reg))
      decreasePhysRegPressure(Reg);
    else
      discoverPhysLiveOut(Reg);
  }
  for (unsigned i = 0; i < VirtRegOpers.Defs.size(); ++i) {
    unsigned Reg = VirtRegOpers.Defs[i];
    if (LiveVirtRegs.erase(Reg))
      decreaseVirtRegPressure(Reg);
    else
      discoverVirtLiveOut(Reg);
  }

  // Generate liveness for uses.
  for (unsigned i = 0; i < PhysRegOpers.Uses.size(); ++i) {
    unsigned Reg = PhysRegOpers.Uses[i];
    if (!hasRegAlias(Reg, LivePhysRegs, TRI)) {
      increasePhysRegPressure(Reg);
      LivePhysRegs.insert(Reg);
    }
  }
  for (unsigned i = 0; i < VirtRegOpers.Uses.size(); ++i) {
    unsigned Reg = VirtRegOpers.Uses[i];
    if (!LiveVirtRegs.count(Reg)) {
      // Adjust liveouts if LiveIntervals are available.
      if (RequireIntervals) {
        const LiveInterval *LI = &LIS->getInterval(Reg);
        if (!LI->killedAt(SlotIdx))
          discoverVirtLiveOut(Reg);
      }
      increaseVirtRegPressure(Reg);
      LiveVirtRegs.insert(Reg);
    }
  }
  return true;
}

/// Advance across the current instruction.
bool RegPressureTracker::advance() {
  // Check for the bottom of the analyzable region.
  if (CurrPos == MBB->end()) {
    closeRegion();
    return false;
  }
  if (!isTopClosed())
    closeTop();

  SlotIndex SlotIdx;
  if (RequireIntervals)
    SlotIdx = LIS->getInstructionIndex(CurrPos).getRegSlot();

  // Open the bottom of the region using slot indexes.
  if (isBottomClosed()) {
    if (RequireIntervals)
      static_cast<IntervalPressure&>(P).openBottom(SlotIdx);
    else
      static_cast<RegionPressure&>(P).openBottom(CurrPos);
  }

  PhysRegOperands PhysRegOpers;
  VirtRegOperands VirtRegOpers;
  collectOperands(CurrPos, PhysRegOpers, VirtRegOpers, TRI, RCI);

  // Kill liveness at last uses.
  for (unsigned i = 0; i < PhysRegOpers.Uses.size(); ++i) {
    unsigned Reg = PhysRegOpers.Uses[i];
    if (!hasRegAlias(Reg, LivePhysRegs, TRI))
      discoverPhysLiveIn(Reg);
    else {
      // Allocatable physregs are always single-use before regalloc.
      decreasePhysRegPressure(Reg);
      LivePhysRegs.erase(Reg);
    }
  }
  for (unsigned i = 0; i < VirtRegOpers.Uses.size(); ++i) {
    unsigned Reg = VirtRegOpers.Uses[i];
    if (RequireIntervals) {
      const LiveInterval *LI = &LIS->getInterval(Reg);
      if (LI->killedAt(SlotIdx)) {
        if (LiveVirtRegs.erase(Reg))
          decreaseVirtRegPressure(Reg);
        else
          discoverVirtLiveIn(Reg);
      }
    }
    else if (!LiveVirtRegs.count(Reg)) {
      discoverVirtLiveIn(Reg);
      increaseVirtRegPressure(Reg);
    }
  }

  // Generate liveness for defs.
  for (unsigned i = 0; i < PhysRegOpers.Defs.size(); ++i) {
    unsigned Reg = PhysRegOpers.Defs[i];
    if (!hasRegAlias(Reg, LivePhysRegs, TRI)) {
      increasePhysRegPressure(Reg);
      LivePhysRegs.insert(Reg);
    }
  }
  for (unsigned i = 0; i < VirtRegOpers.Defs.size(); ++i) {
    unsigned Reg = VirtRegOpers.Defs[i];
    if (LiveVirtRegs.insert(Reg).second)
      increaseVirtRegPressure(Reg);
  }

  // Boost pressure for all dead defs together.
  increasePhysRegPressure(PhysRegOpers.DeadDefs);
  increaseVirtRegPressure(VirtRegOpers.DeadDefs);
  decreasePhysRegPressure(PhysRegOpers.DeadDefs);
  decreaseVirtRegPressure(VirtRegOpers.DeadDefs);

  // Find the next instruction.
  do
    ++CurrPos;
  while (CurrPos != MBB->end() && CurrPos->isDebugValue());
  return true;
}
