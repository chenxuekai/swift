//===--- SILGenPattern.cpp - Pattern matching codegen ---------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGen.h"
#include "Cleanup.h"
#include "Initialization.h"
#include "RValue.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Support/FormattedStream.h"
#include "swift/AST/Diagnostics.h"
#include "swift/AST/Types.h"

using namespace swift;
using namespace Lowering;

/// True if a pattern is a wildcard, meaning it matches any value. '_' and
/// variable patterns are wildcards. We also consider ExprPatterns to be
/// wildcards; we test the match expression as a guard outside of the normal
/// pattern clause matrix. When destructuring wildcard patterns, we also use
/// nullptr to represent newly-constructed wildcards.
static bool isWildcardPattern(const Pattern *p) {
  if (!p)
    return true;

  switch (p->getKind()) {
  // Simple wildcards.
  case PatternKind::Any:
  case PatternKind::Expr:
  case PatternKind::Named:
    return true;
  
  // Non-wildcards.
  case PatternKind::Tuple:
  case PatternKind::Isa:
  case PatternKind::NominalType:
    return false;
  
  // Recur into simple wrapping patterns.
  case PatternKind::Paren:
    return isWildcardPattern(cast<ParenPattern>(p)->getSubPattern());
  case PatternKind::Typed:
    return isWildcardPattern(cast<TypedPattern>(p)->getSubPattern());
  case PatternKind::Var:
    return isWildcardPattern(cast<VarPattern>(p)->getSubPattern());
  }
}
  
/// Emit a conditional branch testing if a value matches the top-level node
/// of this pattern.
/// On the true branch, destructure the value. On return, the insertion point
/// is inside the true branch. The false branch is returned.
///
/// If the pattern node is irrefutable, this performs an unconditional branch
/// and returns null.
static SILBasicBlock *emitBranchAndDestructure(SILGenFunction &gen,
                                      const Pattern *p,
                                      SILValue v,
                                      SmallVectorImpl<SILValue> &destructured) {
  p = p->getSemanticsProvidingPattern();
  
  switch (p->getKind()) {
  case PatternKind::Any:
  case PatternKind::Named:
  case PatternKind::Expr:
    llvm_unreachable("wildcards shouldn't get here");

  case PatternKind::Tuple: {
    auto *tp = cast<TuplePattern>(p);
      
    // Tuples are irrefutable; destructure without branching.
    auto tupleTy = tp->getType()->castTo<TupleType>();
    auto tupleSILTy = gen.getLoweredType(tupleTy);
    if (tupleSILTy.isAddressOnly(gen.F.getModule())) {
      for (unsigned i = 0, e = tupleTy->getFields().size(); i < e; ++i) {
        auto &field = tupleTy->getFields()[i];
        SILType fieldTy = gen.getLoweredType(field.getType());
        SILValue member = gen.B.createTupleElementAddr(SILLocation(),
                                             v, i, fieldTy.getAddressType());
        if (!fieldTy.isAddressOnly(gen.F.getModule()))
          member = gen.B.createLoad(SILLocation(), member);
        destructured.push_back(member);
      }
    } else {
      for (unsigned i = 0, e = tupleTy->getFields().size(); i < e; ++i) {
        auto &field = tupleTy->getFields()[i];
        SILType fieldTy = gen.getLoweredLoadableType(field.getType());
        SILValue member = gen.B.createTupleExtract(SILLocation(),
                                                   v, i, fieldTy);
        destructured.push_back(member);
      }
    }
    return nullptr;
  }
      
  case PatternKind::Isa: {
    auto *ip = cast<IsaPattern>(p);
    
    // Perform a conditional cast and branch on whether it succeeded.
    SILValue cast = gen.emitCheckedCast(SILLocation(),
                                      ManagedValue(v, ManagedValue::Unmanaged),
                                      ip->getType(),
                                      ip->getCastTypeLoc().getType(),
                                      ip->getCastKind(),
                                      CheckedCastMode::Conditional,
                                      /*useCastValue*/ false);
    SILValue didMatch = gen.B.createIsNonnull(SILLocation(), cast,
                    SILType::getBuiltinIntegerType(1, gen.F.getASTContext()));
    
    // On the true branch, we can use the cast value.
    // If the cast result is loadable and we cast a value address, load it.
    if (cast.getType().isAddress()
        && !cast.getType().isAddressOnly(gen.F.getModule()))
      cast = gen.B.createLoad(SILLocation(), cast);
    destructured.push_back(cast);
    
    // Emit the branch.
    SILBasicBlock *trueBB = new (gen.F.getModule()) SILBasicBlock(&gen.F);
    SILBasicBlock *falseBB = new (gen.F.getModule()) SILBasicBlock(&gen.F);
    
    gen.B.createCondBranch(SILLocation(), didMatch,
                           trueBB, falseBB);
    
    // Continue codegen into the true block, and return the false block.
    gen.B.emitBlock(trueBB);
    return falseBB;

  }
      
  case PatternKind::NominalType:
    llvm_unreachable("not implemented");
      
  case PatternKind::Paren:
  case PatternKind::Typed:
  case PatternKind::Var:
    llvm_unreachable("pattern node is never semantic");
  }
}

