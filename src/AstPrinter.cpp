#include "AstPrinter.hpp"
#include <cassert>

namespace {

// LA-20 §8: the elidable-literal side channel for --expand, set once by
// printProgramSource before printing and read by srcString. A file-static
// rather than threading a parameter through every srcExpr/srcStmt* call
// site — source-shaped printing is single-call, non-reentrant.
const std::map<uint32_t, std::pair<std::string, size_t>>* g_importLits = nullptr;

const char* opSymbol(TokenKind k) {
    switch (k) {
        case TokenKind::Plus:     return "+";
        case TokenKind::Minus:    return "-";
        case TokenKind::Star:     return "*";
        case TokenKind::Slash:    return "/";
        case TokenKind::Percent:  return "%";
        case TokenKind::EqEq:     return "==";
        case TokenKind::BangEq:   return "!=";
        case TokenKind::Lt:       return "<";
        case TokenKind::Gt:       return ">";
        case TokenKind::Le:       return "<=";
        case TokenKind::Ge:       return ">=";
        case TokenKind::LtLt:     return "<<";
        case TokenKind::GtGt:     return ">>";
        case TokenKind::AmpAmp:   return "&&";
        case TokenKind::PipePipe: return "||";
        case TokenKind::Amp:      return "&";
        case TokenKind::Pipe:     return "|";
        case TokenKind::Eq:       return "=";
        case TokenKind::QuestionQuestion: return "??";
        case TokenKind::Bang:     return "!";
        default:                  return "?";
    }
}

std::string sv(std::string_view s) { return std::string(s); }

// techdesign-labeled-break-continue.md F2: the `--ast` dump suffix for a
// labeled loop/labeled break/continue (parser golden tests read this).
std::string labelSuffix(const Stmt* s) {
    return s->label.empty() ? "" : " label=" + std::string(s->label);
}
// Same design doc: the `--expand` source-reconstruction prefix (`label: `)
// on a labeled loop, so a rule-injected labeled loop + labeled break stays
// recompilable (corpus_meta_expand_roundtrip).
std::string labelPrefix(const Stmt* s) {
    return s->label.empty() ? "" : std::string(s->label) + ": ";
}

std::string typeStr(const TypeRef* t);

std::string typeList(const std::vector<TypeRefPtr>& ts, const char* sep) {
    std::string out;
    for (size_t i = 0; i < ts.size(); ++i) {
        if (i) out += sep;
        out += typeStr(ts[i].get());
    }
    return out;
}

std::string typeStr(const TypeRef* t) {
    if (!t) return "?";
    switch (t->kind) {
        case TypeKind::Inferred: return "var";
        case TypeKind::Union:    return typeList(t->members, " | ");
        case TypeKind::Function:
            return "(" + typeList(t->funcParams, ", ") + ") => " + typeStr(t->funcRet.get());
        case TypeKind::Named: {
            // R5 (005): emit the `path::` qualifier prefix so a qualified type
            // round-trips through `--ast`/`--expand` (`std::RuntimeException`,
            // and — once R2 produces them — qualified `match`-arm type patterns).
            std::string out;
            for (std::string_view seg : t->path) { out += sv(seg); out += "::"; }
            out += sv(t->name);
            if (!t->generics.empty()) out += "<" + typeList(t->generics, ", ") + ">";
            return out;
        }
    }
    return "?";
}

std::string exprStr(const Expr* e);
std::string srcString(const Expr* e);

std::string exprList(const std::vector<ExprPtr>& es) {
    std::string out;
    for (size_t i = 0; i < es.size(); ++i) {
        if (i) out += ", ";
        if (!es[i]->argLabel.empty()) out += sv(es[i]->argLabel) + ": ";
        out += exprStr(es[i].get());
    }
    return out;
}

std::string paramList(const std::vector<Param>& ps) {
    std::string out;
    for (size_t i = 0; i < ps.size(); ++i) {
        if (i) out += ", ";
        if (ps[i].type) out += typeStr(ps[i].type.get()) + " ";
        out += sv(ps[i].name);
        if (ps[i].defaultValue) out += " = " + exprStr(ps[i].defaultValue.get());
    }
    return out;
}

std::string exprStr(const Expr* e) {
    if (!e) return "<null>";
    switch (e->kind) {
        case ExprKind::IntLit:
        case ExprKind::FloatLit:
        case ExprKind::BoolLit:
        case ExprKind::Name:
            // LA-32 §4.6: a pinned generic VALUE reference (`identity::<int>`, no
            // call) carries explicitTypeArgs on the Name/Member itself. The
            // checker normally rewrites it to an eta-expansion lambda before
            // printing, but render it here too so an unchecked dump round-trips.
            if (!e->explicitTypeArgs.empty())
                return sv(e->text) + "::<" + typeList(e->explicitTypeArgs, ", ") + ">";
            return sv(e->text);
        case ExprKind::StringLit:
            // Unlike names and numeric literals, interpolation/quasiquote raw
            // segments store their contents without quote characters.  The
            // source printer already restores those delimiters; the debug AST
            // printer must do the same or punctuation-only strings look like
            // operators/attributes after rule expansion.
            return srcString(e);
        case ExprKind::This:     return "this";
        case ExprKind::Member: {
            std::string out = exprStr(e->a.get()) +
                              (e->colon ? "::" : e->optChain ? "?." : ".") + sv(e->text);
            if (!e->explicitTypeArgs.empty())   // LA-32 §4.6 value reference
                out += "::<" + typeList(e->explicitTypeArgs, ", ") + ">";
            return out;
        }
        case ExprKind::Call: {
            assert(!(e->isMacroCall && !e->explicitTypeArgs.empty()));
            std::string applied = e->explicitTypeArgs.empty()
                ? "" : "::<" + typeList(e->explicitTypeArgs, ", ") + ">";
            return exprStr(e->a.get()) + applied +
                   (e->isMacroCall ? "!(" : "(") + exprList(e->list) + ")";
        }
        case ExprKind::Index:
            return exprStr(e->a.get()) + "[" + exprStr(e->b.get()) + "]";
        case ExprKind::Unary:
            return std::string(opSymbol(e->op)) + exprStr(e->a.get());
        case ExprKind::Binary:
            return "(" + exprStr(e->a.get()) + " " + opSymbol(e->op) + " " +
                   exprStr(e->b.get()) + ")";
        case ExprKind::Ternary:
            return "(" + exprStr(e->a.get()) + " ? " + exprStr(e->b.get()) + " : " +
                   exprStr(e->c.get()) + ")";
        case ExprKind::Array:
            return "[" + exprList(e->list) + "]";
        case ExprKind::ForSplice:
            return "$for " + sv(e->text) + " in " + exprStr(e->a.get()) +
                   " : " + exprStr(e->b.get());
        case ExprKind::Lambda:
            return "(" + paramList(e->params) + ") => " +
                   (e->block ? "{ ... }" : exprStr(e->a.get()));
        case ExprKind::Await:
            return "await " + exprStr(e->a.get());
        case ExprKind::Inject:
            return "inject " + typeStr(e->type.get());
        case ExprKind::Extract:
            return "(" + exprStr(e->a.get()) + " >>)";
        case ExprKind::Range:
            return "(" + exprStr(e->a.get()) + " .. " + exprStr(e->b.get()) + ")";
        case ExprKind::Is:
            return "(" + exprStr(e->a.get()) + " is " + typeStr(e->type.get()) + ")";
        case ExprKind::Match: {
            std::string out = "match(" + exprStr(e->a.get()) + ") {";
            for (const MatchArm& arm : e->arms) {
                out += " ";
                if (arm.isElse) out += "else";
                else if (arm.type) out += typeStr(arm.type.get());
                else out += exprStr(arm.value.get());
                out += " => " + (arm.bodyBlock ? "{...}" : exprStr(arm.bodyExpr.get())) + ";";
            }
            return out + " }";
        }
    }
    return "?";
}

const char* accessStr(Access a) {
    switch (a) {
        case Access::Public:  return "public ";
        case Access::Private: return "private ";
        default:              return "";
    }
}

struct Printer {
    std::string out;

