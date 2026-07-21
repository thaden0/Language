#pragma once
#include "Rules.hpp"

// refactor_1 session 04: Rules.cpp split into Rules.cpp / RulesExpand.cpp /
// RulesClone.cpp. Every RuleEngine member function stays declared exactly
// once, in Rules.hpp, and is defined in whichever of the three .cpp files
// owns that concern (cross-TU member calls need nothing further).
//
// The two helpers below were file-static free functions in the original
// Rules.cpp with call sites that ended up split across more than one of the
// new TUs; a `static` can't be shared across translation units, so they are
// promoted to ordinary functions in this `rulesDetail` namespace instead.
namespace rulesDetail {

// Look up a class symbol by name in a namespace scope. Shared by
// RuleEngine::metaClassSymbol (Rules.cpp) and RuleEngine::indexDecls /
// RuleEngine::resolveTypeName / RuleEngine::tryMatch (RulesExpand.cpp).
// Defined in RulesExpand.cpp.
Symbol* lookupClassIn(Scope* sc, std::string_view name);

// A short, human-readable word for a declaration's kind, for diagnostics
// (M25/M26/M27). Shared by RuleEngine::expand / RuleEngine::warnDanglingAttrs
// (RulesExpand.cpp) and RuleEngine::cloneParamList / RuleEngine::cloneArgList
// (RulesClone.cpp). Defined in RulesExpand.cpp.
const char* declKindWord(const Stmt* s);

}  // namespace rulesDetail