/// Return the number of columns a destructured pattern constructor produces.
unsigned getDestructuredWidth(const Pattern *specializer) {
  specializer = specializer->getSemanticsProvidingPattern();
  switch (specializer->getKind()) {
  case PatternKind::Any:
  case PatternKind::Named:
  case PatternKind::Expr:
    llvm_unreachable("shouldn't specialize on a wildcard pattern");

  // Tuples destructure into the component tuple fields.
  case PatternKind::Tuple:
    return cast<TuplePattern>(specializer)->getFields().size();
      
  // 'is' patterns destructure to the pattern as the cast type.
  case PatternKind::Isa:
    return 1;
      
  case PatternKind::NominalType:
    llvm_unreachable("not implemented");
      
  case PatternKind::Paren:
  case PatternKind::Typed:
  case PatternKind::Var:
    llvm_unreachable("not semantic");
  }
}

/// Destructure a pattern parallel to the specializing pattern of a clause
/// matrix.
/// Destructured wildcards are represented with null Pattern* pointers.
/// p must be non-orthogonal to this pattern constructor.
void destructurePattern(SILGenFunction &gen,
                        const Pattern *specializer,
                        const Pattern *p,
                        SmallVectorImpl<const Pattern *> &destructured) {
  specializer = specializer->getSemanticsProvidingPattern();
  assert(p && !isWildcardPattern(p) &&
         "wildcard patterns shouldn't be passed here");
  p = p->getSemanticsProvidingPattern();
  switch (specializer->getKind()) {
  case PatternKind::Any:
  case PatternKind::Named:
  case PatternKind::Expr:
    llvm_unreachable("shouldn't specialize on a wildcard pattern");

  case PatternKind::Tuple: {
    auto specializerTuple = cast<TuplePattern>(specializer);
    
    // Tuples should only match with other tuple patterns. Destructure into
    // the tuple fields.
    auto tp = cast<TuplePattern>(p);
    assert(specializerTuple->getFields().size() == tp->getFields().size()
           && "tuple patterns do not share shape");
    std::transform(tp->getFields().begin(), tp->getFields().end(),
                   std::back_inserter(destructured),
                   [&](const TuplePatternElt &e) -> const Pattern * {
                     return e.getPattern();
                   });
    return;
  }
      
  case PatternKind::Isa: {
    auto *specializerIsa = cast<IsaPattern>(specializer);
    auto *ip = cast<IsaPattern>(p);
    CanType newFromType
      = specializerIsa->getCastTypeLoc().getType()->getCanonicalType();
    CanType newToType = ip->getCastTypeLoc().getType()->getCanonicalType();
    
    // If a cast pattern casts to the same type, it reduces to a wildcard.
    // FIXME: 'is' patterns should have a subpattern, in which case they
    // destructure to that pattern in this case.
    if (newFromType == newToType) {
      destructured.push_back(nullptr);
      return;
    }

    // If a cast pattern is non-orthogonal and not to the same type, then
    // we have a cast to a superclass, subclass, or archetype. Produce a
    // new checked cast pattern from the destructured type.
    
    CheckedCastKind newKind;
    // Determine the new cast kind.
    bool fromArchetype = isa<ArchetypeType>(newFromType),
         toArchetype = isa<ArchetypeType>(newToType);
    if (fromArchetype && toArchetype) {
      newKind = CheckedCastKind::ArchetypeToArchetype;
    } else if (fromArchetype) {
      newKind = CheckedCastKind::ArchetypeToConcrete;
    } else if (toArchetype) {
      if (newFromType->isExistentialType()) {
        newKind = CheckedCastKind::ExistentialToArchetype;
      } else {
        // FIXME: concrete-to-archetype
        assert(newFromType->getClassOrBoundGenericClass());
        newKind = CheckedCastKind::SuperToArchetype;
      }
    } else {
      // TODO: For now, we conservatively treat all class-to-class
      // destructurings as downcasts. When SILGen gains the ability to query
      // super/subclass relationships, then superclass patterns should be
      // destructured, and subclass patterns should be turned into downcast
      // patterns as below.
      assert(newFromType->getClassOrBoundGenericClass() &&
             newToType->getClassOrBoundGenericClass() &&
             "non-class, non-archetype cast patterns should be orthogonal!");
      newKind = CheckedCastKind::Downcast;
    }
    
    // Create the new cast pattern.
    auto *newIsa
      = new (gen.F.getASTContext()) IsaPattern(p->getLoc(),
                                               ip->getCastTypeLoc());
    newIsa->setType(newFromType);
    
    destructured.push_back(newIsa);
    return;
  }
      
  case PatternKind::NominalType:
    llvm_unreachable("not implemented");
      
  case PatternKind::Paren:
  case PatternKind::Typed:
  case PatternKind::Var:
    llvm_unreachable("not semantic");
  }
}