    void line(int indent, const std::string& text) {
        out.append(indent * 2, ' ');
        out += text;
        out += '\n';
    }

    void body(int indent, const std::vector<StmtPtr>& stmts) {
        for (const StmtPtr& s : stmts) stmt(indent, s.get());
    }

    void member(int indent, const Stmt* s) {
        std::string head = accessStr(s->access);
        if (s->isCtor) {
            head += "Ctor " + sv(s->name) + "(" + paramList(s->params) + ")";
        } else if (s->isGet || s->isSet) {
            head += (s->isGet ? "Get " : "Set ") + sv(s->name) +
                    "(" + paramList(s->params) + ")";
        } else if (s->selector.symbolic) {
            head += "Operator (" + sv(s->selector.text) + ")(" + paramList(s->params) +
                    ") : " + typeStr(s->type.get());
        } else if (s->callable) {
            head += "Method " + sv(s->name) + "(" + paramList(s->params) +
                    ") : " + typeStr(s->type.get());
        } else {
            head += "Field ";
            if (s->distinct) head += "distinct ";
            if (s->isWeak) head += "weak ";
            if (s->isReadonly) head += "readonly ";
            if (s->isConst) head += "const ";
            head += sv(s->name) + " : " + typeStr(s->type.get());
            if (s->init) head += " = " + exprStr(s->init.get());
        }
        line(indent, head);
        if (s->memberBody) stmt(indent + 1, s->memberBody.get());
    }

