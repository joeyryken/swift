//===--- Expr.cpp - Swift Language Expression ASTs ------------------------===//
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
//
//  This file implements the Expr class and subclasses.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Expr.h"
#include "swift/AST/AST.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Decl.h"
#include "swift/AST/PrettyStackTrace.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"
using namespace swift;

//===----------------------------------------------------------------------===//
// Expr methods.
//===----------------------------------------------------------------------===//

// Only allow allocation of Stmts using the allocator in ASTContext.
void *Expr::operator new(size_t Bytes, ASTContext &C,
                         unsigned Alignment) {
  return C.Allocate(Bytes, Alignment);
}

// Helper functions to verify statically whether the getSourceRange()
// function has been overridden.
typedef const char (&TwoChars)[2];

template<typename Class> 
inline char checkSourceRangeType(SourceRange (Class::*)() const);

inline TwoChars checkSourceRangeType(SourceRange (Expr::*)() const);

SourceRange Expr::getSourceRange() const {
  switch (Kind) {
#define EXPR(ID, PARENT) \
case ExprKind::ID: \
static_assert(sizeof(checkSourceRangeType(&ID##Expr::getSourceRange)) == 1, \
              #ID "Expr is missing getSourceRange()"); \
return cast<ID##Expr>(this)->getSourceRange();
#include "swift/AST/ExprNodes.def"
  }
  
  llvm_unreachable("expression type not handled!");
}

/// getLoc - Return the caret location of the expression.
SourceLoc Expr::getLoc() const {
  switch (Kind) {
#define EXPR(ID, PARENT) \
  case ExprKind::ID: \
    if (&Expr::getLoc != &ID##Expr::getLoc) \
      return cast<ID##Expr>(this)->getLoc(); \
    break;
#include "swift/AST/ExprNodes.def"
  }

  return getStartLoc();
}

Expr *Expr::getSemanticsProvidingExpr() {
  if (ParenExpr *PE = dyn_cast<ParenExpr>(this))
    return PE->getSubExpr()->getSemanticsProvidingExpr();
      
  return this;
}

Expr *Expr::getValueProvidingExpr() {
  // For now, this is totally equivalent to the above.
  // TODO:
  //   - tuple literal projection, which may become interestingly idiomatic
  return getSemanticsProvidingExpr();
}

bool Expr::isImplicit() const {
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(this))
    return !DRE->getLoc().isValid();
  
  if (const ImplicitConversionExpr *ICE
        = dyn_cast<ImplicitConversionExpr>(this))
    return ICE->getSubExpr()->isImplicit();
  
  return false;
}

//===----------------------------------------------------------------------===//
// Support methods for Exprs.
//===----------------------------------------------------------------------===//

APInt IntegerLiteralExpr::getValue() const {
  assert(!getType().isNull() && "Semantic analysis has not completed");
  unsigned BitWidth = getType()->castTo<BuiltinIntegerType>()->getBitWidth();
  
  llvm::APInt Value(BitWidth, 0);
  bool Error = getText().getAsInteger(0, Value);
  assert(!Error && "Invalid IntegerLiteral formed"); (void)Error;
  if (Value.getBitWidth() != BitWidth)
    Value = Value.zextOrTrunc(BitWidth);
  return Value;
}

llvm::APFloat FloatLiteralExpr::getValue() const {
  assert(!getType().isNull() && "Semantic analysis has not completed");
  
  APFloat Val(getType()->castTo<BuiltinFloatType>()->getAPFloatSemantics());
  APFloat::opStatus Res =
    Val.convertFromString(getText(), llvm::APFloat::rmNearestTiesToEven);
  assert(Res != APFloat::opInvalidOp && "Sema didn't reject invalid number");
  (void)Res;
  return Val;
}

MemberRefExpr::MemberRefExpr(Expr *Base, SourceLoc DotLoc, VarDecl *Value,
                             SourceLoc NameLoc)
  : Expr(ExprKind::MemberRef, Value->getTypeOfReference()), Base(Base),
    Value(Value), DotLoc(DotLoc), NameLoc(NameLoc) { }


Type OverloadSetRefExpr::getBaseType() const {
  if (isa<OverloadedDeclRefExpr>(this))
    return Type();
  if (const OverloadedMemberRefExpr *DRE
      = dyn_cast<OverloadedMemberRefExpr>(this)) {
    Type BaseTy = DRE->getBase()->getType();
    
    // Metatype types aren't considered to be base types.
    // FIXME:: If metatypes stop being singletons, we'll have to change this
    // and update all callers.
    if (BaseTy->is<MetaTypeType>())
      return Type();
    
    return BaseTy;
  }
  
  llvm_unreachable("Unhandled overloaded set reference expression");
}

Expr *OverloadSetRefExpr::createFilteredWithCopy(ArrayRef<ValueDecl *> Decls) {
  if (OverloadedDeclRefExpr *DRE = dyn_cast<OverloadedDeclRefExpr>(this))
    return OverloadedDeclRefExpr::createWithCopy(Decls, DRE->getLoc());
  if (OverloadedMemberRefExpr *DRE = dyn_cast<OverloadedMemberRefExpr>(this))
    return OverloadedMemberRefExpr::createWithCopy(DRE->getBase(),
                                                   DRE->getDotLoc(), Decls,
                                                   DRE->getMemberLoc());
  
  llvm_unreachable("Unhandled overloaded set reference expression");
}

/// createWithCopy - Create and return a new OverloadedDeclRefExpr or a new
/// DeclRefExpr (if the list of decls has a single entry) from the specified
/// (non-empty) list of decls.  If we end up creating an overload set, this
/// method handles copying the list of decls into ASTContext memory.
Expr *OverloadedDeclRefExpr::createWithCopy(ArrayRef<ValueDecl*> Decls,
                                            SourceLoc Loc) {
  assert(!Decls.empty() &&
         "Cannot create a decl ref with an empty list of decls");
  ASTContext &C = Decls[0]->getASTContext();
  if (Decls.size() == 1)
    return new (C) DeclRefExpr(Decls[0], Loc, Decls[0]->getTypeOfReference());
  
  // Otherwise, copy the overload set into ASTContext memory and return the
  // overload set.
  return new (C) OverloadedDeclRefExpr(C.AllocateCopy(Decls), Loc,
                                       UnstructuredDependentType::get(C));
}

Expr *OverloadedMemberRefExpr::createWithCopy(Expr *Base, SourceLoc DotLoc,
                                              ArrayRef<ValueDecl*> Decls,
                                              SourceLoc MemberLoc) {
  assert(!Decls.empty() &&
         "Cannot create an overloaded member ref with no decls");
  ASTContext &C = Decls[0]->getASTContext();

  if (Decls.size() == 1) {
    Expr *Fn = new (C) DeclRefExpr(Decls[0], MemberLoc,
                                   Decls[0]->getTypeOfReference());
    // FIXME: If metatype types ever get a runtime representation, we'll need
    // to evaluate the object.
    if (Decls[0]->isInstanceMember() &&
        !Base->getType()->is<MetaTypeType>()) {
      if (isa<FuncDecl>(Decls[0]))
        return new (C) DotSyntaxCallExpr(Fn, DotLoc, Base);
      
      VarDecl *Var = cast<VarDecl>(Decls[0]);
      return new (C) MemberRefExpr(Base, DotLoc, Var, MemberLoc);
    }
    
    return new (C) DotSyntaxBaseIgnoredExpr(Base, DotLoc, Fn);
  }
  
  // Otherwise, copy the overload set into the ASTContext's memory.
  return new (C) OverloadedMemberRefExpr(Base, DotLoc, C.AllocateCopy(Decls),
                                         MemberLoc,
                                         UnstructuredDependentType::get(C));
}

SequenceExpr *SequenceExpr::create(ASTContext &ctx, ArrayRef<Expr*> elements) {
  void *Buffer = ctx.Allocate(sizeof(SequenceExpr) +
                              elements.size() * sizeof(Expr*),
                              Expr::Alignment);
  return ::new(Buffer) SequenceExpr(elements);
}

NewArrayExpr *NewArrayExpr::create(ASTContext &ctx, SourceLoc newLoc,
                                   Type elementTy, ArrayRef<Bound> bounds) {
  void *buffer = ctx.Allocate(sizeof(NewArrayExpr) +
                              bounds.size() * sizeof(Bound),
                              Expr::Alignment);
  NewArrayExpr *E =
    ::new(buffer) NewArrayExpr(newLoc, elementTy, bounds.size(), Type());
  memcpy(E->getBoundsBuffer(), bounds.data(), bounds.size() * sizeof(Bound));
  return E;
}

SourceRange TupleExpr::getSourceRange() const {
  if (LParenLoc.isValid()) {
    assert(RParenLoc.isValid() && "Mismatched parens?");
    return SourceRange(LParenLoc, RParenLoc);
  }
  assert(getNumElements() == 2 && "Unexpected tuple expr");
  SourceLoc Start = getElement(0)->getStartLoc();
  SourceLoc End = getElement(1)->getEndLoc();
  return SourceRange(Start, End);
}

SubscriptExpr::SubscriptExpr(Expr *Base, SourceLoc LBracketLoc, Expr *Index,
                             SourceLoc RBracketLoc, SubscriptDecl *D)
  : Expr(ExprKind::Subscript, D? D->getElementType() : Type()),
    D(D), Brackets(LBracketLoc, RBracketLoc), Base(Base), Index(Index) { }

Expr *OverloadedSubscriptExpr::createWithCopy(Expr *Base,
                                              ArrayRef<ValueDecl*> Decls,
                                              SourceLoc LBracketLoc,
                                              Expr *Index,
                                              SourceLoc RBracketLoc) {
  assert(!Decls.empty() &&
         "Cannot create an overloaded member ref with no decls");
  ASTContext &C = Decls[0]->getASTContext();
  
  if (Decls.size() == 1)
    return new (C) SubscriptExpr(Base, LBracketLoc, Index, RBracketLoc);
  
  // Otherwise, copy the overload set into the ASTContext's memory.
  return new (C) OverloadedSubscriptExpr(Base, C.AllocateCopy(Decls),
                                         LBracketLoc, Index, RBracketLoc,
                                         UnstructuredDependentType::get(C));
}

FuncExpr *FuncExpr::create(ASTContext &C, SourceLoc funcLoc,
                           ArrayRef<Pattern*> params, Type fnType,
                           BraceStmt *body, DeclContext *parent) {
  unsigned nParams = params.size();
  void *buf = C.Allocate(sizeof(FuncExpr) + nParams * sizeof(Pattern*),
                         Expr::Alignment);
  FuncExpr *fn = ::new(buf) FuncExpr(funcLoc, nParams, fnType, body, parent);
  for (unsigned i = 0; i != nParams; ++i)
    fn->getParamsBuffer()[i] = params[i];
  return fn;
}

SourceRange FuncExpr::getSourceRange() const {
  return SourceRange(FuncLoc, Body->getEndLoc());
}

/// Returns the result type of the function defined by the body.  For
/// an uncurried function, this is just the normal result type; for a
/// curried function, however, this is the result type of the
/// uncurried part.
///
/// Examples:
///   func(x : int) -> ((y : int) -> (int -> int))
///     The body result type is '((y : int) -> (int -> int))'.
///   func(x : int) -> (y : int) -> (int -> int)
///     The body result type is '(int -> int)'.
Type FuncExpr::getBodyResultType() const {
  unsigned n = getParamPatterns().size();
  Type ty = getType();
  do {
    ty = cast<FunctionType>(ty)->getResult();
  } while (--n);
  return ty;
}

static ValueDecl *getCalledValue(Expr *E) {
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  Expr *E2 = E->getValueProvidingExpr();
  if (E != E2) return getCalledValue(E2);
  return nullptr;
}

ValueDecl *ApplyExpr::getCalledValue() const {
  return ::getCalledValue(Fn);
}

void ExplicitClosureExpr::GenerateVarDecls(unsigned NumDecls,
                                           std::vector<VarDecl*> &Decls,
                                           ASTContext &Context) {
  while (NumDecls >= Decls.size()) {
    unsigned NextIdx = Decls.size();
    llvm::SmallVector<char, 4> StrBuf;
    StringRef VarName = ("$" + Twine(NextIdx)).toStringRef(StrBuf);
    Identifier ident = Context.getIdentifier(VarName);
    SourceLoc VarLoc; // FIXME: Location?
    VarDecl *var = new (Context) VarDecl(VarLoc, ident, Type(), this);
    Decls.push_back(var);
  }
}

//===----------------------------------------------------------------------===//
// Printing for Expr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintExpr - Visitor implementation of Expr::print.
class PrintExpr : public ExprVisitor<PrintExpr> {
public:
  raw_ostream &OS;
  unsigned Indent;
  
  PrintExpr(raw_ostream &os, unsigned indent) : OS(os), Indent(indent) {
  }
  
  void printRec(Expr *E) {
    Indent += 2;
    if (E)
      visit(E);
    else
      OS.indent(Indent) << "(**NULL EXPRESSION**)";
    Indent -= 2;
  }
  
  /// FIXME: This should use ExprWalker to print children.
  
  void printRec(Decl *D) { D->print(OS, Indent+2); }
  void printRec(Stmt *S) { S->print(OS, Indent+2); }

  raw_ostream &printCommon(Expr *E, const char *C) {
    return OS.indent(Indent) << '(' << C << " type='" << E->getType() << '\'';
  }
  
  void visitErrorExpr(ErrorExpr *E) {
    printCommon(E, "error_expr") << ')';
  }

  void visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    printCommon(E, "integer_literal_expr") << " value=";
    if (E->getType().isNull() || E->getType()->isDependentType())
      OS << E->getText();
    else
      OS << E->getValue();
    OS << ')';
  }
  void visitFloatLiteralExpr(FloatLiteralExpr *E) {
    printCommon(E, "float_literal_expr") << " value=" << E->getText() << ')';
  }
  void visitCharacterLiteralExpr(CharacterLiteralExpr *E) {
    printCommon(E, "character_literal_expr") << " value=" << E->getValue()<<')';
  }
  void visitStringLiteralExpr(StringLiteralExpr *E) {
    printCommon(E, "string_literal_expr") << " value=" << E->getValue() << ')';
  }
  void visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *E) {
    printCommon(E, "interpolated_string_literal_expr") << '\n';
    for (auto Segment : E->getSegments()) {
      printRec(Segment);
      OS << '\n';
    }
    OS << ')';
  }
  void visitDeclRefExpr(DeclRefExpr *E) {
    printCommon(E, "declref_expr")
      << " decl=" << E->getDecl()->getName() << ')';
  }
  void visitOverloadedDeclRefExpr(OverloadedDeclRefExpr *E) {
    printCommon(E, "overloadeddeclref_expr")
      << " #decls=" << E->getDecls().size();
    for (Decl *D : E->getDecls()) {
      OS << '\n';
      printRec(D);
    }
    OS << ')';
  }
  void visitOverloadedMemberRefExpr(OverloadedMemberRefExpr *E) {
    printCommon(E, "overloadedmemberref_expr")
      << "#decls=" << E->getDecls().size() << "\n"
      << "base = ";
    printRec(E->getBase());
    for (Decl *D : E->getDecls()) {
      OS << '\n';
      printRec(D);
    }
    OS << ')';
  }
  void visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *E) {
    printCommon(E, "unresolved_decl_ref_expr")
      << " name=" << E->getName() << ')';
  }
  void visitMemberRefExpr(MemberRefExpr *E) {
    printCommon(E, "member_ref_expr")
      << " decl=" << E->getDecl()->getName() << '\n';
    printRec(E->getBase());
    OS << ')';
  }
  
  void visitUnresolvedMemberExpr(UnresolvedMemberExpr *E) {
    printCommon(E, "unresolved_member_expr")
      << " name='" << E->getName() << "')";
  }
  void visitParenExpr(ParenExpr *E) {
    printCommon(E, "paren_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitTupleExpr(TupleExpr *E) {
    printCommon(E, "tuple_expr");
    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      OS << '\n';
      if (E->getElement(i))
        printRec(E->getElement(i));
      else
        OS.indent(Indent+2) << "<<tuple element default value>>";
    }
    OS << ')';
  }
  void visitSubscriptExpr(SubscriptExpr *E) {
    printCommon(E, "subscript_expr");
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getIndex());
    OS << ')';
  }
  void visitOverloadedSubscriptExpr(OverloadedSubscriptExpr *E) {
    printCommon(E, "overloaded_subscript_expr");
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getIndex());
    OS << ')';
  }
  void visitUnresolvedDotExpr(UnresolvedDotExpr *E) {
    printCommon(E, "unresolved_dot_expr")
      << " field '" << E->getName().str() << "'";
    if (E->getBase()) {
      OS << '\n';
      printRec(E->getBase());
    }
    OS << ')';
  }
  void visitModuleExpr(ModuleExpr *E) {
    printCommon(E, "module_expr") << ')';
  }
  void visitSyntacticTupleElementExpr(TupleElementExpr *E) {
    printCommon(E, "syntactic_tuple_element_expr")
      << " field #" << E->getFieldNumber() << '\n';
    printRec(E->getBase());
    OS << ')';
  }
  void visitImplicitThisTupleElementExpr(TupleElementExpr *E) {
    printCommon(E, "implicit_this_tuple_element_expr")
    << " field #" << E->getFieldNumber() << '\n';
    printRec(E->getBase());
    OS << ')';
  }
  

  void visitTupleShuffleExpr(TupleShuffleExpr *E) {
    printCommon(E, "tuple_shuffle_expr") << " elements=[";
    for (unsigned i = 0, e = E->getElementMapping().size(); i != e; ++i) {
      if (i) OS << ", ";
      OS << E->getElementMapping()[i];
    }
    OS << "]\n";
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitLookThroughOneofExpr(LookThroughOneofExpr *E) {
    printCommon(E, "look_through_oneof_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitParameterRenameExpr(ParameterRenameExpr *E) {
    printCommon(E, "parameter_rename_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitScalarToTupleExpr(ScalarToTupleExpr *E) {
    printCommon(E, "scalar_to_tuple_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitLoadExpr(LoadExpr *E) {
    printCommon(E, "load_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitMaterializeExpr(MaterializeExpr *E) {
    printCommon(E, "materialize_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitRequalifyExpr(RequalifyExpr *E) {
    printCommon(E, "requalify_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }

  void visitAddressOfExpr(AddressOfExpr *E) {
    printCommon(E, "address_of_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitSequenceExpr(SequenceExpr *E) {
    printCommon(E, "sequence_expr") << '\n';
    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      OS << '\n';
      printRec(E->getElement(i));
    }
    OS << ')';
  }
  void visitFuncExpr(FuncExpr *E) {
    printCommon(E, "func_expr") << '\n';
    printRec(E->getBody());
    OS << ')';
  }
  void visitExplicitClosureExpr(ExplicitClosureExpr *E) {
    printCommon(E, "explicit_closure_expr") << '\n';
    printRec(E->getBody());
    OS << ')';
  }
  void visitImplicitClosureExpr(ImplicitClosureExpr *E) {
    printCommon(E, "implicit_closure_expr") << '\n';
    printRec(E->getBody());
    OS << ')';
  }

  void visitNewArrayExpr(NewArrayExpr *E) {
    printCommon(E, "new_array_expr")
      << " elementType='" << E->getElementType() << "'\n";
    for (auto &bound : E->getBounds())
      printRec(bound.Value);
    OS << ')';
  }
  
  void printApplyExpr(ApplyExpr *E, const char *NodeName) {
    printCommon(E, NodeName) << '\n';
    printRec(E->getFn());
    OS << '\n';
    printRec(E->getArg());
    OS << ')';
  }
  
  void visitCallExpr(CallExpr *E) {
    printApplyExpr(E, "call_expr");
  }
  void visitUnaryExpr(UnaryExpr *E) {
    printApplyExpr(E, "unary_expr");
  }
  void visitBinaryExpr(BinaryExpr *E) {
    printApplyExpr(E, "binary_expr");
  }
  void visitConstructorCallExpr(ConstructorCallExpr *E) {
    printApplyExpr(E, "constructor_call_expr");
  }
  void visitDotSyntaxCallExpr(DotSyntaxCallExpr *E) {
    printApplyExpr(E, "dot_syntax_call_expr");
  }
  void visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *E) {
    printCommon(E, "dot_syntax_base_ignored") << '\n';
    printRec(E->getLHS());
    OS << '\n';
    printRec(E->getRHS());
    OS << ')';
  }
  void visitCoerceExpr(CoerceExpr *E) {
    printCommon(E, "coerce_expr") << '\n';
    printRec(E->getLHS());
    OS << '\n';
    printRec(E->getRHS());
    OS << ')';
  }
};

} // end anonymous namespace.


void Expr::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

void Expr::print(raw_ostream &OS, unsigned Indent) const {
  PrintExpr(OS, Indent).visit(const_cast<Expr*>(this));
}