/// True if two pattern nodes are orthogonal, that is, they never both match
/// the same value.
bool arePatternsOrthogonal(const Pattern *a,
                           const Pattern *b) {
  // Wildcards are never orthogonal.
  if (!a) return false;
  if (!b) return false;
  if (isWildcardPattern(b)) return false;

  a = a->getSemanticsProvidingPattern();
  b = b->getSemanticsProvidingPattern();

  // A pattern is never orthogonal to itself.
  if (a == b) return false;

  switch (a->getKind()) {
  case PatternKind::Any:
  case PatternKind::Named:
  case PatternKind::Expr:
    return false;
      
  case PatternKind::Tuple: {
    // Tuples should only match wildcard or same-shaped tuple patterns, to
    // which they are never orthogonal.
    auto *ta = cast<TuplePattern>(a);
    auto *tb = cast<TuplePattern>(b);
    
    assert(ta->getType()->isEqual(tb->getType()) &&
           "tuple patterns should match same type");
    assert(ta->getFields().size() == tb->getFields().size() &&
           "tuple patterns have same shape");

    (void)ta;
    (void)tb;

    return false;
  }
      
  case PatternKind::Isa: {
    auto *ia = cast<IsaPattern>(a);

    // Casts are orthogonal to non-cast patterns.
    // FIXME: Not to NominalTypePatterns of related type.
    assert(!isa<NominalTypePattern>(b)
           && "Isa/NominalType combination not implemented");
    auto *ib = dyn_cast<IsaPattern>(b);
    if (!ib)
      return true;
    
    // Casts to the same type are parallel.
    Type aTy = ia->getCastTypeLoc().getType();
    Type bTy = ib->getCastTypeLoc().getType();
    
    if (aTy->isEqual(bTy))
      return false;
      
    // Archetype casts are never orthogonal; the archetype could substitute
    // for any other type.
    // TODO: Casts to archetypes with unrelated superclass constraints could
    // be orthogonal. We could also treat casts to types that don't fit the
    // archetype's constraints as orthogonal.
    if (aTy->is<ArchetypeType>())
      return false;
    if (bTy->is<ArchetypeType>())
      return false;
    
    // Class casts are orthogonal to non-class casts.
    bool aClass = aTy->getClassOrBoundGenericClass();
    bool bClass = bTy->getClassOrBoundGenericClass();
      
    if (!aClass || !bClass)
      return true;
      
    // Class casts are orthogonal to casts to a class without a subtype or
    // supertype relationship.
    // TODO: We can't check super/subclass relationships in SILGen yet.
    // For now we conservatively assume all class casts may overlap.
    return false;
  }
      
  case PatternKind::NominalType:
    llvm_unreachable("not implemented");
      
  case PatternKind::Paren:
  case PatternKind::Typed:
  case PatternKind::Var:
    llvm_unreachable("not semantic");
  }
}

/// True if a set of pattern nodes forms a signature for the type they match.
static bool
patternsFormSignature(ArrayRef<const Pattern*> set) {
  if (set.empty())
    return false;
  
  for (const Pattern *p : set) {
    p = p->getSemanticsProvidingPattern();
    switch (p->getKind()) {
    // Tuple and non-oneof nominal type patterns trivially form a signature.
    case PatternKind::Tuple:
    case PatternKind::NominalType:
      return true;
      
    // 'is' constructors never affect signature-ness; there can always be
    // new types we don't know about.
    case PatternKind::Isa:
      continue;
      
    case PatternKind::Any:
    case PatternKind::Named:
    case PatternKind::Expr:
      llvm_unreachable("shouldn't have specialized on a wildcard");
        
    case PatternKind::Paren:
    case PatternKind::Var:
    case PatternKind::Typed:
      llvm_unreachable("not semantic");
    }
  }
  return false;
}

namespace {

/// A pair of an ExprPattern node and the SILValue it will test.
struct ExprGuard {
  const ExprPattern *pattern;
  SILValue value;
  
  bool operator==(ExprGuard o) const {
    return pattern == o.pattern && value == o.value;
  }
  
  bool operator!=(ExprGuard o) const {
    return pattern != o.pattern || value != o.value;
  }
};
  
/// A CaseMap entry.
struct CaseBlock {
  /// The entry BB for the case.
  SILBasicBlock *entry = nullptr;
  /// The continuation BB for the case.
  SILBasicBlock *cont = nullptr;
  /// The scope of the case.
  CleanupsDepth cleanupsDepth = CleanupsDepth::invalid();
};
  
/// Map type used to associate CaseStmts to SILBasicBlocks. Filled in as
/// dispatch is resolved.
using CaseMap = llvm::MapVector<CaseStmt*, CaseBlock>;

/// A handle to a row in a clause matrix. Does not own memory; use of the
/// ClauseRow must be dominated by its originating ClauseMatrix.
class ClauseRow {
  friend class ClauseMatrix;
  
  /// The in-memory layout of a clause row prefix.
  struct Prefix {
    /// The CaseStmt corresponding to the patterns in the row.
    CaseStmt *caseBlock;
    /// The guard expression for the row, or null if there is no guard.
    Expr *guardExpr;
    /// The cleanup level of the case body.
    CleanupsDepth cleanupsDepth;
    /// The continuation BB for this case.
    SILBasicBlock *cont;
    /// The ExprPatterns within the pattern and their matching values.
    unsigned firstExprGuard, lastExprGuard;
  };
  
  Prefix *row;
  unsigned columnCount;
  ArrayRef<ExprGuard> exprGuards;
  
  // ClauseRows should only be vended by ClauseMatrix::operator[].
  ClauseRow(Prefix *row, unsigned columnCount, ArrayRef<ExprGuard> guards)
    : row(row), columnCount(columnCount), exprGuards(guards)
  {}
  
public:
  ClauseRow() = default;
  
  CaseStmt *getCaseBlock() const {
    return row->caseBlock;
  }
  Expr *getGuard() const {
    return row->guardExpr;
  }
  bool hasGuard() const {
    return row->guardExpr;
  }
  CleanupsDepth getCleanupsDepth() const {
    return row->cleanupsDepth;
  }
  void setCleanupsDepth(CleanupsDepth d) {
    row->cleanupsDepth = d;
  }
  
