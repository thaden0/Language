#include "Token.hpp"
#include <cstdlib>
#include <unordered_map>

const char* tokenKindName(TokenKind k) {
    switch (k) {
        case TokenKind::End:           return "End";
        case TokenKind::Error:         return "Error";
        case TokenKind::Identifier:    return "Identifier";
        case TokenKind::IntLiteral:    return "IntLiteral";
        case TokenKind::FloatLiteral:  return "FloatLiteral";
        case TokenKind::StringLiteral: return "StringLiteral";
        case TokenKind::RawStringLiteral: return "RawStringLiteral";
        case TokenKind::QuasiLiteral:  return "QuasiLiteral";
        case TokenKind::KwNamespace:   return "KwNamespace";
        case TokenKind::KwClass:       return "KwClass";
        case TokenKind::KwStruct:      return "KwStruct";
        case TokenKind::KwInterface:   return "KwInterface";
        case TokenKind::KwMutating:    return "KwMutating";
        case TokenKind::KwPublic:      return "KwPublic";
        case TokenKind::KwPrivate:     return "KwPrivate";
        case TokenKind::KwNew:         return "KwNew";
        case TokenKind::KwDistinct:    return "KwDistinct";
        case TokenKind::KwConst:       return "KwConst";
        case TokenKind::KwReadonly:    return "KwReadonly";
        case TokenKind::KwWeak:        return "KwWeak";
        case TokenKind::KwGet:         return "KwGet";
        case TokenKind::KwSet:         return "KwSet";
        case TokenKind::KwReturn:      return "KwReturn";
        case TokenKind::KwVar:         return "KwVar";
        case TokenKind::KwLet:         return "KwLet";
        case TokenKind::KwAwait:       return "KwAwait";
        case TokenKind::KwBind:        return "KwBind";
        case TokenKind::KwInject:      return "KwInject";
        case TokenKind::KwUse:         return "KwUse";
        case TokenKind::KwUses:        return "KwUses";
        case TokenKind::KwIf:          return "KwIf";
        case TokenKind::KwElse:        return "KwElse";
        case TokenKind::KwTry:         return "KwTry";
        case TokenKind::KwCatch:       return "KwCatch";
        case TokenKind::KwThrow:       return "KwThrow";
        case TokenKind::KwWhile:       return "KwWhile";
        case TokenKind::KwFor:         return "KwFor";
        case TokenKind::KwIn:          return "KwIn";
        case TokenKind::KwIs:          return "KwIs";
        case TokenKind::KwMatch:       return "KwMatch";
        case TokenKind::KwThis:        return "KwThis";
        case TokenKind::KwTrue:        return "KwTrue";
        case TokenKind::KwFalse:       return "KwFalse";
        case TokenKind::KwBreak:       return "KwBreak";
        case TokenKind::KwContinue:    return "KwContinue";
        case TokenKind::KwDo:          return "KwDo";
        case TokenKind::KwUsing:       return "KwUsing";
        case TokenKind::KwEnum:        return "KwEnum";
        case TokenKind::ColonColon:    return "ColonColon";
        case TokenKind::Colon:         return "Colon";
        case TokenKind::Semicolon:     return "Semicolon";
        case TokenKind::Comma:         return "Comma";
        case TokenKind::Dot:           return "Dot";
        case TokenKind::DotDot:        return "DotDot";
        case TokenKind::LParen:        return "LParen";
        case TokenKind::RParen:        return "RParen";
        case TokenKind::LBrace:        return "LBrace";
        case TokenKind::RBrace:        return "RBrace";
        case TokenKind::LBracket:      return "LBracket";
        case TokenKind::RBracket:      return "RBracket";
        case TokenKind::Arrow:         return "Arrow";
        case TokenKind::Eq:            return "Eq";
        case TokenKind::EqEq:          return "EqEq";
        case TokenKind::BangEq:        return "BangEq";
        case TokenKind::Bang:          return "Bang";
        case TokenKind::Lt:            return "Lt";
        case TokenKind::Gt:            return "Gt";
        case TokenKind::Le:            return "Le";
        case TokenKind::Ge:            return "Ge";
        case TokenKind::Plus:          return "Plus";
        case TokenKind::Minus:         return "Minus";
        case TokenKind::Star:          return "Star";
        case TokenKind::Slash:         return "Slash";
        case TokenKind::Percent:       return "Percent";
        case TokenKind::PlusEq:        return "PlusEq";
        case TokenKind::MinusEq:       return "MinusEq";
        case TokenKind::StarEq:        return "StarEq";
        case TokenKind::SlashEq:       return "SlashEq";
        case TokenKind::PercentEq:     return "PercentEq";
        case TokenKind::LtLt:          return "LtLt";
        case TokenKind::GtGt:          return "GtGt";
        case TokenKind::AmpAmp:        return "AmpAmp";
        case TokenKind::PipePipe:      return "PipePipe";
        case TokenKind::Amp:           return "Amp";
        case TokenKind::Pipe:          return "Pipe";
        case TokenKind::Question:      return "Question";
        case TokenKind::QuestionQuestion: return "QuestionQuestion";
        case TokenKind::QuestionDot:   return "QuestionDot";
        case TokenKind::At:            return "At";
        case TokenKind::Caret:         return "Caret";
        case TokenKind::Tilde:         return "Tilde";
    }
    return "<?>";
}

