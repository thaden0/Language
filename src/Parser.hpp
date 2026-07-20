#pragma once
#include "Ast.hpp"
#include "Diagnostic.hpp"
#include "Token.hpp"
#include <vector>

// Hand-written recursive-descent parser with a Pratt expression core.
// Operator precedence lives in a runtime table (infixBP) rather than being
// baked into a grammar, which is what will let user-defined operators (§5)
// slot in later. Errors are reported to the sink; the parser recovers rather
// than throwing.
class Parser {
public:
    Parser(std::vector<Token> tokens, const SourceFile& file, DiagnosticSink& sink)
        : tokens_(std::move(tokens)), file_(file), sink_(sink) {}

    Program parseProgram();

    // F4 heap-string fragment seam used only by RuleEngine's comptime parser
    // hook. The SourceFile supplied to this Parser owns all resulting views.
    ExprPtr parseExpressionFragment();
    std::vector<StmtPtr> parseStatementsFragment();

private:
    std::vector<Token> tokens_;
    const SourceFile& file_;
    DiagnosticSink& sink_;
    size_t pos_ = 0;

    // --- token cursor ---
    const Token& peek(size_t ahead = 0) const;
    const Token& cur() const { return peek(0); }
    bool at(TokenKind k) const { return cur().kind == k; }
    bool atEnd() const { return cur().kind == TokenKind::End; }
    const Token& advance();
    bool accept(TokenKind k);                 // consume if matches
    bool expect(TokenKind k, const char* what);
    bool expectGt();                           // closes generics, splitting '>>' into two '>'
    void error(const char* message);
    void synchronize();                        // recover to a statement boundary

    // --- types ---
    TypeRefPtr parseType();
    TypeRefPtr parseTypePrimary();
    TypeRefPtr wrapNullable(TypeRefPtr t);   // apply a trailing `?` suffix (T? == T | None)
    bool skipTypeLA(size_t& i) const;        // lookahead: advance past a type (typed-lambda-param detect)
    bool typedParamAhead() const;            // do the tokens here read as `type name`?
    bool looksLikeType() const;

    // --- expressions (Pratt) ---
    ExprPtr parseExpr(int minBP = 0);
    ExprPtr parseUnary();
    ExprPtr parsePostfix(ExprPtr base);
    ExprPtr parsePrimary();
    // F4: `"...${expr}..."` desugars to a `+`-chain of literal segments and
    // `(hole).toString()` calls, built entirely from the raw token text —
    // scans for unescaped `${...}` holes (tracking brace depth and nested-
    // string quote state so `}` inside a nested string/call doesn't
    // prematurely close the hole), sub-lexes/sub-parses each hole's exact
    // source range (real spans, via Lexer::tokenizeRange — the same
    // mechanism quasiquote fragments use), and reconstructs each literal
    // segment as an ordinary StringLit node (raw bytes, undecoded — decoding
    // happens later, uniformly, via decodeStringLiteral). Zero holes ->
    // reconstructs byte-identical to `t.text` by construction (substr copy,
    // no special case).
    ExprPtr parseInterpolatedString(const Token& t);
    ExprPtr parseParenOrLambda();
    ExprPtr parseMatch();
    std::vector<ExprPtr> parseArgs();          // ( a, b, c )
    std::vector<TypeRefPtr> parseExplicitTypeArgs(); // ::<T, U>
    std::vector<ExprPtr> parseMacroArgs();     // adds scoped `...` raw strings
    bool looksLikeLambda() const;
    ExprPtr parseArrayElement();               // an array literal element, incl. $for (§5)
    // A statement/decl/member-position `$for` splice inside a quasiquote
    // fragment (LA-4 item J). `body` selects how the per-iteration body parses.
    enum class SpliceBody { Stmt, Item, Member };
    StmtPtr parseForSpliceStmt(SpliceBody body);
    // One element of a quasiquote fragment: dispatches a reserved hole-head
    // (`$for` -> ForSplice, `$if` -> ForkSplice) or the ordinary statement/item/
    // member parser for `body`. `$else` here (no preceding `$if`) is M41.
    StmtPtr parseFragmentStmt(SpliceBody body);
    // Conditional splice (B2, techdesign-splices-conditional): `$if (cond) <frag>
    // ( $else if (cond) <frag> )* ( $else <frag> )?`. `parseForkSpliceStmt`
    // consumes the leading `$if`; `parseForkTail` parses `(cond) <frag> [else]`
    // (also entered for each desugared `$else if`). A `<frag>` is a brace group
    // of the same fragment kind as `body`, wrapped in a Block by parseForkBranch.
    StmtPtr parseForkSpliceStmt(SpliceBody body);
    StmtPtr parseForkTail(SpliceBody body, SourceSpan sp);
    StmtPtr parseForkBranch(SpliceBody body);
    // Expression-position `$if (cond) { e } $else { e }` (array element). The
    // leading `$if` is already consumed by the caller. a=cond, b=then, c=else.
    ExprPtr parseForkSpliceExpr(SourceSpan sp);
    ExprPtr parseForkBranchExpr();

    // --- statements ---
    StmtPtr parseStatement();                  // also the "body is one statement" routine
    StmtPtr parseBlock();
    StmtPtr parseVarDecl();
    bool looksLikeVarDecl() const;

    // --- declarations ---
    StmtPtr parseTopLevelItem();
    StmtPtr parseTopLevelItemInner();
    StmtPtr parseNamespace();
    StmtPtr parseClass(Access access, bool isInterface, bool isValue = false);
    StmtPtr parseEnum(Access access);              // `enum Name : carrier { M, M = v }` (Track 03 §2)
    StmtPtr parseClassMember(Access sectionAccess, bool sectionConst = false);
    StmtPtr parseClassMemberInner(Access sectionAccess, bool sectionConst = false);
    StmtPtr parseBind();
    void parsePath(std::vector<std::string_view>& out);   // '::'-separated identifiers
    StmtPtr parseUses();
    StmtPtr parseUse();
    std::vector<Param> parseParamList();
    void parseTypeParams(std::vector<std::string_view>& out);   // <T, U> on class/fn/method

    // --- metaprogramming surface (§16.5) ---
    std::vector<AttrUse> parseAttrUses();          // `@Name(args)`* before a decl
    StmtPtr parseAttributeDecl(Access access);     // `attribute Name { fields }`
    StmtPtr tryParseComptime();                    // comptime var / if / expr-stmt
    bool nextStartsExpr(size_t ahead) const;       // token can begin an expression
    bool sawMeta_ = false;                         // any @/attribute/comptime/rule seen

    // --- rules (§16.5 Layer B) ---
    StmtPtr parseRule(Access access);              // `rule Name { match ... inject ... }`
    void parseRuleMatch(RuleMatch& out);           // the match clause
    void parseRuleAction(RuleAction& out);         // one inject clause + anchor
    // --- expression macros (Phase 3 §7) ---
    StmtPtr parseMacroDecl(Access access);         // `macro Name(params) => \`expr\`;`
    // Fragment parsers: re-lex a quasiquote's payload (with holes) into AST.
    std::vector<StmtPtr> parseStmtsFragment(SourceSpan quasi);
    StmtPtr parseMemberFragment(SourceSpan quasi);
    ExprPtr parseExprFragment(SourceSpan quasi);
    std::vector<StmtPtr> parseItemsFragment(SourceSpan quasi);   // `namespace N` (§8.3)
};