  SILBasicBlock *getContBB() const {
    return row->cont;
  }
  
  ArrayRef<ExprGuard> getExprGuards() const {
    return exprGuards;
  }
  
  /// Emit the case block corresponding to this row if necessary. Returns the
  /// entry point BB emitted for the block. If the mayUseBB argument is nonnull,
  /// and the case has not been emitted, it will be emitted into the given BB.
  SILBasicBlock *emitCaseBlock(SILGenFunction &gen, CaseMap &caseMap) const {
    CaseBlock &dest = caseMap[getCaseBlock()];
    
    // If the block was emitted, sanity check that it was emitted at the same
    // scope level we think it should be.
    if (dest.entry) {
      assert(getCleanupsDepth() == dest.cleanupsDepth
             && "divergent cleanup depths for case");
      assert(getContBB() == dest.cont
             && "divergent continuation BBs for case");
      return dest.entry;
    }

    // Set up the basic block for the case.
    dest.entry = new (gen.F.getModule()) SILBasicBlock(&gen.F);
    dest.cleanupsDepth = getCleanupsDepth();
    dest.cont = getContBB();
    
    SILBasicBlock *savedIP = gen.B.getInsertionBB();
    
    gen.B.setInsertionPoint(dest.entry);
    gen.visit(getCaseBlock()->getBody());
    if (gen.B.hasValidInsertionPoint())
      gen.B.createBranch(getCaseBlock(), getContBB());
    
    // Restore the insertion point (unless we didn't have one).
    if (savedIP)
      gen.B.setInsertionPoint(savedIP);
    
    return dest.entry;
  }
  
  /// Emit dispatch to the row's destination. If the row has no guard, the
  /// branch is unconditional, and this terminates the current block and
  /// returns true. Otherwise, it dispatches conditionally on the guard, leaves
  /// the insertion branch on the not-taken path, and returns false.
  bool emitDispatch(SILGenFunction &gen, CaseMap &caseMap) const {
    // If there is no guard, branch unconditionally.
    if (!hasGuard() && exprGuards.empty()) {
      // If we haven't emitted a block for this case, and our insertion point
      // BB is empty, we can hijack this BB as the case's BB.
      SILBasicBlock *insertionBB = gen.B.getInsertionBB();
      SILBasicBlock *caseBB = emitCaseBlock(gen, caseMap);
      
      if (caseBB != insertionBB)
        gen.Cleanups.emitBranchAndCleanups(JumpDest{caseBB, getCleanupsDepth()});
      
      gen.B.moveBlockToEnd(caseBB);
      return true;
    }
    
    // Emit the case body.
    SILBasicBlock *caseBB = emitCaseBlock(gen, caseMap);

    // Create new BBs for the guard branch.
    SILBasicBlock *trueBB = new (gen.F.getModule()) SILBasicBlock(&gen.F);
    SILBasicBlock *falseBB = new (gen.F.getModule()) SILBasicBlock(&gen.F);
    
    // TODO: We should emit every guard once and share code if it covers
    // multiple patterns.
    if (!exprGuards.empty()) {
      // If we have a guard, we'll jump to it if all the expr patterns check
      // out. Otherwise we can jump directly to the destination.
      SILBasicBlock *guardBB = trueBB;
      if (hasGuard())
        guardBB = new (gen.F.getModule()) SILBasicBlock(&gen.F);
      // Test ExprPatterns from the row in an "and" chain.
      for (unsigned i = 0, e = exprGuards.size(); i < e; ++i) {
        ExprGuard eg = exprGuards[i];
        
        // The last pattern test jumps to the guard.
        SILBasicBlock *nextBB = guardBB;
        if (i < e - 1)
          nextBB = new (gen.F.getModule()) SILBasicBlock(&gen.F);
        
        // Emit the match test.
        SILValue testBool;
        {
          FullExpr scope(gen.Cleanups);
          testBool = gen.visit(eg.pattern->getMatchExpr())
            .getUnmanagedSingleValue(gen);
        }

        // If the test succeeds, we move on to the next test; otherwise, we
        // fail and move to the next pattern.
        gen.B.createCondBranch(getCaseBlock(), testBool, nextBB, falseBB);
        if (i < e - 1)
          gen.B.emitBlock(nextBB);
      }
      
      if (hasGuard())
        gen.B.emitBlock(guardBB);
    }
    
    if (hasGuard()) {
      SILValue guardBool;
      {
        FullExpr scope(gen.Cleanups);
          
        // Emit the guard.
        // TODO: We should emit every guard once and share code if it covers
        // multiple patterns.
        guardBool = gen.visit(getGuard()).getUnmanagedSingleValue(gen);
      }
      
      // Branch either to the row destination or the new BB.
      gen.B.createCondBranch(getCaseBlock(), guardBool, trueBB, falseBB);
    }
    
    // On the true block, jump to the case block, unwinding if necessary.
    gen.B.emitBlock(trueBB);
    gen.Cleanups.emitBranchAndCleanups(JumpDest{caseBB, getCleanupsDepth()});
    
    // Position the case block logically.
    gen.B.moveBlockToEnd(caseBB);
    
    // Continue codegen on the false branch.
    gen.B.emitBlock(falseBB);
    return false;
  }

