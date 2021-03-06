// [AsmJit]
// Complete x86/x64 JIT and Remote Assembler for C++.
//
// [License]
// ZLIB - See LICENSE.md file in the package.

// [Guard]
#ifndef _ASMJIT_CORE_RALOCAL_P_H
#define _ASMJIT_CORE_RALOCAL_P_H

#include "../core/build.h"
#ifndef ASMJIT_DISABLE_COMPILER

// [Dependencies]
#include "../core/raassignment_p.h"
#include "../core/radefs_p.h"
#include "../core/rapass_p.h"
#include "../core/support.h"

ASMJIT_BEGIN_NAMESPACE

//! \addtogroup asmjit_core_ra
//! \{

// ============================================================================
// [asmjit::RALocalAllocator]
// ============================================================================

//! Local register allocator.
class RALocalAllocator {
public:
  ASMJIT_NONCOPYABLE(RALocalAllocator)

  typedef RAAssignment::PhysToWorkMap PhysToWorkMap;
  typedef RAAssignment::WorkToPhysMap WorkToPhysMap;

  // --------------------------------------------------------------------------
  // [Init / Reset]
  // --------------------------------------------------------------------------

  inline RALocalAllocator(RAPass* pass) noexcept
    : _pass(pass),
      _cc(pass->cc()),
      _archTraits(pass->_archTraits),
      _availableRegs(pass->_availableRegs),
      _clobberedRegs(),
      _curAssignment(),
      _block(nullptr),
      _node(nullptr),
      _raInst(nullptr),
      _tiedTotal(),
      _tiedCount() {}

  Error init() noexcept;

  // --------------------------------------------------------------------------
  // [Accessors]
  // --------------------------------------------------------------------------

  inline RAWorkReg* workRegById(uint32_t workId) const noexcept { return _pass->workRegById(workId); }
  inline PhysToWorkMap* physToWorkMap() const noexcept { return _curAssignment.physToWorkMap(); }
  inline WorkToPhysMap* workToPhysMap() const noexcept { return _curAssignment.workToPhysMap(); }

  // --------------------------------------------------------------------------
  // [Block]
  // --------------------------------------------------------------------------

  //! Gets the currently processed block.
  inline RABlock* block() const noexcept { return _block; }
  //! Sets the currently processed block.
  inline void setBlock(RABlock* block) noexcept { _block = block; }

  // --------------------------------------------------------------------------
  // [Instruction]
  // --------------------------------------------------------------------------

  //! Gets the currently processed `InstNode`.
  inline InstNode* node() const noexcept { return _node; }
  //! Gets the currently processed `RAInst`.
  inline RAInst* raInst() const noexcept { return _raInst; }

  //! Gets all tied regs.
  inline RATiedReg* tiedRegs() const noexcept { return _raInst->tiedRegs(); }
  //! Gets grouped tied regs.
  inline RATiedReg* tiedRegs(uint32_t group) const noexcept { return _raInst->tiedRegs(group); }

  //! Gets TiedReg count (all).
  inline uint32_t tiedCount() const noexcept { return _tiedTotal; }
  //! Gets TiedReg count (per class).
  inline uint32_t tiedCount(uint32_t group) const noexcept { return _tiedCount.get(group); }

  inline bool isGroupUsed(uint32_t group) const noexcept { return _tiedCount[group] != 0; }

  // --------------------------------------------------------------------------
  // [Assignment]
  // --------------------------------------------------------------------------

  Error makeInitialAssignment() noexcept;

  Error replaceAssignment(
    const PhysToWorkMap* physToWorkMap,
    const WorkToPhysMap* workToPhysMap) noexcept;

  //! Switch to the given assignment by reassigning all register and emitting
  //! code that reassigns them. This is always used to switch to a previously
  //! stored assignment.
  //!
  //! If `tryMode` is true then the final assignment doesn't have to be exactly
  //! same as specified by `dstPhysToWorkMap` and `dstWorkToPhysMap`. This mode
  //! is only used before conditional jumps that already have assignment to
  //! generate a code sequence that is always executed regardless of the flow.
  Error switchToAssignment(
    PhysToWorkMap* dstPhysToWorkMap,
    WorkToPhysMap* dstWorkToPhysMap,
    const ZoneBitVector& liveIn,
    bool dstReadOnly,
    bool tryMode) noexcept;

  // --------------------------------------------------------------------------
  // [Allocation]
  // --------------------------------------------------------------------------

  Error allocInst(InstNode* inst) noexcept;
  Error allocBranch(InstNode* inst, RABlock* target, RABlock* cont) noexcept;

  // --------------------------------------------------------------------------
  // [Decision Making]
  // --------------------------------------------------------------------------

  enum CostModel : uint32_t {
    kCostOfFrequency = 1048576,
    kCostOfDirtyFlag = kCostOfFrequency / 4
  };

  inline uint32_t costByFrequency(float freq) const noexcept {
    return uint32_t(int32_t(freq * float(kCostOfFrequency)));
  }