    // `@Name(args)` annotations, one line each, above their declaration.
    void attrs(int indent, const Stmt* s) {
        for (const AttrUse& a : s->attrs) {
            std::string t = "@";
            for (std::string_view seg : a.path) t += sv(seg) + "::";
            t += sv(a.name);
            if (!a.args.empty()) {
                t += "(";
                for (size_t i = 0; i < a.args.size(); ++i) {
                    if (i) t += ", ";
                    t += exprStr(a.args[i].get());
                }
                t += ")";
            }
            line(indent, t);
        }
    }

    void stmt(int indent, const Stmt* s) {
        if (!s) return;
        attrs(indent, s);
        switch (s->kind) {
            case StmtKind::Namespace:
                line(indent, "Namespace " + sv(s->name));
                body(indent + 1, s->body);
                break;
            case StmtKind::Class: {
                std::string head = accessStr(s->access);
                head += s->isAttribute  ? "Attribute "
                        : s->isInterface ? "Interface " : "Class ";
                head += sv(s->name);
                if (!s->generics.empty()) {
                    head += "<";
                    for (size_t i = 0; i < s->generics.size(); ++i) {
                        if (i) head += ", ";
                        head += sv(s->generics[i]);
                    }
                    head += ">";
                }
                if (!s->bases.empty()) head += " : " + typeList(s->bases, ", ");
                line(indent, head);
                body(indent + 1, s->body);
                break;
            }
            case StmtKind::Enum: {
                std::string head = accessStr(s->access);
                head += "Enum " + sv(s->name);
                if (s->type) head += " : " + typeStr(s->type.get());
                line(indent, head);
                for (const StmtPtr& m : s->body) {
                    std::string ml = "Member " + sv(m->name);
                    if (m->init) ml += " = " + exprStr(m->init.get());
                    line(indent + 1, ml);
                }
                break;
            }
            case StmtKind::Member:
                member(indent, s);
                break;
            case StmtKind::Bind: {
                if (s->type) {
                    line(indent, "Bind " + typeStr(s->type.get()) + " =>");
                    if (s->memberBody) stmt(indent + 1, s->memberBody.get());
                } else {
                    line(indent, "Bind (object) " + exprStr(s->init.get()));
                }
                break;
            }
            case StmtKind::Var: {
                std::string head = accessStr(s->access);
                if (s->isComptime) head += "Comptime ";
                head += "Var " + sv(s->name) + " : " +
                        (s->inferred ? "var" : typeStr(s->type.get()));
                if (s->init) head += " = " + exprStr(s->init.get());
                line(indent, head);
                break;
            }
            case StmtKind::Block:
                line(indent, "Block");
                body(indent + 1, s->body);
                break;
            case StmtKind::ExprStmt:
                line(indent, "Expr " + exprStr(s->expr.get()));
                break;
            case StmtKind::Return:
                line(indent, s->expr ? "Return " + exprStr(s->expr.get()) : "Return");
                break;
            case StmtKind::If:
                line(indent, std::string(s->isComptime ? "Comptime " : "") +
                             "If (" + exprStr(s->expr.get()) + ")");
                stmt(indent + 1, s->thenBranch.get());
                if (s->elseBranch) {
                    line(indent, "Else");
                    stmt(indent + 1, s->elseBranch.get());
                }
                break;
            case StmtKind::ForSplice:
                line(indent, "ForSplice " + sv(s->name) + " in " +
                     exprStr(s->expr.get()));
                stmt(indent + 1, s->thenBranch.get());
                break;
            case StmtKind::While:
                line(indent, "While (" + exprStr(s->expr.get()) + ")" + labelSuffix(s));
                stmt(indent + 1, s->thenBranch.get());
                break;
            case StmtKind::DoWhile:
                line(indent, "DoWhile (" + exprStr(s->expr.get()) + ")" + labelSuffix(s));
                stmt(indent + 1, s->thenBranch.get());
                break;
            case StmtKind::Break:
                line(indent, "Break" + labelSuffix(s));
                break;
            case StmtKind::Continue:
                line(indent, "Continue" + labelSuffix(s));
                break;
            case StmtKind::ForIn:
                line(indent, "ForIn " + sv(s->name) + " : " +
                     (s->inferred ? "var" : typeStr(s->type.get())) +
                     " in " + exprStr(s->expr.get()) + labelSuffix(s));
                stmt(indent + 1, s->thenBranch.get());
                break;
            case StmtKind::For:
                line(indent, "For (cond " +
                     (s->expr ? exprStr(s->expr.get()) : std::string("true")) + ")" + labelSuffix(s));
                if (s->forInit) { line(indent + 1, "init"); stmt(indent + 2, s->forInit.get()); }
                if (s->forStep) line(indent + 1, "step " + exprStr(s->forStep.get()));
                stmt(indent + 1, s->thenBranch.get());
                break;
            case StmtKind::Use: {
                std::string path;
                for (size_t i = 0; i < s->generics.size(); ++i) {
                    if (i) path += "::";
                    path += sv(s->generics[i]);
                }
                std::string alias = (!s->generics.empty() && s->name != s->generics.back())
                    ? " as " + sv(s->name) : "";
                line(indent, "Use " + path + alias);
                break;
            }
            case StmtKind::UsesImport: {
                std::string path;
                for (size_t i = 0; i < s->generics.size(); ++i) {
                    if (i) path += "::";
                    path += sv(s->generics[i]);
                }
                line(indent, "Uses " + path);
                break;
            }
            case StmtKind::Throw:
                line(indent, "Throw " + exprStr(s->expr.get()));
                break;
            case StmtKind::Try:
                line(indent, "Try");
                stmt(indent + 1, s->thenBranch.get());
                for (const CatchClause& c : s->catches) {
                    line(indent, "Catch (" + typeStr(c.type.get()) +
                         (c.name.empty() ? "" : " " + sv(c.name)) + ")");
                    stmt(indent + 1, c.body.get());
                }
                break;
            case StmtKind::Empty:
                line(indent, s->name.empty() ? "Empty" : "Marker " + sv(s->name));
                break;
            case StmtKind::Rule: {
                if (s->isMacroDecl) {
                    std::string params;
                    for (size_t i = 0; i < s->generics.size(); ++i) {
                        if (i) params += ", ";
                        params += sv(s->generics[i]);
                    }
                    std::string tmpl = s->ruleActions.empty() || !s->ruleActions[0].tmplExpr
                        ? "?" : exprStr(s->ruleActions[0].tmplExpr.get());
                    line(indent, "Macro " + sv(s->name) + "(" + params + ") => " + tmpl);
                    break;
                }
                std::string head = "Rule " + sv(s->name);
                if (s->ruleRewrites) head += " rewrites";
                line(indent, head);
                if (s->ruleMatch) {
                    const RuleMatch& m = *s->ruleMatch;
                    std::string mt = "match ";
                    if (m.one) mt += "one ";
                    if (m.hasAttr) {
                        mt += "@";
                        for (std::string_view seg : m.attrPath) mt += sv(seg) + "::";
                        mt += sv(m.attrName);
                        if (!m.attrBind.empty()) mt += "(" + sv(m.attrBind) + ")";
                        mt += " ";
                    }
                    mt += "on " + sv(m.subject.kindWord) + " " + sv(m.subject.bind);
                    for (const RuleLevel& lv : m.enclosers) {
                        mt += " in " + sv(lv.kindWord) + " " + sv(lv.bind);
                        if (lv.constraint) mt += " : " + typeStr(lv.constraint.get());
                    }
                    line(indent + 1, mt);
                }
                for (const RuleAction& a : s->ruleActions) {
                    const char* an = a.anchor == AnchorKind::CtorTop ? "top of ctor"
                        : a.anchor == AnchorKind::CtorBottom ? "bottom of ctor"
                        : a.anchor == AnchorKind::MemberOf ? "member of"
                        : a.anchor == AnchorKind::BodyTop ? "top of body"
                        : a.anchor == AnchorKind::BodyBottom ? "bottom of body"
                        : a.anchor == AnchorKind::Marker ? "marker"
                        : "namespace";
                    std::string target = a.anchor == AnchorKind::Marker
                        ? "\"" + sv(a.markerName) + "\"" : sv(a.target);
                    line(indent + 1, std::string("inject at ") + an + " " + target);
                    for (const StmtPtr& t : a.tmplStmts) stmt(indent + 2, t.get());
                    if (a.tmplMember) stmt(indent + 2, a.tmplMember.get());
                    if (a.tmplExpr) line(indent + 2, "Expr " + exprStr(a.tmplExpr.get()));
                }
                break;
            }
        }
    }
};

// ===========================================================================
//  Source-shaped printer (metaprog Phase 4 §6): re-emit the post-rules AST as
//  compilable Leviathan source. The strong acceptance is round-trip: the output
//  saved as a `.lev` compiles and runs identically to the original. So the goal
//  is semantic fidelity, not byte-for-byte formatting — parens are kept for
//  precedence, single-statement control-flow bodies are wrapped in braces, and
//  arrow bodies re-emit as `=> expr;`.
// ===========================================================================

std::string srcExpr(const Expr* e);
std::string srcStmtInline(const Stmt* s);

std::string srcExprList(const std::vector<ExprPtr>& es) {
    std::string out;
    for (size_t i = 0; i < es.size(); ++i) {
        if (i) out += ", ";
        if (!es[i]->argLabel.empty()) out += sv(es[i]->argLabel) + ": ";
        out += srcExpr(es[i].get());
    }
    return out;
}

// Lambda parameters re-emit NAMES ONLY: the parser accepts only untyped lambda
// params (types are inferred from the function-type context), whereas `$_params`
// forwarding can put typed params in the AST — emitting those would not reparse.
std::string lambdaParamList(const std::vector<Param>& ps) {
    std::string out;
    for (size_t i = 0; i < ps.size(); ++i) { if (i) out += ", "; out += sv(ps[i].name); }
    return out;
}

// A block body inside an expression (lambda / match arm), rendered on one line.
std::string srcBlockInline(const Stmt* block) {
    std::string out = "{ ";
    if (block && block->kind == StmtKind::Block)
        for (const StmtPtr& s : block->body) out += srcStmtInline(s.get()) + " ";
    else if (block)
        out += srcStmtInline(block) + " ";
    return out + "}";
}

// A string literal, faithfully re-quoted. A normal literal keeps its raw token
// text (quotes + escapes intact -> emit verbatim). An interpolation/char raw
// segment holds bare content, so it is re-wrapped in its original quote style
// (single quotes preserve a `char`-eligible literal's re-typing, Track 03 §1).
std::string srcString(const Expr* e) {
    if (e->isRawSegment) {
        char q = e->singleQuoted ? '\'' : '"';
        return std::string(1, q) + std::string(e->text) + std::string(1, q);
    }
    // LA-20 §8: an imported literal over ~200 bytes would drown the dump —
    // elide to a provenance comment instead of the raw content. Only when
    // this exact span was recorded as import()-sourced (RuleEngine); a small
    // template prints verbatim so expand_roundtrip stays honest on it.
    if (g_importLits) {
        auto it = g_importLits->find(e->span.offset);
        if (it != g_importLits->end() && it->second.second > 200) {
            return "\"…(" + std::to_string(it->second.second) + " bytes, imported: " +
                   it->second.first + ")…\"";
        }
    }
    return std::string(e->text);
}

std::string srcExpr(const Expr* e) {
    if (!e) return "";
    switch (e->kind) {
        case ExprKind::StringLit: return srcString(e);
        case ExprKind::IntLit:
        case ExprKind::FloatLit:
        case ExprKind::BoolLit:
        case ExprKind::Name:
            if (!e->explicitTypeArgs.empty())   // LA-32 §4.6 value reference
                return sv(e->text) + "::<" + typeList(e->explicitTypeArgs, ", ") + ">";
            return sv(e->text);
        case ExprKind::This:     return "this";
        case ExprKind::Member: {
            std::string out = srcExpr(e->a.get()) +
                              (e->colon ? "::" : e->optChain ? "?." : ".") + sv(e->text);
            if (!e->explicitTypeArgs.empty())   // LA-32 §4.6 value reference
                out += "::<" + typeList(e->explicitTypeArgs, ", ") + ">";
            return out;
        }
        case ExprKind::Call: {
            assert(!(e->isMacroCall && !e->explicitTypeArgs.empty()));
            std::string applied = e->explicitTypeArgs.empty()
                ? "" : "::<" + typeList(e->explicitTypeArgs, ", ") + ">";
            return srcExpr(e->a.get()) + applied +
                   (e->isMacroCall ? "!(" : "(") + srcExprList(e->list) + ")";
        }
        case ExprKind::Index:
            return srcExpr(e->a.get()) + "[" + srcExpr(e->b.get()) + "]";
        case ExprKind::Unary:
            return std::string(opSymbol(e->op)) + srcExpr(e->a.get());
        case ExprKind::Binary:
            return "(" + srcExpr(e->a.get()) + " " + opSymbol(e->op) + " " + srcExpr(e->b.get()) + ")";
        case ExprKind::Ternary:
            return "(" + srcExpr(e->a.get()) + " ? " + srcExpr(e->b.get()) + " : " + srcExpr(e->c.get()) + ")";
        case ExprKind::Array:
            return "[" + srcExprList(e->list) + "]";
        case ExprKind::Range:
            return "(" + srcExpr(e->a.get()) + " .. " + srcExpr(e->b.get()) + ")";
        case ExprKind::Lambda:
            return "(" + lambdaParamList(e->params) + ") => " +
                   (e->block ? srcBlockInline(e->block.get()) : srcExpr(e->a.get()));
        case ExprKind::Await:   return "await " + srcExpr(e->a.get());
        case ExprKind::Inject:  return "inject " + typeStr(e->type.get());
        case ExprKind::Extract: return "(" + srcExpr(e->a.get()) + " >>)";
        case ExprKind::Is:      return "(" + srcExpr(e->a.get()) + " is " + typeStr(e->type.get()) + ")";
        case ExprKind::ForSplice:
            return "$for " + sv(e->text) + " in " + srcExpr(e->a.get()) + " : " + srcExpr(e->b.get());
        case ExprKind::Match: {
            std::string out = "match (" + srcExpr(e->a.get()) + ") {";
            for (const MatchArm& arm : e->arms) {
                out += " ";
                if (arm.isElse) out += "else";
                else if (arm.type) out += typeStr(arm.type.get());
                else out += srcExpr(arm.value.get());
                out += " => " + (arm.bodyBlock ? srcBlockInline(arm.bodyBlock.get())
                                               : srcExpr(arm.bodyExpr.get())) + ";";
            }
            return out + " }";
        }
    }
    return "";
}

// A single statement rendered on one line (lambda/match block bodies, and
// braced-body wrapping). Covers the shapes those positions realistically take.
std::string srcStmtInline(const Stmt* s) {
    if (!s) return ";";
    switch (s->kind) {
        case StmtKind::ExprStmt: return srcExpr(s->expr.get()) + ";";
        case StmtKind::Return:   return s->expr ? "return " + srcExpr(s->expr.get()) + ";" : "return;";
        case StmtKind::Var: {
            std::string h;
            if (s->isConst) h += "const ";
            h += (s->inferred ? "var" : typeStr(s->type.get())) + " " + sv(s->name);
            if (s->init) h += " = " + srcExpr(s->init.get());
            return h + ";";
        }
        case StmtKind::Block:    return srcBlockInline(s);
        case StmtKind::If: {
            std::string h = "if (" + srcExpr(s->expr.get()) + ") " + srcStmtInline(s->thenBranch.get());
            if (s->elseBranch) h += " else " + srcStmtInline(s->elseBranch.get());
            return h;
        }
        case StmtKind::While:
            return labelPrefix(s) + "while (" + srcExpr(s->expr.get()) + ") " +
                   srcStmtInline(s->thenBranch.get());
        case StmtKind::DoWhile:
            return labelPrefix(s) + "do " + srcStmtInline(s->thenBranch.get()) +
                   " while (" + srcExpr(s->expr.get()) + ");";
        case StmtKind::For:
            return labelPrefix(s) + "for (" + (s->forInit ? srcStmtInline(s->forInit.get()) : ";") +
                   " " + (s->expr ? srcExpr(s->expr.get()) : std::string()) +
                   "; " + (s->forStep ? srcExpr(s->forStep.get()) : std::string()) + ") " +
                   srcStmtInline(s->thenBranch.get());
        case StmtKind::ForIn:
            return labelPrefix(s) + "for (" + (s->inferred ? std::string("var") : typeStr(s->type.get())) +
                   " " + sv(s->name) + " in " + srcExpr(s->expr.get()) + ") " +
                   srcStmtInline(s->thenBranch.get());
        case StmtKind::Try: {
            std::string h = "try " + srcStmtInline(s->thenBranch.get());
            for (const CatchClause& c : s->catches) {
                h += " catch (" + typeStr(c.type.get()) +
                     (c.name.empty() ? "" : " " + sv(c.name)) + ") " + srcStmtInline(c.body.get());
            }
            return h;
        }
        case StmtKind::Break:    return s->label.empty() ? "break;" : "break " + sv(s->label) + ";";
        case StmtKind::Continue: return s->label.empty() ? "continue;" : "continue " + sv(s->label) + ";";
        case StmtKind::Throw:    return "throw " + srcExpr(s->expr.get()) + ";";
        // The marker name is stored already-quoted (the raw StringLiteral token).
        case StmtKind::Empty:    return s->name.empty() ? ";" : "@anchor(" + sv(s->name) + ");";
        default:                 return ";";
    }
}

struct SourcePrinter {
    std::string out;
    const std::vector<ExpandProvenance>* prov = nullptr;