  ArrayRef<const Pattern *> getColumns() const {
    return {reinterpret_cast<const Pattern * const*>(row + 1), columnCount};
  }
  MutableArrayRef<const Pattern *> getColumns() {
    return {reinterpret_cast<const Pattern **>(row + 1), columnCount};
  }
  
  const Pattern * const *begin() const {
    return getColumns().begin();
  }
  const Pattern * const *end() const {
    return getColumns().end();
  }
  
  const Pattern **begin() {
    return getColumns().begin();
  }
  const Pattern **end() {
    return getColumns().end();
  }
  
  const Pattern *operator[](unsigned column) const {
    return getColumns()[column];
  }
  const Pattern *&operator[](unsigned column) {
    return getColumns()[column];
  }
  unsigned columns() const {
    return columnCount;
  }
};

/// Get a pattern as an ExprPattern, unwrapping semantically transparent
/// pattern nodes.
const ExprPattern *getAsExprPattern(const Pattern *p) {
  if (!p) return nullptr;

  switch (p->getKind()) {
  case PatternKind::Expr:
    return cast<ExprPattern>(p);
  
  case PatternKind::Tuple:
  case PatternKind::Named:
  case PatternKind::Any:
  case PatternKind::Isa:
  case PatternKind::NominalType:
    return nullptr;

  // Recur into simple wrapping patterns.
  case PatternKind::Paren:
    return getAsExprPattern(cast<ParenPattern>(p)->getSubPattern());
  case PatternKind::Typed:
    return getAsExprPattern(cast<TypedPattern>(p)->getSubPattern());
  case PatternKind::Var:
    return getAsExprPattern(cast<VarPattern>(p)->getSubPattern());
  }
}

/// A clause matrix. This matrix associates subpattern rows to their
/// corresponding guard expressions, and associates destination basic block
/// and columns to their associated subject value.
class ClauseMatrix {
  /// The capacity of the memory buffer in rows/columns.
  unsigned rowCapacity, columnCapacity;
  /// The inhabited part of the memory buffer.
  unsigned rowCount, columnCount;
  /// The memory buffer containing the matrix data.
  void *data;
  /// A list of ExprPatterns and the associated SILValue they test. Rows
  /// reference into slices of this vector to indicate which ExprPatterns form
  /// part of their guard.
  std::vector<ExprGuard> exprGuards;
  
  // The memory layout of data is as follows:
  
  // - columnCapacity SILValues, for the occurrence vector. The first
  //   columnCount entries are populated.
  size_t getOccurrenceVectorStride() const {
    return columnCapacity * sizeof(SILValue);
  }
  MutableArrayRef<SILValue> getMutableOccurrences() {
    return {reinterpret_cast<SILValue*>(data), columnCount};
  }
  
  // - rowCapacity rows, each of which contains:
  //   - a ClauseRow::Prefix, followed by
  //   - columnCapacity Pattern*s, for the clause patterns.
  
  size_t getRowStride() const {
    return sizeof(ClauseRow::Prefix) + columnCapacity * sizeof(Pattern*);
  }
  ClauseRow::Prefix *getMutableRowPrefix(unsigned i) {
    assert(i < rowCount && "row access out of bounds");
    char *firstRow = reinterpret_cast<char*>(data) + getOccurrenceVectorStride();
    return reinterpret_cast<ClauseRow::Prefix*>(firstRow + getRowStride() * i);
  }
  
  size_t getDataSize() const {
    return getOccurrenceVectorStride() + getRowStride() * rowCapacity;
  }
  
  ClauseMatrix(const ClauseMatrix &) = delete;
  ClauseMatrix &operator=(const ClauseMatrix &) = delete;
public:
  /// Allocate a clause matrix over the given set of occurrences
  /// (actively matched values) and enough initial capacity for the
  /// given number of rows. The clause matrix will be initialized with zero rows
  /// and a column for every occurrence. Rows can be added using addRows.
  explicit ClauseMatrix(ArrayRef<SILValue> occurrences,
                        unsigned rowCapacity)
    : rowCapacity(rowCapacity), columnCapacity(occurrences.size()),
      rowCount(0), columnCount(columnCapacity)
  {
    // Allocate the data buffer.
    data = malloc(getDataSize());
    assert(data);
    
    // Initialize the occurrence vector.
    MutableArrayRef<SILValue> occurrenceBuf = getMutableOccurrences();
    for (unsigned i = 0, e = occurrences.size(); i < e; ++i) {
      occurrenceBuf[i] = occurrences[i];
    }
  }
  
  ~ClauseMatrix() {
    free(data);
  }
  
  ClauseMatrix(ClauseMatrix &&m)
    : rowCapacity(m.rowCapacity), columnCapacity(m.columnCapacity),
      rowCount(m.rowCount), columnCount(m.columnCount),
      data(m.data)
  {
    m.data = nullptr;
  }

  unsigned rows() const { return rowCount; }
  unsigned columns() const { return columnCount; }