  inline uint32_t calculateSpillCost(uint32_t group, uint32_t workId, uint32_t assignedId) const noexcept {
    RAWorkReg* workReg = workRegById(workId);
    uint32_t cost = costByFrequency(workReg->liveStats().freq());

    if (_curAssignment.isPhysDirty(group, assignedId))
      cost += kCostOfDirtyFlag;

    return cost;
  }

  //! Decides on register assignment.
  uint32_t decideOnAssignment(uint32_t group, uint32_t workId, uint32_t assignedId, uint32_t allocableRegs) const noexcept;

  //! Decides on whether to MOVE or SPILL the given WorkReg.
  //!
  //! The function must return either `RAAssignment::kPhysNone`, which means that
  //! the WorkReg should be spilled, or a valid physical register ID, which means
  //! that the register should be moved to that physical register instead.
  uint32_t decideOnUnassignment(uint32_t group, uint32_t workId, uint32_t assignedId, uint32_t allocableRegs) const noexcept;

  //! Decides on best spill given a register mask `spillableRegs`
  uint32_t decideOnSpillFor(uint32_t group, uint32_t workId, uint32_t spillableRegs, uint32_t* spillWorkId) const noexcept;

  // --------------------------------------------------------------------------
  // [Emit]
  // --------------------------------------------------------------------------

  //! Emits a move between a destination and source register, and fixes the
  //! register assignment.
  inline Error onMoveReg(uint32_t group, uint32_t workId, uint32_t dstPhysId, uint32_t srcPhysId) noexcept {
    if (dstPhysId == srcPhysId) return kErrorOk;
    _curAssignment.reassign(group, workId, dstPhysId, srcPhysId);
    return _pass->onEmitMove(workId, dstPhysId, srcPhysId);
  }

  //! Emits a swap between two physical registers and fixes their assignment.
  //!
  //! NOTE: Target must support this operation otherwise this would ASSERT.
  inline Error onSwapReg(uint32_t group, uint32_t aWorkId, uint32_t aPhysId, uint32_t bWorkId, uint32_t bPhysId) noexcept {
    _curAssignment.swap(group, aWorkId, aPhysId, bWorkId, bPhysId);
    return _pass->onEmitSwap(aWorkId, aPhysId, bWorkId, bPhysId);
  }

  //! Emits a load from [VirtReg/WorkReg]'s spill slot to a physical register
  //! and makes it assigned and clean.
  inline Error onLoadReg(uint32_t group, uint32_t workId, uint32_t physId) noexcept {
    _curAssignment.assign(group, workId, physId, RAAssignment::kClean);
    return _pass->onEmitLoad(workId, physId);
  }

  //! Emits a save a physical register to a [VirtReg/WorkReg]'s spill slot,
  //! keeps it assigned, and makes it clean.
  inline Error onSaveReg(uint32_t group, uint32_t workId, uint32_t physId) noexcept {
    ASMJIT_ASSERT(_curAssignment.workToPhysId(group, workId) == physId);
    ASMJIT_ASSERT(_curAssignment.physToWorkId(group, physId) == workId);

    _curAssignment.makeClean(group, workId, physId);
    return _pass->onEmitSave(workId, physId);
  }

  //! Assigns a register, the content of it is undefined at this point.
  inline Error onAssignReg(uint32_t group, uint32_t workId, uint32_t physId, uint32_t dirty) noexcept {
    _curAssignment.assign(group, workId, physId, dirty);
    return kErrorOk;
  }

  //! Spills a variable/register, saves the content to the memory-home if modified.
  inline Error onSpillReg(uint32_t group, uint32_t workId, uint32_t physId) noexcept {
    if (_curAssignment.isPhysDirty(group, physId))
      ASMJIT_PROPAGATE(onSaveReg(group, workId, physId));
    return onKillReg(group, workId, physId);
  }

  inline Error onDirtyReg(uint32_t group, uint32_t workId, uint32_t physId) noexcept {
    _curAssignment.makeDirty(group, workId, physId);
    return kErrorOk;
  }

  inline Error onKillReg(uint32_t group, uint32_t workId, uint32_t physId) noexcept {
    _curAssignment.unassign(group, workId, physId);
    return kErrorOk;
  }

  // --------------------------------------------------------------------------
  // [Members]
  // --------------------------------------------------------------------------

  //! Link to `RAPass`.
  RAPass* _pass;
  //! Link to `BaseCompiler`.
  BaseCompiler* _cc;

  //! Architecture traits.
  RAArchTraits _archTraits;
  //! Registers available to the allocator.
  RARegMask _availableRegs;
  //! Registers clobbered by the allocator.
  RARegMask _clobberedRegs;

  //! Register assignment (current).
  RAAssignment _curAssignment;
  //! Register assignment used temporarily during assignment switches.
  RAAssignment _tmpAssignment;

  //! Link to the current `RABlock`.
  RABlock* _block;
  //! InstNode.
  InstNode* _node;
  //! RA instruction.
  RAInst* _raInst;

  //! Count of all TiedReg's.
  uint32_t _tiedTotal;
  //! TiedReg's total counter.
  RARegCount _tiedCount;
};

//! \}

ASMJIT_END_NAMESPACE

// [Guard]
#endif // !ASMJIT_DISABLE_COMPILER
#endif // _ASMJIT_CORE_RALOCAL_P_H
