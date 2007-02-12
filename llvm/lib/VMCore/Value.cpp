//===-- Value.cpp - Implement the Value class -----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Value and User classes.
//
//===----------------------------------------------------------------------===//

#include "llvm/Constant.h"
#include "llvm/DerivedTypes.h"
#include "llvm/InstrTypes.h"
#include "llvm/Module.h"
#include "llvm/ValueSymbolTable.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/LeakDetector.h"
#include <algorithm>
using namespace llvm;

//===----------------------------------------------------------------------===//
//                                Value Class
//===----------------------------------------------------------------------===//

static inline const Type *checkType(const Type *Ty) {
  assert(Ty && "Value defined with a null type: Error!");
  return Ty;
}

Value::Value(const Type *ty, unsigned scid)
  : SubclassID(scid), SubclassData(0), Ty(checkType(ty)),
    UseList(0), Name(0) {
  if (!isa<Constant>(this) && !isa<BasicBlock>(this))
    assert((Ty->isFirstClassType() || Ty == Type::VoidTy ||
           isa<OpaqueType>(ty)) &&
           "Cannot create non-first-class values except for constants!");
}

Value::~Value() {
#ifndef NDEBUG      // Only in -g mode...
  // Check to make sure that there are no uses of this value that are still
  // around when the value is destroyed.  If there are, then we have a dangling
  // reference and something is wrong.  This code is here to print out what is
  // still being referenced.  The value in question should be printed as
  // a <badref>
  //
  if (use_begin() != use_end()) {
    DOUT << "While deleting: " << *Ty << " %" << Name << "\n";
    for (use_iterator I = use_begin(), E = use_end(); I != E; ++I)
      DOUT << "Use still stuck around after Def is destroyed:"
           << **I << "\n";
  }
#endif
  assert(use_begin() == use_end() && "Uses remain when a value is destroyed!");

  // There should be no uses of this object anymore, remove it.
  LeakDetector::removeGarbageObject(this);
}

/// hasNUses - Return true if this Value has exactly N users.
///
bool Value::hasNUses(unsigned N) const {
  use_const_iterator UI = use_begin(), E = use_end();

  for (; N; --N, ++UI)
    if (UI == E) return false;  // Too few.
  return UI == E;
}

/// hasNUsesOrMore - Return true if this value has N users or more.  This is
/// logically equivalent to getNumUses() >= N.
///
bool Value::hasNUsesOrMore(unsigned N) const {
  use_const_iterator UI = use_begin(), E = use_end();

  for (; N; --N, ++UI)
    if (UI == E) return false;  // Too few.

  return true;
}


/// getNumUses - This method computes the number of uses of this Value.  This
/// is a linear time operation.  Use hasOneUse or hasNUses to check for specific
/// values.
unsigned Value::getNumUses() const {
  return (unsigned)std::distance(use_begin(), use_end());
}

static bool getSymTab(Value *V, ValueSymbolTable *&ST) {
  ST = 0;
  if (Instruction *I = dyn_cast<Instruction>(V)) {
    if (BasicBlock *P = I->getParent())
      if (Function *PP = P->getParent())
        ST = &PP->getValueSymbolTable();
  } else if (BasicBlock *BB = dyn_cast<BasicBlock>(V)) {
    if (Function *P = BB->getParent()) 
      ST = &P->getValueSymbolTable();
  } else if (GlobalValue *GV = dyn_cast<GlobalValue>(V)) {
    if (Module *P = GV->getParent()) 
      ST = &P->getValueSymbolTable();
  } else if (Argument *A = dyn_cast<Argument>(V)) {
    if (Function *P = A->getParent()) 
      ST = &P->getValueSymbolTable();
  } else {
    assert(isa<Constant>(V) && "Unknown value type!");
    return true;  // no name is setable for this.
  }
  return false;
}

std::string Value::getName() const {
  if (Name == 0) return "";
  return std::string(Name->getKeyData(),
                     Name->getKeyData()+Name->getKeyLength());
}

void Value::setName(const std::string &name) {
  if (name.empty() && !hasName()) return;
  if (getType() != Type::VoidTy && "Cannot assign a name to void values!");
  
  
  // Get the symbol table to update for this object.
  ValueSymbolTable *ST;
  if (getSymTab(this, ST))
    return;  // Cannot set a name on this value (e.g. constant).

  if (!ST) { // No symbol table to update?  Just do the change.
    if (name.empty()) {
      // Free the name for this value.
      Name->Destroy();
      Name = 0;
    } else {
      if (Name) {
        // Name isn't changing.
        if (name.size() == Name->getKeyLength() &&
            !memcmp(Name->getKeyData(), &name[0], name.size()))
          return;
        Name->Destroy();
      }
      
      // Create the new name.
      Name = ValueName::Create(&name[0], &name[name.size()]);
      Name->setValue(this);
    }
    return;
  }
  
  // NOTE: Could optimize for the case the name is shrinking to not deallocate
  // then reallocated.
  if (hasName()) {
    // Name isn't changing?
    if (name.size() == Name->getKeyLength() &&
        !memcmp(Name->getKeyData(), &name[0], name.size()))
      return;

    // Remove old name.
    ST->removeValueName(Name);
    Name->Destroy();
    Name = 0;

    if (name.empty())
       return;
  }

  // Name is changing to something new.
  Name = ST->createValueName(&name[0], name.size(), this);
}

/// takeName - transfer the name from V to this value, setting V's name to
/// empty.  It is an error to call V->takeName(V). 
void Value::takeName(Value *V) {
  if (!V->hasName()) {
    if (hasName())
      setName("");
    return;
  }
  
  std::string Name = V->getName();
  V->setName("");
  setName(Name);
}


// uncheckedReplaceAllUsesWith - This is exactly the same as replaceAllUsesWith,
// except that it doesn't have all of the asserts.  The asserts fail because we
// are half-way done resolving types, which causes some types to exist as two
// different Type*'s at the same time.  This is a sledgehammer to work around
// this problem.
//
void Value::uncheckedReplaceAllUsesWith(Value *New) {
  while (!use_empty()) {
    Use &U = *UseList;
    // Must handle Constants specially, we cannot call replaceUsesOfWith on a
    // constant!
    if (Constant *C = dyn_cast<Constant>(U.getUser())) {
      if (!isa<GlobalValue>(C))
        C->replaceUsesOfWithOnConstant(this, New, &U);
      else
        U.set(New);
    } else {
      U.set(New);
    }
  }
}

void Value::replaceAllUsesWith(Value *New) {
  assert(New && "Value::replaceAllUsesWith(<null>) is invalid!");
  assert(New != this && "this->replaceAllUsesWith(this) is NOT valid!");
  assert(New->getType() == getType() &&
         "replaceAllUses of value with new value of different type!");

  uncheckedReplaceAllUsesWith(New);
}

//===----------------------------------------------------------------------===//
//                                 User Class
//===----------------------------------------------------------------------===//

// replaceUsesOfWith - Replaces all references to the "From" definition with
// references to the "To" definition.
//
void User::replaceUsesOfWith(Value *From, Value *To) {
  if (From == To) return;   // Duh what?

  assert(!isa<Constant>(this) || isa<GlobalValue>(this) &&
         "Cannot call User::replaceUsesofWith on a constant!");

  for (unsigned i = 0, E = getNumOperands(); i != E; ++i)
    if (getOperand(i) == From) {  // Is This operand is pointing to oldval?
      // The side effects of this setOperand call include linking to
      // "To", adding "this" to the uses list of To, and
      // most importantly, removing "this" from the use list of "From".
      setOperand(i, To); // Fix it now...
    }
}