    void ind(int n) { out.append(n * 4, ' '); }
    void line(int n, const std::string& t) { ind(n); out += t; out += '\n'; }

    // A `// from rule …` comment above an injected declaration, located by
    // whether its span falls inside a firing's quasiquote template span.
    void provComment(int n, const Stmt* s) {
        if (!prov) return;
        for (const ExpandProvenance& p : *prov)
            if (s->span.offset >= p.tmplStart && s->span.offset < p.tmplEnd) {
                line(n, "// " + p.comment);
                return;
            }
    }

    void attrs(int n, const Stmt* s) {
        for (const AttrUse& a : s->attrs) {
            std::string t = "@";
            for (std::string_view seg : a.path) t += sv(seg) + "::";
            t += sv(a.name);
            if (!a.args.empty()) t += "(" + srcExprList(a.args) + ")";
            line(n, t);
        }
    }

    std::string generics(const Stmt* s) {
        if (s->generics.empty()) return "";
        std::string g = "<";
        for (size_t i = 0; i < s->generics.size(); ++i) { if (i) g += ", "; g += sv(s->generics[i]); }
        return g + ">";
    }

    // Emit a callable's body after `head`: block over lines, `=> expr;` for an
    // arrow (single Return), a bare inline statement otherwise, or `;` for none.
    void emitBody(int n, const std::string& head, const Stmt* body) {
        if (!body) { line(n, head + ";"); return; }
        if (body->kind == StmtKind::Block) {
            line(n, head + " {");
            for (const StmtPtr& s : body->body) stmt(n + 1, s.get());
            line(n, "}");
        } else if (body->kind == StmtKind::Return && body->expr) {
            line(n, head + " => " + srcExpr(body->expr.get()) + ";");
        } else {
            line(n, head + " " + srcStmtInline(body));
        }
    }