TokenKind keywordKind(std::string_view word) {
    static const std::unordered_map<std::string_view, TokenKind> table = {
        {"namespace", TokenKind::KwNamespace},
        {"class",     TokenKind::KwClass},
        {"struct",    TokenKind::KwStruct},
        {"interface", TokenKind::KwInterface},
        {"mutating",  TokenKind::KwMutating},
        {"public",    TokenKind::KwPublic},
        {"private",   TokenKind::KwPrivate},
        {"new",       TokenKind::KwNew},
        {"distinct",  TokenKind::KwDistinct},
        {"const",     TokenKind::KwConst},
        {"readonly",  TokenKind::KwReadonly},
        {"weak",      TokenKind::KwWeak},
        {"get",       TokenKind::KwGet},
        {"set",       TokenKind::KwSet},
        {"return",    TokenKind::KwReturn},
        {"var",       TokenKind::KwVar},
        {"let",       TokenKind::KwLet},
        {"await",     TokenKind::KwAwait},
        {"bind",      TokenKind::KwBind},
        {"inject",    TokenKind::KwInject},
        {"use",       TokenKind::KwUse},
        {"uses",      TokenKind::KwUses},
        {"if",        TokenKind::KwIf},
        {"else",      TokenKind::KwElse},
        {"try",       TokenKind::KwTry},
        {"catch",     TokenKind::KwCatch},
        {"throw",     TokenKind::KwThrow},
        {"while",     TokenKind::KwWhile},
        {"for",       TokenKind::KwFor},
        {"in",        TokenKind::KwIn},
        {"is",        TokenKind::KwIs},
        {"match",     TokenKind::KwMatch},
        {"this",      TokenKind::KwThis},
        {"true",      TokenKind::KwTrue},
        {"false",     TokenKind::KwFalse},
        {"break",     TokenKind::KwBreak},
        {"continue",  TokenKind::KwContinue},
        {"do",        TokenKind::KwDo},
        {"using",     TokenKind::KwUsing},
        {"enum",      TokenKind::KwEnum},
    };
    auto it = table.find(word);
    return it == table.end() ? TokenKind::Identifier : it->second;
}

long long parseIntLiteral(std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    for (char c : text) if (c != '_') clean.push_back(c);
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X'))
        return (long long)std::strtoull(clean.c_str() + 2, nullptr, 16);
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'b' || clean[1] == 'B'))
        return (long long)std::strtoull(clean.c_str() + 2, nullptr, 2);
    return std::atoll(clean.c_str());
}

double parseFloatLiteral(std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    for (char c : text) if (c != '_') clean.push_back(c);
    return std::atof(clean.c_str());
}

namespace {
bool isHexDigitChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int hexDigitValue(char c) {
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;   // (c|0x20): fold to lowercase
}
// Encode one Unicode scalar to UTF-8 (mirrors RuntimeValue.hpp's utf8Encode —
// duplicated rather than shared because that header sits above Token in the
// dependency graph; this file stays a leaf).
void appendUtf8(std::string& out, long long cp) {
    unsigned long c = (unsigned long)cp;
    if (c <= 0x7F) {
        out.push_back((char)c);
    } else if (c <= 0x7FF) {
        out.push_back((char)(0xC0 | (c >> 6)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    } else if (c <= 0xFFFF) {
        out.push_back((char)(0xE0 | (c >> 12)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (c >> 18)));
        out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    }
}
}  // namespace

std::string decodeEscapes(std::string_view content) {
    std::string out;
    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];
        if (c == '\\' && i + 1 < content.size()) {
            char n = content[++i];
            if (n == 'x' && i + 2 < content.size() &&
                isHexDigitChar(content[i + 1]) && isHexDigitChar(content[i + 2])) {
                out.push_back((char)((hexDigitValue(content[i + 1]) << 4) | hexDigitValue(content[i + 2])));
                i += 2;
                continue;
            }
            if (n == 'u' && i + 1 < content.size() && content[i + 1] == '{') {
                size_t j = i + 2;
                long long cp = 0;
                int digits = 0;
                while (j < content.size() && isHexDigitChar(content[j]) && digits < 6) {
                    cp = (cp << 4) | hexDigitValue(content[j]);
                    ++j;
                    ++digits;
                }
                if (digits > 0 && j < content.size() && content[j] == '}') {
                    bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
                    appendUtf8(out, (cp > 0x10FFFF || surrogate) ? 0xFFFD : cp);
                    i = j;
                    continue;
                }
                // malformed (no hex digits, missing '}', or >6 digits) — left
                // alone, same compat rule as a malformed `\x` below.
            }
            switch (n) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '0': out.push_back('\0'); break;
                default:  out.push_back(n); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string decodeStringLiteral(std::string_view raw) {
    if (raw.size() < 2) return "";
    return decodeEscapes(raw.substr(1, raw.size() - 2));
}