  /// Append a row to the matrix.
  void addRow(CaseStmt *caseBlock, Expr *guardExpr,
              ArrayRef<const Pattern*> cols,
              CleanupsDepth scope,
              SILBasicBlock *contBB,
              ArrayRef<ExprGuard> parentExprGuards = {}) {
    assert(cols.size() == columnCount && "new row has wrong number of columns");

    // Grow storage if necessary.
    if (rowCount == rowCapacity) {
      rowCapacity *= 2;
      data = realloc(data, getDataSize());
      assert(data);
    }
    
    // Collect guards introduced by ExprPatterns in this row.
    unsigned exprGuardStart = exprGuards.size();
    exprGuards.insert(exprGuards.end(),
                      parentExprGuards.begin(), parentExprGuards.end());
    for (unsigned i = 0, e = cols.size(); i < e; ++i) {
      auto *ep = getAsExprPattern(cols[i]);
      if (!ep)
        continue;
      ExprGuard eg{ep, getOccurrences()[i]};
      if (std::find(parentExprGuards.begin(), parentExprGuards.end(), eg)
           == parentExprGuards.end())
        exprGuards.push_back(eg);
    }
    unsigned exprGuardEnd = exprGuards.size();
    
    // Initialize the next row.
    ClauseRow::Prefix *row = getMutableRowPrefix(rowCount++);
    ::new (row) ClauseRow::Prefix{caseBlock, guardExpr,
                                  scope, contBB,
                                  exprGuardStart, exprGuardEnd};
    MutableArrayRef<const Pattern*> columnsBuf{
      reinterpret_cast<const Pattern**>(row+1),
      columnCount
    };
    
    for (unsigned i = 0; i < columnCount; ++i)
      columnsBuf[i] = cols[i];
  }
  
  /// Remove a row from the matrix.
  void removeRow(unsigned r) {
    assert(r < rowCount && "removeRow out of bounds");
    memmove(getMutableRowPrefix(r), getMutableRowPrefix(r+1),
            getRowStride() * (rowCount - (r+1)));
    --rowCount;
  }
  
  ArrayRef<SILValue> getOccurrences() const {
    return const_cast<ClauseMatrix*>(this)->getMutableOccurrences();
  }
  
  ClauseRow operator[](unsigned row) {
    auto *prefix = getMutableRowPrefix(row);
    return {getMutableRowPrefix(row), columnCount,
            {&exprGuards[prefix->firstExprGuard],
             prefix->lastExprGuard - prefix->firstExprGuard}};
  }
  const ClauseRow operator[](unsigned row) const {
    return const_cast<ClauseMatrix&>(*this)[row];
  }
  
  /// Specialize this matrix's first column on a constructor, and emit a branch
  /// conditional on the constructor matching the current value. Given an n-ary
  /// constructor form c(x1...xn), this performs the following row-wise
  /// transformation to create a new matrix:
  ///
  /// If the row is [ c(x1...xn)   p1 p2 ... -> Dest ],
  /// expand to     [ x1 x2 ... xn p1 p2 ... -> Dest ].
  ///
  /// If the row is [ _            p1 p2 ... -> Dest ],
  /// expand to     [ _  _  ... _  p1 p2 ... -> Dest ].
  ///
  /// If the row is [ c'(...)      p1 p2 ... -> Dest ] where c' != c,
  /// remove the column.
  ///
  /// The first skipRows rows are removed prior to the transformation.
  /// We treat ExprPatterns as wildcards with a guard.
  ///
  /// Returns a pair of the
  /// specialized clause matrix and the false branch for the constructor test.
  /// If the constructor is irrefutable, as for a tuple, struct, class, or
  /// singleton oneof, the branch block will be null.
  std::pair<ClauseMatrix, SILBasicBlock /*nullable*/ *>
  emitSpecializedBranch(SILGenFunction &gen,
                        const Pattern *specializer, unsigned skipRows,
                        CleanupsDepth specDepth, SILBasicBlock *specCont) const{
    assert(columnCount >= 1 && "can't specialize a matrix with no columns");
    assert(skipRows < rowCount && "can't skip more rows than we have");
    
    // Test and destructure the first column's occurrence.
    SmallVector<SILValue, 4> newOccurrences;
    SILBasicBlock *falseBB = emitBranchAndDestructure(gen, specializer,
                                                      getOccurrences()[0],
                                                      newOccurrences);
    // Forward the remaining columns' occurrences.
    std::copy(getOccurrences().begin() + 1, getOccurrences().end(),
              std::back_inserter(newOccurrences));
    
    // Build the specialized clause matrix.
    std::pair<ClauseMatrix, SILBasicBlock*> result{
      ClauseMatrix{newOccurrences, rowCount},
      falseBB
    };
    ClauseMatrix &specialized = result.first;
    
    unsigned specializedWidth = getDestructuredWidth(specializer);
    
    for (unsigned r = skipRows; r < rowCount; ++r) {
      auto row = (*this)[r];
      auto columns = row.getColumns();
      // If the pattern in this row isn't a wildcard and matches an orthogonal
      // constructor, it is removed from the specialized matrix.
      if (arePatternsOrthogonal(specializer, columns[0]))
        continue;
      
      // Destructure matching constructors and wildcards.
      SmallVector<const Pattern*, 4> newPatterns;
      CleanupsDepth rowDepth = row.getCleanupsDepth();
      SILBasicBlock *rowCont = row.getContBB();
      if (isWildcardPattern(columns[0])) {
        // Wildcards explode into more wildcards. Since exploding a wildcard
        // won't expose more var bindings, we don't modify the scope from the
        // original row. (This scope must be the same as the scope in other
        // specializations that carry the row forward, since we emit the
        // case block only once.)
        for (unsigned i = 0; i < specializedWidth; ++i)
          newPatterns.push_back(nullptr);
      } else {
        // Non-wildcards destructure relative to the specializing pattern
        // constructor. Since the result row may bind to newly-exposed vars,
        // we rescope it to the specialized scope.
        destructurePattern(gen, specializer, columns[0], newPatterns);
        rowDepth = CleanupsDepth::invalid();
        rowCont = specCont;
      }
      assert(newPatterns.size() == specializedWidth &&
             "destructurePattern did not produce number of columns reported "
             "by getDestructuredWidth");
      
      // Forward the remaining pattern columns from the row.
      std::copy(columns.begin() + 1, columns.end(),
                std::back_inserter(newPatterns));
      
      // Append the new row.
      specialized.addRow(row.getCaseBlock(), row.getGuard(),
                         newPatterns,
                         rowDepth, rowCont,
                         row.getExprGuards());
    }
    
    // Emit variable bindings from the newly specialized rows.
    for (unsigned i = 0; i < specializedWidth; ++i)
      specialized.emitVarsInColumn(gen, i);
    
    // Update the scope of specialized rows to include the new variables.
    for (unsigned r = 0, e = specialized.rows(); r < e; ++r)
      if (!specialized[r].getCleanupsDepth().isValid())
        specialized[r].setCleanupsDepth(gen.Cleanups.getCleanupsDepth());
    
    return result;
  }
  
