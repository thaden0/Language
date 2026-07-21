#pragma once
#include "Parser.hpp"

// refactor_1 session 04: Parser.cpp split into Parser.cpp / ParserExpr.cpp /
// ParserMeta.cpp. Every Parser member function stays declared exactly once,
// in Parser.hpp, and is defined in whichever of the three .cpp files owns
// that concern (cross-TU member calls need nothing further).
//
// The three helpers below were file-static free functions in the original
// Parser.cpp with call sites that ended up split across more than one of the
// new TUs; a `static` can't be shared across translation units, so they are
// promoted to ordinary functions in this `parserDetail` namespace instead.
// All three are defined in Parser.cpp.
namespace parserDetail {

// A keyword that may also serve as a member name after '.' / '::' (get/set/
// is/in/use/uses/new/bind/inject). Shared by Parser::parsePostfix
// (ParserExpr.cpp) and Parser::parseClassMemberInner /
// Parser::parseTopLevelItemInner (Parser.cpp).
bool isContextualName(TokenKind k);

// Can a token begin an expression? Shared by Parser::parseExpr
// (ParserExpr.cpp) and Parser::tryParseComptime / Parser::nextStartsExpr
// (Parser.cpp).
bool canStartExpr(TokenKind k);

// Allocate a Stmt node with its span pre-filled. Shared by every
// statement/declaration constructor in Parser.cpp and by the rule/macro
// grammar in ParserMeta.cpp.
StmtPtr mkStmt(StmtKind k, SourceSpan sp);

}  // namespace parserDetail
