// refactor_1 session 03: shared declarations for the Checker.cpp split
// (designs/complete/refactor_1/techdesign-03-checker-split-sonnet.md).
//
// checkerDetail:: holds the handful of free-function helpers that used to be
// file-static / anonymous-namespace helpers in the monolithic Checker.cpp but
// are now called from more than one of the split translation units. Each is
// defined exactly once, in Checker.cpp (see the checkerDetail block there);
// every other TU only sees the declaration below. No static may be
// duplicated across TUs.
#pragma once
#include "sema/Checker.hpp"
#include <functional>

namespace checkerDetail {

Type unknown();
Type primitive(std::string c);
Type classType(Symbol* s);
Symbol* nsChainSym(Scope* scope, const Expr* e);
const char* opSymbol(TokenKind k);
bool isCompoundAssign(TokenKind k);
std::vector<const Slot*> slotsNamed(const Shape& sh, std::string_view name);
bool mentionsTypeParam(const TypeRef* t);
TypeRefPtr copyTypeRef(const TypeRef* t);
TypeRefPtr copyTypeRefWithSubst(
    const TypeRef* t, const std::unordered_map<std::string_view, Type>& subst);
void walkProperAncestors(const Symbol* X, const std::function<void(const Symbol*)>& f);
void resolveExprType(TypeRef* t, Scope* scope);
bool isCharLiteral(const Expr* e);
void markCharLiteral(const Expr* e);

}  // namespace checkerDetail