  /// Emit pattern variable bindings, if any, to the value in a column of the
  /// matrix.
  void emitVarsInColumn(SILGenFunction &gen, unsigned column) {
    assert(column < columns() && "column out of bounds");
    if (rows() == 0) return;
    
    SILValue v = getOccurrences()[column];
    
    // Since only one row's variables will ever be available in any case block,
    // we can emit one box and alias all the column variables to it.
    // FIXME: If we end up supporting variables in case alternates, we will need
    // to handle that, perhaps by phi-ing the corresponding boxes into the case
    // block.
    VarDecl *emittedVar = nullptr;
    
    auto emitVar = [&](VarDecl *vd) {
      // FIXME: Pessimize all variable bindings to be allocated in boxes.
      // The old capture analysis pass will be dying soon anyway.
      vd->setHasFixedLifetime(false);
      
      // If we already emitted a variable from another row, alias that variable.
      if (emittedVar) {
        gen.VarLocs[vd] = gen.VarLocs[emittedVar];
        return;
      }
      
      // Create and initialize the variable.
      InitializationPtr init = gen.emitLocalVariableWithCleanup(vd);
      ManagedValue(v, ManagedValue::Unmanaged).copyInto(gen,init->getAddress());
      init->finishInitialization(gen);
      emittedVar = vd;
    };
    
    for (unsigned r = 0, e = rows(); r < e; ++r) {
      const Pattern *p = (*this)[r][column];
      if (!p)
        continue;
      p = p->getSemanticsProvidingPattern();
      switch (p->getKind()) {
      // Non-variables.
      case PatternKind::Any:
      case PatternKind::Tuple:
      case PatternKind::NominalType:
      case PatternKind::Isa:
        continue;
          
      case PatternKind::Named:
        // Bind the named variable.
        emitVar(cast<NamedPattern>(p)->getDecl());
        break;
          
      case PatternKind::Expr:
        // Bind the ExprPattern's implicit match var.
        // TODO: It'd be nice not to need the temp var for expr patterns.
        emitVar(cast<ExprPattern>(p)->getMatchVar());
        break;

      case PatternKind::Paren:
      case PatternKind::Typed:
      case PatternKind::Var:
        llvm_unreachable("not semantic");
      }
    }
  }
  
  /// Transform this matrix into its default. The following row-wise
  /// transformation on the matrix is performed in-place:
  ///
  /// If the row is [ _ p1 p2 ... -> Dest ],
  /// reduce to       [ p1 p2 ... -> Dest ].
  ///
  /// If the row is [ c(...) p1 p2 ... -> Dest ] for any constructor c,
  /// remove the row.
  ///
  /// This collects all the wildcard rows not exclusively associated with any
  /// one specialized matrix.
  ///
  /// The first skipRows rows are removed prior to the transformation.
  /// We treat ExprPatterns as wildcards with a guard.
  void reduceToDefault(unsigned skipRows) {
    assert(columnCount >= 1 && "can't default a matrix with no columns");
    assert(skipRows < rowCount && "can't skip more rows than we have");

    // Drop the first occurrence.
    memmove(getMutableOccurrences().begin(), getMutableOccurrences().begin()+1,
            sizeof(SILValue) * (columnCount - 1));
    
    // Discard the skipped rows.
    memmove(getMutableRowPrefix(0), getMutableRowPrefix(skipRows),
            getRowStride() * (rowCount - skipRows));
    rowCount -= skipRows;
    
    unsigned r = 0;
    while (r < rowCount) {
      // Filter specialized rows.
      if (!isWildcardPattern((*this)[r][0])) {
        removeRow(r);
        continue;
      }
      
      // Discard the head wildcard of wildcard rows.
      auto row = (*this)[r];
      memmove(row.begin(), row.begin()+1, sizeof(Pattern*) * (columnCount - 1));
      ++r;
    }
    
    // Shrink the column count.
    --columnCount;
  }  
};

} // end anonymous namespace