    // Emit a control-flow body as `{ ... }`, wrapping a single statement (a
    // semantically-identical brace wrap that always round-trips). No newline —
    // the caller adds the terminator (to allow a trailing `else`).
    void bracedBody(int n, const Stmt* body) {
        out += " {\n";
        if (body) {
            if (body->kind == StmtKind::Block)
                for (const StmtPtr& s : body->body) stmt(n + 1, s.get());
            else
                stmt(n + 1, body);
        }
        ind(n); out += "}";
    }

    void member(int n, const Stmt* s) {
        provComment(n, s);
        attrs(n, s);
        std::string head = accessStr(s->access);
        if (s->isMutating) head += "mutating ";
        if (s->isCtor) {
            head += "new " + sv(s->name) + "(" + paramList(s->params) + ")";
            emitBody(n, head, s->memberBody.get());
        } else if (s->isGet || s->isSet) {
            head += s->isGet ? "get " : "set ";
            head += s->selector.symbolic ? "(" + sv(s->selector.text) + ")" : sv(s->name);
            head += "(" + paramList(s->params) + ")";
            emitBody(n, head, s->memberBody.get());
        } else if (s->selector.symbolic) {   // operator: RetType (sym)(params)
            head += typeStr(s->type.get()) + " (" + sv(s->selector.text) + ")(" +
                    paramList(s->params) + ")";
            emitBody(n, head, s->memberBody.get());
        } else if (s->callable) {            // method / function
            head += typeStr(s->type.get()) + " " + sv(s->name) + generics(s) +
                    "(" + paramList(s->params) + ")";
            emitBody(n, head, s->memberBody.get());
        } else {                             // field
            if (s->distinct) head += "distinct ";
            if (s->isWeak) head += "weak ";
            if (s->isReadonly) head += "readonly ";
            if (s->isConst) head += "const ";
            head += typeStr(s->type.get()) + " " + sv(s->name);
            if (s->init) head += " = " + srcExpr(s->init.get());
            line(n, head + ";");
        }
    }