/// Recursively emit a decision tree from the given pattern matrix.
static void emitDecisionTree(SILGenFunction &gen,
                             SwitchStmt *stmt,
                             ClauseMatrix &&clauses,
                             CaseMap &caseMap,
                             SILBasicBlock *contBB) {
  // If there are no rows, then we fail. This will be a dataflow error if we
  // can reach here.
  if (clauses.rows() == 0) {
    gen.B.createUnreachable(stmt);
    return;
  }
  
  // If the first rows are all wildcards (or there are no columns), then
  // try guards. ExprPatterns are treated as wildcards with guards.
  unsigned r = 0, rows = clauses.rows();
  for (; r < rows; ++r) {
    ClauseRow row = clauses[r];
    bool wildcardRow = true;
    for (unsigned c = 0, cols = clauses.columns(); c < cols; ++c) {
      if (!isWildcardPattern(row[c])) {
        wildcardRow = false;
        break;
      }
    }
  
    if (!wildcardRow)
      break;
  
    // If the row has a guard, emit it, and try the next row if it fails.
    if (row.emitDispatch(gen, caseMap))
      // If the row is irrefutable, we're done.
      return;
  }
  
  // If there are no rows remaining, fail. This will be a dataflow error if we
  // can reach here.
  if (r >= rows) {
    gen.B.createUnreachable(stmt);
    return;
  }

  // If we get this far, we have at least one non-wildcard row with at least
  // one column.
  assert(clauses.rows() >= 1 && clauses.columns() >= 1 &&
         "empty clause matrices should have been handled above");
  
  // Specialize on the next necessary column to continue testing the match.
  // TODO: We should choose the necessary column using one or more of Maranget's
  // heuristics and specialize on that column. For now we just do naive
  // left-to-right specialization.
  SmallVector<const Pattern *, 4> specialized;
  unsigned skipRows = r;
  
  auto isOrthogonalToSpecialized = [&](const Pattern *p) -> bool {
    for (auto s : specialized)
      if (!arePatternsOrthogonal(p, s))
        return false;
    return true;
  };

  for (; r < rows; ++r) {
    const Pattern *p = clauses[r][0];
    if (isWildcardPattern(p))
      continue;
    // If we've seen a constructor non-orthogonal to this one, skip it.
    if (!isOrthogonalToSpecialized(p))
      continue;
    
    specialized.push_back(p);
    
    SILBasicBlock *nextBB;

    // Create a nested scope and cont bb to clean up var bindings exposed by
    // specializing the matrix.
    SILBasicBlock *innerContBB = new(gen.F.getModule()) SILBasicBlock(&gen.F);
    {
      Scope patternVarScope(gen.Cleanups);
    
      auto specialization = clauses.emitSpecializedBranch(gen, p, r,
                                  gen.Cleanups.getCleanupsDepth(), innerContBB);
      ClauseMatrix &submatrix = specialization.first;
      nextBB = specialization.second;
      // Emit the submatrix into the true branch of the specialization.
      emitDecisionTree(gen, stmt, std::move(submatrix), caseMap, innerContBB);
      assert(!gen.B.hasValidInsertionPoint()
             && "recursive emitDecisionTree did not terminate all its BBs");
      
      // Emit scope cleanups into the continuation BB.
      gen.B.emitBlock(innerContBB);
    }
    // Chain the inner continuation to the outer.
    gen.B.createBranch(SILLocation(), contBB);
    
    // Continue dispatch on the false branch, if any.
    if (nextBB)
      gen.B.emitBlock(nextBB);
  }
  
  // If the set of specialized constructors form a signature for the type (i.e.,
  // they're exhaustive), then we're done.
  if (patternsFormSignature(specialized)) {
    // FIXME: Kill this BB.
    gen.B.createUnreachable(stmt);
    return;
  }
  
  // Otherwise, recur into the default matrix.
  clauses.reduceToDefault(skipRows);
  return emitDecisionTree(gen, stmt, std::move(clauses), caseMap, contBB);
}

void SILGenFunction::emitSwitchStmt(SwitchStmt *S) {
  Scope OuterSwitchScope(Cleanups);
  
  // Emit the subject value.
  ManagedValue subject = visit(S->getSubjectExpr()).getAsSingleValue(*this);
  
  // Prepare a case-to-bb mapping for fallthrough to consult.
  // The bbs for reachable cases will be filled in by emitDecisionTree below.
  CaseMap caseMap;

  // Create a scope to contain pattern variables.
  {
    Scope patternVarScope(Cleanups);

    // Create a continuation bb for life after the switch.
    SILBasicBlock *contBB = new (F.getModule()) SILBasicBlock(&F);
    
    // Set up an initial clause matrix.
    ClauseMatrix clauses(subject.getValue(), S->getCases().size());
    
    for (auto *caseBlock : S->getCases()) {
      caseMap[caseBlock] = {};
      for (auto *label : caseBlock->getCaseLabels())
        for (auto *pattern : label->getPatterns())
          clauses.addRow(caseBlock, label->getGuardExpr(), pattern,
                         CleanupsDepth::invalid(), contBB);
    }
  
    // Bind variable bindings from the topmost pattern nodes.
    clauses.emitVarsInColumn(*this, 0);
    
    // Update the case scopes to include the bound variables.
    for (unsigned r = 0, e = clauses.rows(); r < e; ++r) {
      clauses[r].setCleanupsDepth(Cleanups.getCleanupsDepth());
    }

    // Emit the decision tree.
    emitDecisionTree(*this, S, std::move(clauses), caseMap, contBB);
    assert(!B.hasValidInsertionPoint() &&
           "emitDecisionTree did not terminate all its BBs");

    // Emit top-level cleanups into the continuation BB.
    B.emitBlock(contBB);
  }
}