    void stmt(int n, const Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::Namespace:
                provComment(n, s);
                line(n, "namespace " + sv(s->name) + " {");
                for (const StmtPtr& b : s->body) stmt(n + 1, b.get());
                line(n, "}");
                break;
            case StmtKind::Class: {
                provComment(n, s);
                attrs(n, s);
                std::string h = accessStr(s->access);
                h += s->isAttribute ? "attribute " : s->isInterface ? "interface "
                     : s->isValue ? "struct " : "class ";
                h += sv(s->name) + generics(s);
                if (!s->bases.empty()) h += " : " + typeList(s->bases, ", ");
                line(n, h + " {");
                for (const StmtPtr& b : s->body) stmt(n + 1, b.get());
                line(n, "}");
                break;
            }
            case StmtKind::Enum: {
                std::string h = std::string(accessStr(s->access)) + "enum " + sv(s->name);
                if (s->type) h += " : " + typeStr(s->type.get());
                line(n, h + " {");
                for (size_t i = 0; i < s->body.size(); ++i) {
                    const Stmt* m = s->body[i].get();
                    std::string ml = sv(m->name);
                    if (m->init) ml += " = " + srcExpr(m->init.get());
                    if (i + 1 < s->body.size()) ml += ",";
                    line(n + 1, ml);
                }
                line(n, "}");
                break;
            }
            case StmtKind::Member: member(n, s); break;
            case StmtKind::Var: {
                provComment(n, s);
                std::string h = accessStr(s->access);
                if (s->isComptime) h += "comptime ";
                if (s->isUsing) h += "using ";
                else if (s->isConst) h += "const ";
                h += (s->inferred ? "var" : typeStr(s->type.get())) + " " + sv(s->name);
                if (s->init) h += " = " + srcExpr(s->init.get());
                line(n, h + ";");
                break;
            }
            case StmtKind::Bind: {
                provComment(n, s);
                if (s->type) {
                    if (s->memberBody && s->memberBody->kind == StmtKind::Block) {
                        ind(n); out += "bind " + typeStr(s->type.get());
                        bracedBody(n, s->memberBody.get());
                        out += "\n";
                    } else {
                        emitBody(n, "bind " + typeStr(s->type.get()), s->memberBody.get());
                    }
                } else {
                    line(n, "bind " + srcExpr(s->init.get()) + ";");
                }
                break;
            }
            case StmtKind::Block:
                line(n, "{");
                for (const StmtPtr& b : s->body) stmt(n + 1, b.get());
                line(n, "}");
                break;
            case StmtKind::ExprStmt:
                provComment(n, s);
                line(n, srcExpr(s->expr.get()) + ";");
                break;
            case StmtKind::Return:
                line(n, s->expr ? "return " + srcExpr(s->expr.get()) + ";" : "return;");
                break;
            case StmtKind::If:
                ind(n);
                out += (s->isComptime ? "comptime if (" : "if (") + srcExpr(s->expr.get()) + ")";
                bracedBody(n, s->thenBranch.get());
                if (s->elseBranch) { out += " else"; bracedBody(n, s->elseBranch.get()); }
                out += "\n";
                break;
            case StmtKind::ForSplice:
                // Template-only; the expanded program never contains one, so this
                // renders only in a raw template dump.
                ind(n); out += "$for " + sv(s->name) + " in " +
                       srcExpr(s->expr.get()) + " :\n";
                stmt(n + 1, s->thenBranch.get());
                break;
            case StmtKind::While:
                ind(n); out += labelPrefix(s) + "while (" + srcExpr(s->expr.get()) + ")";
                bracedBody(n, s->thenBranch.get()); out += "\n";
                break;
            case StmtKind::DoWhile:
                ind(n); out += labelPrefix(s) + "do";
                bracedBody(n, s->thenBranch.get());
                out += " while (" + srcExpr(s->expr.get()) + ");\n";
                break;
            case StmtKind::For:
                ind(n); out += labelPrefix(s) + "for (";
                out += (s->forInit ? srcStmtInline(s->forInit.get()) : ";");
                out += " " + (s->expr ? srcExpr(s->expr.get()) : std::string());
                out += "; " + (s->forStep ? srcExpr(s->forStep.get()) : std::string()) + ")";
                bracedBody(n, s->thenBranch.get()); out += "\n";
                break;
            case StmtKind::ForIn:
                ind(n); out += labelPrefix(s) + "for (" + (s->inferred ? std::string("var") : typeStr(s->type.get())) +
                       " " + sv(s->name) + " in " + srcExpr(s->expr.get()) + ")";
                bracedBody(n, s->thenBranch.get()); out += "\n";
                break;
            case StmtKind::Break:    line(n, s->label.empty() ? "break;" : "break " + sv(s->label) + ";"); break;
            case StmtKind::Continue: line(n, s->label.empty() ? "continue;" : "continue " + sv(s->label) + ";"); break;
            case StmtKind::Throw:    line(n, "throw " + srcExpr(s->expr.get()) + ";"); break;
            case StmtKind::Try:
                ind(n); out += "try";
                bracedBody(n, s->thenBranch.get());
                for (const CatchClause& c : s->catches) {
                    out += " catch (" + typeStr(c.type.get()) +
                           (c.name.empty() ? "" : " " + sv(c.name)) + ")";
                    bracedBody(n, c.body.get());
                }
                out += "\n";
                break;
            case StmtKind::Use: {
                std::string path;
                for (size_t i = 0; i < s->generics.size(); ++i) {
                    if (i) path += "::";
                    path += sv(s->generics[i]);
                }
                std::string alias = (!s->generics.empty() && s->name != s->generics.back())
                    ? " as " + sv(s->name) : "";
                line(n, "use " + path + alias + ";");
                break;
            }
            case StmtKind::UsesImport: {
                std::string path;
                for (size_t i = 0; i < s->generics.size(); ++i) {
                    if (i) path += "::";
                    path += sv(s->generics[i]);
                }
                line(n, "uses " + path + ";");
                break;
            }
            case StmtKind::Empty:
                // The marker name is stored already-quoted (raw StringLiteral).
                if (!s->name.empty()) line(n, "@anchor(" + sv(s->name) + ");");
                break;
            case StmtKind::Rule:
                // Rules are detached from the tree before this printer ever
                // runs (§5.1); nothing to emit if one somehow remains.
                break;
        }
    }
};

}  // namespace

std::string printProgram(const Program& program) {
    Printer p;
    p.line(0, "Program");
    p.body(1, program.items);
    return p.out;
}

std::string printProgramSource(const Program& program,
                               const std::vector<ExpandProvenance>& prov,
                               const std::map<uint32_t, std::pair<std::string, size_t>>& importLits) {
    SourcePrinter p;
    p.prov = &prov;
    g_importLits = &importLits;
    for (const StmtPtr& s : program.items) p.stmt(0, s.get());
    g_importLits = nullptr;
    return p.out;
}
