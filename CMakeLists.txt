cmake_minimum_required(VERSION 3.20)
project(leviathan CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# --- levsyntax: lexer/token/diagnostic shared by compiler + trident (techdesign-toolchain.md §5.2) ---
# H-3: a slim shared library so trident can tokenize the project{} manifest
# literal with the language's own real lexer without linking the rest of the
# compiler (Parser/Resolver/Checker/Eval/... stay langfront-only).
add_library(levsyntax
  src/core/Diagnostic.cpp
  src/core/Token.cpp
  src/frontend/Lexer.cpp)
# src/core + src/frontend: levsyntax's own sources use directory-qualified
# includes, but its consumers (tools/trident/*) include Diagnostic/Source/
# Token/Lexer unqualified; PUBLIC propagation keeps them building unedited.
target_include_directories(levsyntax PUBLIC src src/core src/frontend)
# --- end levsyntax ---

# --- shipped prelude (techdesign-prelude-ship-as-files-opus.md §3/§5) ---
# Canonical segment list, in concatenation order. This list is the single
# source of truth (design R5): the generator bakes it into the embedded-
# fallback table that Resolver::parsePrelude() and main.cpp's directory
# validator both iterate. wasm = Track W W-M3 DOM bridge, wasm32*-only (R3).
set(LV_PRELUDE_SEGMENTS core std rest regex_core regex_api web wasm expr)
set(LV_PRELUDE_WASM_ONLY wasm)
set(LV_PRELUDE_DIR ${CMAKE_SOURCE_DIR}/prelude)
set(LV_PRELUDE_EMBEDDED ${CMAKE_BINARY_DIR}/generated/PreludeEmbedded.cpp)
list(TRANSFORM LV_PRELUDE_SEGMENTS APPEND ".lev"
     OUTPUT_VARIABLE LV_PRELUDE_FILES)
list(TRANSFORM LV_PRELUDE_FILES PREPEND "${LV_PRELUDE_DIR}/"
     OUTPUT_VARIABLE LV_PRELUDE_FILES)
# Pass the lists comma-joined: a raw ;-list would be re-split by the shell
# when the custom command's rule is generated. The script rejoins on ",".
string(REPLACE ";" "," LV_PRELUDE_SEGMENTS_CSV "${LV_PRELUDE_SEGMENTS}")
string(REPLACE ";" "," LV_PRELUDE_WASM_ONLY_CSV "${LV_PRELUDE_WASM_ONLY}")
add_custom_command(
  OUTPUT ${LV_PRELUDE_EMBEDDED}
  COMMAND ${CMAKE_COMMAND}
          -DOUT=${LV_PRELUDE_EMBEDDED}
          -DPRELUDE_DIR=${LV_PRELUDE_DIR}
          -DSEGMENTS=${LV_PRELUDE_SEGMENTS_CSV}
          -DWASM_ONLY=${LV_PRELUDE_WASM_ONLY_CSV}
          -P ${CMAKE_SOURCE_DIR}/cmake/GenPreludeEmbedded.cmake
  DEPENDS ${LV_PRELUDE_FILES} ${CMAKE_SOURCE_DIR}/cmake/GenPreludeEmbedded.cmake
  COMMENT "Generating embedded prelude fallback")
# --- end shipped prelude ---

# --- front-end library (shared by driver + tests) ---
add_library(langfront
  ${LV_PRELUDE_EMBEDDED}
  src/frontend/Parser.cpp
  src/frontend/ParserExpr.cpp
  src/frontend/ParserMeta.cpp
  src/meta/AstPrinter.cpp
  src/driver/Project.cpp
  # --- Track A: compiler build-plan input (techdesign-toolchain.md §5.1) ---
  src/driver/BuildPlan.cpp
  # --- end Track A ---
  src/sema/Resolver.cpp
  src/sema/Checker.cpp
  src/sema/CheckerInfer.cpp
  src/sema/CheckerFlow.cpp
  src/sema/CheckerDispatch.cpp
  src/sema/CheckerGenerics.cpp
  src/sema/CheckerReify.cpp
  # refactor_1 session 03 (owner-approved addition, not pre-staged by session
  # 01): the overload-resolution / generic-inference-support / bind-inject
  # cluster shared across CheckerInfer/CheckerDispatch/CheckerGenerics call
  # sites — table gap discovered during implementation, see
  # designs/complete/refactor_1/techdesign-03-checker-split-sonnet.md.
  src/sema/CheckerOverload.cpp
  src/runtime/Eval.cpp
  src/meta/Rules.cpp
  src/meta/RulesExpand.cpp
  src/meta/RulesClone.cpp
  src/runtime/RuntimeCore.cpp
  src/runtime/RuntimeLoop.cpp
  src/runtime/RuntimeNatives.cpp
  src/ir/Lower.cpp
  src/backend/IrInterp.cpp
  src/ir/Ownership.cpp
  src/ir/MemVerify.cpp
  src/backend/CGen.cpp
  src/X64.cpp
  src/X64Gen.cpp)
# Everything under src/ uses directory-qualified includes resolved via `src`;
# the extra layer dirs are on the path solely for files that may not / need
# not be edited and still include unqualified:
#   - the FROZEN X64 files at src/ root (Diagnostic.hpp -> core, Ir.hpp /
#     Ownership.hpp -> ir) — X64 sources may never be edited;
#   - the generated build/generated/PreludeEmbedded.cpp (PreludeEmbedded.hpp
#     -> driver);
#   - tests/* (Lexer/Parser -> frontend, Checker/Resolver -> sema,
#     AstPrinter/Rules -> meta, Eval/RuntimeLoop -> runtime, Project ->
#     driver), which get these PUBLICly.
# The `layering` ctest enforces the dependency map on the qualified includes
# within the src/ layer directories themselves.
target_include_directories(langfront PUBLIC src src/core src/frontend
  src/sema src/meta src/ir src/runtime src/driver)
target_link_libraries(langfront PUBLIC levsyntax)

# --- optional LLVM backend ---
# Ubuntu installs LLVM outside CMake's default search path; ask llvm-config.
execute_process(COMMAND llvm-config-18 --cmakedir
                OUTPUT_VARIABLE LLVM_CMAKE_DIR_HINT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
find_package(LLVM QUIET CONFIG HINTS "${LLVM_CMAKE_DIR_HINT}")
if (LLVM_FOUND)
  message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} found — native backend enabled")
  target_sources(langfront PRIVATE src/backend/LlvmGen.cpp src/backend/LlvmGenOps.cpp src/backend/LlvmGenGlue.cpp)
  target_include_directories(langfront SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(langfront PUBLIC HAVE_LLVM)
  target_link_libraries(langfront PUBLIC LLVM)
endif()

# --- TLS (LA-2) --- optional OpenSSL backing (techdesign-tls-crypto.md §5.1)
# Same feature-detected, graceful-degradation pattern as HAVE_LLVM: when OpenSSL
# is absent the TLS/crypto natives compile as clean not-built stubs (the
# interpreters return the "TLS support not built" diagnostic), so the compiler
# always builds. When present, the interpreters get real OpenSSL 1.1.1+/3.x.
find_package(OpenSSL QUIET)
if (OpenSSL_FOUND)
  message(STATUS "OpenSSL ${OPENSSL_VERSION} found — TLS/crypto enabled")
  target_compile_definitions(langfront PUBLIC HAVE_OPENSSL)
  target_link_libraries(langfront PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
# --- end TLS (LA-2) ---

# --- compiler driver ---
add_executable(leviathan src/driver/main.cpp)
target_link_libraries(leviathan PRIVATE langfront)

# --- Track B: trident package manager (techdesign-toolchain.md §5.2) ---
# B-M1/B-M2: manifest ownership, dependency resolution, plan generation, and
# the leviathan-invoking driver. Links only levsyntax (Lexer/Token/Diagnostic)
# — never langfront — so trident carries none of the compiler's pipeline.
add_executable(trident
  tools/trident/main.cpp
  tools/trident/manifest.cpp
  tools/trident/resolve.cpp
  tools/trident/plan.cpp
  tools/trident/discover.cpp
  tools/trident/semver.cpp
  tools/trident/mvs.cpp
  tools/trident/hash.cpp
  tools/trident/store.cpp
  tools/trident/source_set.cpp
  tools/trident/process.cpp
  tools/trident/endpoint.cpp
  tools/trident/vcs.cpp
  tools/trident/fetch.cpp
  tools/trident/proxy.cpp
  tools/trident/index.cpp
  tools/trident/lock.cpp
  tools/trident/checksum.cpp
  tools/trident/package.cpp
  tools/trident/provenance.cpp
  tools/trident/commands.cpp)
target_link_libraries(trident PRIVATE levsyntax)

# --- tests ---
enable_testing()

# refactor_1 session 06: enforce the src/ layer dependency map (X64 exempt by
# virtue of living at src/ root).
add_test(NAME layering COMMAND bash ${CMAKE_SOURCE_DIR}/tools/check_layering.sh)

# refactor_1 session 08: fail on NEW cross-file duplication above the checked-in
# baseline (identifier-blind winnowing; X64 never scanned).
add_test(NAME clone-ratchet
  COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/clonedet.py check
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(unittests tests/test_lexer.cpp)
target_link_libraries(unittests PRIVATE langfront)
add_test(NAME unittests COMMAND unittests)

add_executable(parsertests tests/test_parser.cpp)
target_link_libraries(parsertests PRIVATE langfront)
add_test(NAME parsertests COMMAND parsertests)

add_executable(resolvertests tests/test_resolver.cpp)
target_link_libraries(resolvertests PRIVATE langfront)
add_test(NAME resolvertests COMMAND resolvertests)

add_executable(checkertests tests/test_checker.cpp)
target_link_libraries(checkertests PRIVATE langfront)
add_test(NAME checkertests COMMAND checkertests)

add_executable(evaltests tests/test_eval.cpp)
target_link_libraries(evaltests PRIVATE langfront)
add_test(NAME evaltests COMMAND evaltests)

add_executable(metatests tests/test_meta.cpp)
target_link_libraries(metatests PRIVATE langfront)
add_test(NAME metatests COMMAND metatests)

# P2.1a (techdesign-package-manager.md §5.1): MVS unit tests against a fake,
# in-memory ModuleProvider — no I/O, no levsyntax needed. Recompiles just the
# three trident sources under test (no shared "trident library" target
# exists — trident is an executable, and main.cpp's own main() would clash).
add_executable(mvstests tests/test_trident_mvs.cpp
  tools/trident/manifest.cpp
  tools/trident/semver.cpp
  tools/trident/mvs.cpp)
add_test(NAME mvstests COMMAND mvstests)

# P2.1b (techdesign-package-manager.md §5.2): SHA-256 against NIST vectors +
# content-addressed store idempotency/canonicalization.
add_executable(storetests tests/test_trident_store.cpp
  tools/trident/hash.cpp
  tools/trident/store.cpp)
add_test(NAME storetests COMMAND storetests)

# P2.1c (techdesign-package-manager.md §5.3): the GitProvider, run entirely
# against the offline bare-repo fixture tests/trident/store/fixture_json.git
# (H-7 — no test may reach the real network).
add_executable(fetchtests tests/test_trident_fetch.cpp
  tools/trident/manifest.cpp
  tools/trident/semver.cpp
  tools/trident/mvs.cpp
  tools/trident/hash.cpp
  tools/trident/store.cpp
  tools/trident/source_set.cpp
  tools/trident/process.cpp
  tools/trident/vcs.cpp
  tools/trident/fetch.cpp)
target_compile_definitions(fetchtests PRIVATE LANG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME fetchtests COMMAND fetchtests)

# P2.1d (techdesign-package-manager.md §5.4): the lockfile — round-trip,
# staleness detection, deterministic regeneration, §0.5 identity text.
add_executable(locktests tests/test_trident_lock.cpp
  tools/trident/semver.cpp
  tools/trident/lock.cpp)
add_test(NAME locktests COMMAND locktests)

# P2.2 (techdesign-package-manager.md §6, GT4): the checksum DB — record/
# verify round-trip, tamper detection (edited entry, edited chain link,
# truncated log), first-fetch baseline recording.
add_executable(checksumtests tests/test_trident_checksum.cpp
  tools/trident/hash.cpp
  tools/trident/semver.cpp
  tools/trident/checksum.cpp)
add_test(NAME checksumtests COMMAND checksumtests)

# P2.3 optional thin index: exact lookup, retry-safe registration, and
# first-registration-wins immutability (filesystem backend, no network).
add_executable(indextests tests/test_trident_index.cpp
  tools/trident/process.cpp
  tools/trident/endpoint.cpp
  tools/trident/index.cpp)
add_test(NAME indextests COMMAND indextests)

# Conformance: lexer, parser, and resolver must accept the whole demo, no errors.
# (Full type-check is not asserted here: the demo is a loose "tour" with known
#  type mismatches — see the operator/Promise return-type design questions.)
# P0-3 (techdesign-toolchain.md): the demo is the CMake-wired canonical system
# file, so it moved to the new default extension (project.ext -> demo.lev) and
# grew a canonical trident.toml manifest wrapping it. Existing demo/example/
# corpus .ext fixtures are untouched (owner directive) — only this file did.
add_test(NAME lex_demo_lev
         COMMAND leviathan --tokens "${CMAKE_SOURCE_DIR}/demo.lev")
add_test(NAME parse_demo_lev
         COMMAND leviathan --ast "${CMAKE_SOURCE_DIR}/demo.lev")
add_test(NAME resolve_demo_lev
         COMMAND leviathan --resolve "${CMAKE_SOURCE_DIR}/demo.lev")
# Full type-check now passes on the demo (operators construct their own type;
# slowSquare returns a real Promise<int> with T inferred).
add_test(NAME check_demo_lev
         COMMAND leviathan "${CMAKE_SOURCE_DIR}/demo.lev")
# GT0/GT1 smoke: the same demo, loaded through trident.toml and driven by
# trident (the front door, §3.1) instead of a bare file path.
add_test(NAME check_demo_trident
         COMMAND bash -c "\"$<TARGET_FILE:trident>\" check \"${CMAKE_SOURCE_DIR}\" --leviathan \"$<TARGET_FILE:leviathan>\" --plan \"${CMAKE_BINARY_DIR}/demo.lvplan\"")

# Corpus: real programs with expected outputs. The full corpus runs on the
# tree-walking oracle; the core subset ALSO runs on the bytecode IR, and both
# engines must produce identical output (the §17 two-variants contract).
add_test(NAME corpus_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus")
add_test(NAME corpus_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus")
add_test(NAME corpus_meta_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/meta")
add_test(NAME corpus_meta_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/meta")
# F4 procedural macros expand before lowering. Pin the same branching/list/
# import-fed runtime proof on emit-C++; the broader meta directory contains a
# compile-time logging fixture whose stdout intentionally is not pure C++.
add_test(NAME corpus_procedural_macros_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/meta/procedural_macros.lev")
# Item Q (techdesign-target-predicate.md): platform-conditional `uses` keyed by
# target::os folds in the front end, so the compiled backends must see the
# already-selected branch — same single-file lane pattern as procedural_macros.
add_test(NAME corpus_target_uses_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/meta/target_uses.lev")
# Metaprog Phase 4 §6: --expand emits recompilable source. This round-trips
# every meta program (expand -> compile -> run) and asserts byte-identical run
# output — the strong, mechanical proof the source printer is faithful.
add_test(NAME corpus_meta_expand_roundtrip
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expand_roundtrip.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/meta")
# B2 conditional splice (designs/complete/metaprog-splices/
# techdesign-splices-conditional-opus.md §5/§8): the three `$if`/`$else` reject
# rows — each a hard compile error carrying its named diagnostic. The green
# fixtures (rule_if_splice{,_twin}, rule_if_expr{,_twin}, rule_if_chain) ride the
# corpus_meta_* lanes above.
add_test(NAME rule_if_reject_badcond            # M40: non-bool condition
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/rule_if_badcond.ext"
                 "$if-condition must be bool")
add_test(NAME rule_if_reject_dangling_else      # M41: $else without a $if
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/rule_if_dangling_else.ext"
                 "$else without a preceding $if")
add_test(NAME rule_if_reject_kindmix            # M41: statement in an expr-position branch
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/rule_if_kindmix.ext"
                 "expected expression")
# techdesign-splices-desugars-sonnet.md §1.2 point 4: a trailing `@attr(A);`
# grouped-attribute group with no following declaration is a parse error, not
# a silently-dropped attribute.
add_test(NAME rule_attr_group_reject_dangling
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/rule_attr_group_dangling.ext"
                 "@attr(...) must precede a declaration")
# Named splice anchor (designs/metaprog-splices/techdesign-splices-named-anchor-
# opus.md §4): the three splice-anchor reject rows. The green fixtures
# (rule_splice_routes{,_twin}, rule_splice_multi, rule_splice_empty) ride the
# corpus_meta_* + expand round-trip lanes above.
add_test(NAME rule_splice_reject_missing         # M42: `at splice X` with no `@X()` site
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/rule_splice_missing.ext"
                 "no '@InjectRoutes()' splice site exists")
add_test(NAME rule_splice_reject_dup             # M42: two `@X()` sites, non-`multi` rule
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/rule_splice_dup.ext"
                 "add 'multi' to fan out or keep one site")
add_test(NAME rule_splice_reject_notattr         # M43: `@X()` site where X is not an attribute
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/rule_splice_notattr.ext"
                 "is not a declared attribute")
add_test(NAME rule_splice_reject_notattr_norule  # M43 fires even with NO rules present
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/rule_splice_notattr_norule.ext"
                 "is not a declared attribute")
# Track 04 M2-M4: stdlib string natives added AFTER the X64Gen freeze
# (toInt/toFloat -> optional, byteAt/fromByte, concatAll — designs/
# techdesign-04-stdlib-strings.md). Kept out of the shared tests/corpus/ dir
# deliberately: the frozen ELF backend (docs/techdesign-portable-backend.md
# — X64Gen.cpp is kept, never extended) never gains these natives, so it
# would fail (wrong output, not a clean SKIP) rather than pass. Runs on the
# four actively-maintained engines: oracle, IR, emit-C++, and LLVM below.
# bug.md #10: integer div/mod by zero throws a catchable RuntimeException on the
# four actively-maintained engines. The frozen ELF backend still returns 0, so
# (same reasoning as strings_native) this dir has NO ELF lane.
add_test(NAME corpus_div_by_zero_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/div_by_zero")
add_test(NAME corpus_div_by_zero_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/div_by_zero")
add_test(NAME corpus_div_by_zero_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/div_by_zero")
add_test(NAME corpus_strings_native_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/strings_native")
add_test(NAME corpus_strings_native_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/strings_native")
add_test(NAME corpus_strings_native_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/strings_native")
# Track 09 (web foundations): encoding/digest/datetime/json are in-language
# code over the post-freeze byteAt/byteToString string natives, so — exactly
# like corpus_strings_native above — the frozen ELF backend can't run them.
# Each rides the four actively-maintained engines (oracle, IR, emit-C++, and
# the LLVM lane added below).
add_test(NAME corpus_encoding_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/encoding")
add_test(NAME corpus_encoding_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/encoding")
add_test(NAME corpus_encoding_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/encoding")
add_test(NAME corpus_datetime_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/datetime")
add_test(NAME corpus_datetime_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/datetime")
add_test(NAME corpus_datetime_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/datetime")
add_test(NAME corpus_digest_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/digest")
add_test(NAME corpus_digest_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/digest")
add_test(NAME corpus_digest_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/digest")
# JSON: parse/render round-trips, \uXXXX + surrogates, deep nesting, malformed
# table. The LLVM lane (below, in the LLVM_FOUND block) is BACK: bug.md #30 (a
# returned Map.with result freed at the return, which the recursive JsonValue
# object model tripped) is fixed — designs/complete/techdesign-bug30-map-with-ownership.md.
# ELF still excluded like the rest of Track 09 (post-freeze byteAt/byteToString).
add_test(NAME corpus_json_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/json")
add_test(NAME corpus_json_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/json")
add_test(NAME corpus_json_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/json")
# HTTP parser hardening (M5): HeaderMap + chunked codec + request/response
# parse/render, exercised without sockets. Uses byteAt (post-freeze) so no ELF
# lane; emit-C++ pulls the whole std namespace's socket natives in via coarser
# reachability and cleanly SKIPs, so no cpp lane either — oracle, IR, and the
# precise-reachability LLVM backend run it for real.
add_test(NAME corpus_http_parse_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/http_parse")
add_test(NAME corpus_http_parse_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/http_parse")
# Track 06 deferred tail: the math transcendental group (float.pow,
# math::log/log2/exp/sin/cos/tan/atan2). Covered by oracle/IR/emit-C++ and
# LLVM; deliberately still excluded from the frozen ELF backend, which keeps
# its clean deferral diagnostic instead of growing libm/SSE polynomial code.
add_test(NAME corpus_math_transcendental_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/math_transcendental")
add_test(NAME corpus_math_transcendental_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/math_transcendental")
add_test(NAME corpus_math_transcendental_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/math_transcendental")
# argv (designs/argv.md §10): env::args() across every engine with a fixed
# argument vector — the five-engine agreement rule, plus the two negative cases
# (frozen ELF defers cleanly; comptime env::args() is denied by hermeticity).
# One dedicated script because passing a program `-- <args>` vector is unique to
# this feature (the generic corpus runners don't). The LLVM group self-skips if
# liblvrt.a isn't next to the binary, so ordering against the lvrt target is not
# required for the test to be meaningful.
add_test(NAME argv_differential
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_argv.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus")
# ship-as-files prelude (techdesign-prelude-ship-as-files-opus.md §6.1):
# per-target segment selection + directory-resolution tiers/fatals. Front-end
# only, no wasm toolchain needed.
add_test(NAME prelude_select
         COMMAND ${CMAKE_SOURCE_DIR}/tests/run_prelude_select.sh
                 $<TARGET_FILE:leviathan> ${CMAKE_SOURCE_DIR}/prelude)
# terminal raw mode (designs/terminal-raw-mode.md §7): non-tty degradation across
# the four non-frozen engines, the frozen-ELF deferral, comptime hermeticity, and
# a pty end-to-end that reads one keystroke with no Enter/echo and verifies the
# terminal is restored even after an UNCAUGHT throw (the runtime exit epilogue).
# The pty groups self-skip without python3/a compiler; a dedicated script because
# driving a pty is unique to this feature.
add_test(NAME term_raw_mode
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_term.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus")
# terminal floor completion (designs/techdesign-terminal-floor.md §7): winsize
# non-tty degradation + real-pty read-back, signal::on delivery / fanout / WINCH
# coalescing, SIGTERM-under-raw restore, emit-C++ sysWinSize-full but signal-
# loop-bound, and comptime hermeticity. Differential across --run/--ir/--build/
# --build-native; the pty groups self-skip without python3/a compiler.
add_test(NAME terminal_floor
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_terminal_floor.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus")
# process exit codes (designs/exit-codes.md §7): the `$?` contract across the four
# non-frozen engines — env.exit(n) / env.setExitCode(n)-then-finish / fall-off /
# uncaught->1 / 8-bit mask / exit-now-abandons-loop — plus the frozen-ELF clean
# deferral of both natives, comptime hermeticity, and a pty terminal-restore on
# exit-now. A dedicated script because asserting per-engine `$?` (not just stdout)
# is unique to this feature; engine groups self-skip without a compiler/liblvrt.a.
add_test(NAME exit_codes
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_exit.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus")
# Track 08 system natives (designs/techdesign-08-system-natives.md §10): the
# environment-dependent halves the golden corpus file omits — env PATH, DNS
# localhost, and a filesystem round-trip. Filesystem behavior is asserted on
# oracle/IR/LLVM; other checks keep their actual per-backend matrix, plus
# comptime hermeticity and clean unsupported diagnostics. A dedicated script
# is needed because the fs round-trip uses a temp dir and property assertions.
add_test(NAME sys_natives
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_sysnatives.sh"
                 $<TARGET_FILE:leviathan>)
# The Track 08 golden property-check file lives in its OWN directory (not the
# shared top-level corpus): it includes natives such as sysIsTty/sysResolve
# that remain interpreter-only, so the LLVM/ELF-scanned `corpus_llvm_full` lane
# must not compile it. The environment-dependent LLVM filesystem lane is above.
add_test(NAME corpus_sys_natives_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/sys_natives")
add_test(NAME corpus_sys_natives_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/sys_natives")
# G-LANG-2 (techdesign-spawn-llvm.md §7.1): the spawn golden, promoted out of
# sys_natives/ into its own dir now that LLVM covers the process floor. All
# three lanes diff the SAME .expected — byte-identity across oracle/IR/LLVM is
# the pass condition (the LLVM sibling lives in the LLVM_FOUND block below).
# emit-C++ keeps its deliberate system-layer deferral (no cpp lane).
add_test(NAME corpus_sys_spawn_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/sys_spawn")
add_test(NAME corpus_sys_spawn_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/sys_spawn")
# G-PTY2 (designs/pty/ docs 01+02): the pty floor golden. All three lanes diff
# the SAME .expected — oracle = IR = LLVM byte-identity is the pass condition
# (the sys_spawn convention; the LLVM sibling lives in the LLVM_FOUND block
# below). Own directory so corpus_llvm_full's top-level scan never compiles it.
# emit-C++ keeps its deliberate system-layer deferral (no cpp lane).
add_test(NAME corpus_sys_pty_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/sys_pty")
add_test(NAME corpus_sys_pty_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/sys_pty")
# Track 10 native floor. Kept outside the frozen ELF lane: sysCpuCount and the
# SO_REUSEPORT overload are interpreter/LLVM worker primitives.
add_test(NAME corpus_threads_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/threads")
add_test(NAME corpus_threads_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/threads")
# Track 10 M1: spawn/Worker<T>/join + flatten-rebuild copy boundary on the
# SERIAL reference engines only (oracle + IR). True OS threads (LLVM) land with
# M3, at which point the spawn corpus gains its native lane; until then this is
# interpreters-only, like div_by_zero's no-ELF lane.
add_test(NAME corpus_threads_serial_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/threads_serial")
add_test(NAME corpus_threads_serial_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/threads_serial")
# LA-30 doc 5 §4: the tasks/ corpus (C1/C2/C3/C4 repros + the doc 3 §5 G9
# recursion-depth probe + bug #35's spawn-global-Promise reject pins), asserting
# the post-M5-flip default (tasks mode; the LANG_PUMP=1 escape hatch is the only
# remaining pump path, deleted at M6). bug #35 is FIXED (reject route A): a spawn
# body that references a Promise through a bare global now rejects loudly and
# byte-identically on every engine (pins: spawn_global_promise_rejected.lev /
# spawn_global_promise_container_rejected.lev), so order_two_awaits/
# order_fifo_three were reshaped to the sanctioned same-thread TaskGroup shape
# and now run on the LLVM lane too. Two files stay interpreters-only: park_storm
# builds 10000 promises via copy-on-write `add`, whose intermediate buffers leak
# on the LLVM runtime's fixed 256 MiB bump heap (O(N^2) — a native COW-array
# reclamation defect independent of #35; the interpreters' unbounded heap
# absorbs it), and recursion_depth.lev targets the INTERPRETER stack-sizing
# question specifically (doc 3 §5, G9; native recursion depth is a distinct,
# uninteresting question — tiny compiled frames vs. the ~KiB-sized interpreter
# frame this file sizes for). The LLVM-safe subset runs separately below via the
# tests/corpus/tasks_llvm/ symlink directory (the corpus_llvm_m3/m4 convention
# above).
add_test(NAME corpus_tasks_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/tasks")
add_test(NAME corpus_tasks_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/tasks")
# Track 10 M3b: the C flatten/rebuild engine, unit-probed via std::sysThreadTransfer
# round-trips (deep/shared/cyclic/collections) — main thread -> main thread, no OS
# threads. Runs on ALL THREE active engines byte-identically (the interpreters'
# lvThreadCopy vs the LLVM C engine); the LLVM lane lives in the LLVM_FOUND block.
add_test(NAME corpus_threads_transfer_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/threads_transfer")
add_test(NAME corpus_threads_transfer_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/threads_transfer")
# bug.md #11: bool == bool / != bool pinning (RuntimeValue.hpp::arithPrim +
# CGen's kRuntime `ar()` both had a never-populated-.i-field hole for Bool,
# same family as the float/string relational holes). LLVM/ELF always computed
# the right answer, so this only needs the three engines that were wrong.
add_test(NAME corpus_bool_cmp_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/bool_cmp")
add_test(NAME corpus_bool_cmp_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/bool_cmp")
add_test(NAME corpus_bool_cmp_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/bool_cmp")
# LA-31 Stage 1 (designs/expr-reification/techdesign-01-prelude.md): the expr::
# reification prelude surface — node taxonomy construction (§3.3), string.like/
# ilike unit rows (§2.4), and the hand-built-tree + expr::eval walker proof
# (§3.2). oracle + IR + LLVM full (the LLVM legs live in the LLVM_FOUND block
# below); no cpp/ELF lanes by design (§1: emit-C++ not required for the expr
# corpus, ELF frozen). Own directories keep them off the top-level scan so
# corpus_native/corpus_elf_full never compile them.
add_test(NAME corpus_expr_prelude_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/expr_prelude")
add_test(NAME corpus_expr_prelude_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/expr_prelude")
add_test(NAME corpus_expr_like_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/expr_like")
add_test(NAME corpus_expr_like_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/expr_like")
add_test(NAME corpus_expr_eval_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/expr_eval")
add_test(NAME corpus_expr_eval_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/expr_eval")
# LA-31 Stage 2 — the reifier: lambda literals in expr::Expr<F> positions reify
# to walkable trees + snapshotted binds + a per-compilation siteId. Own corpus
# dir (kept off the top-level scan, matching Stage 1's posture: emit-C++ not
# required, ELF never gates). LLVM leg alongside its siblings below.
add_test(NAME corpus_expr_reify_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify")
add_test(NAME corpus_expr_reify_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify")
# LA-31 Stage 3 §4 (techdesign-03-verification.md): the differential corpus —
# closure leg (.fn) vs expr::eval leg agreement per whitelisted op, None-operand
# semantics, the like/ilike escape/fold edge pins re-run through reified trees,
# and the two R5 snapshot rows. Own dir, same posture as expr_reify (oracle +
# IR + LLVM; no emit-C++/ELF lanes).
add_test(NAME corpus_expr_diff_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/expr_diff")
add_test(NAME corpus_expr_diff_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/expr_diff")
# The four reification-reject smokes (§8.3/§8.4): each is a hard compile error
# carrying the exact named diagnostic the negative corpus greps.
add_test(NAME expr_reify_reject_block
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_smoke_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_smoke_block.lev"
                 "cannot reify block body: outside the LA-31 reifiable subset")
add_test(NAME expr_reify_reject_call
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_smoke_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_smoke_call.lev"
                 "reifiable calls: string.like/1, string.ilike/1, string.startsWith/1, string.endsWith/1, string.contains/1, Array.contains/1")
add_test(NAME expr_reify_reject_value
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_smoke_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_smoke_value.lev"
                 "only a lambda literal can be reified to expr::Expr<F>")
add_test(NAME expr_reify_reject_ambig
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_smoke_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_smoke_ambig.lev"
                 "ambiguous lambda argument: matches both a function parameter and an expr::Expr parameter")
# LA-31 Stage 3 §3 (techdesign-03-verification.md): the full 14-row negative
# corpus — every reject construct in the ask's taxonomy, substring-checked.
# `interp` asserts the actual diagnostic (non-whitelisted call 'toString'),
# not the doc's literal "string interpolation" — logged as a finding in the
# design doc's §10: interpolation desugars to a +/.toString() chain at parse
# time (Parser.cpp:1709 parseInterpolatedString), so no distinct ExprKind
# survives to the reifier for a dedicated named rejection.
add_test(NAME expr_reify_row_call
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_call.lev"
                 "non-whitelisted call 'toUpper'")
add_test(NAME expr_reify_row_await
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_await.lev"
                 "cannot reify await")
add_test(NAME expr_reify_row_nested
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_nested.lev"
                 "nested lambda")
add_test(NAME expr_reify_row_block
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_block.lev"
                 "block body")
add_test(NAME expr_reify_row_assign_pos
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_assign_pos.lev"
                 "assignment outside a set-shaped lambda")
add_test(NAME expr_reify_row_capmut
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_capmut.lev"
                 "mutation of a capture")
add_test(NAME expr_reify_row_is
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_is.lev"
                 "'is' expression")
add_test(NAME expr_reify_row_match
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_match.lev"
                 "'match' expression")
add_test(NAME expr_reify_row_interp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_interp.lev"
                 "non-whitelisted call 'toString'")
add_test(NAME expr_reify_row_index
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_index.lev"
                 "indexing")
add_test(NAME expr_reify_row_ternary
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_ternary.lev"
                 "conditional expression")
add_test(NAME expr_reify_row_captype
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_captype.lev"
                 "capture of a non-value type")
add_test(NAME expr_reify_row_value
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_value.lev"
                 "only a lambda literal can be reified")
add_test(NAME expr_reify_row_ambig
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_error.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/negative/expr_reify_ambig.lev"
                 "ambiguous lambda argument")
# LA-31 Stage 3 §2: hand-written-twin byte-equivalence — --expand of a reified
# corpus file's site must print byte-identically to its hand-authored twin.
add_test(NAME expr_reify_twin_r01
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_twin.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify/expr_reify_r01_field.lev"
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify/expr_reify_twin_r01.lev")
add_test(NAME expr_reify_twin_r04
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_twin.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify/expr_reify_r04_range.lev"
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify/expr_reify_twin_r04.lev")
add_test(NAME expr_reify_twin_r12
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_twin.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify/expr_reify_r12_like.lev"
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify/expr_reify_twin_r12.lev")
add_test(NAME expr_reify_twin_r15
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_twin.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify/expr_reify_r15_arraycapture.lev"
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify/expr_reify_twin_r15.lev")
add_test(NAME expr_reify_twin_r18
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expr_reify_twin.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify/expr_reify_r18_assign.lev"
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify/expr_reify_twin_r18.lev")
# Track 03 §2 — enum: value struct + members + code()/toString()/==/fromCode +
# exhaustive match. Desugar-only (no VKind, no ABI, no X64Gen edits), so it is
# FULL-COVERAGE on all four non-frozen engines; enums.lev runs byte-identical on
# run/ir/cpp/llvm (the LLVM lane is added alongside its siblings below).
add_test(NAME corpus_enums_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/enums")
add_test(NAME corpus_enums_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/enums")
add_test(NAME corpus_enums_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/enums")
# Track 03 §1 — char: Unicode-scalar value primitive. Ships on oracle + IR +
# emit-C++ + LLVM (the LV_CHAR ABI addendum landed — deferal-char-block-abi.md;
# corpus_chars_llvm below). No ELF lane: X64Gen is frozen (M4 stays deferred).
add_test(NAME corpus_chars_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/chars")
add_test(NAME corpus_chars_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/chars")
add_test(NAME corpus_chars_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/chars")
# Track 03 §3 — Block: fixed-length mutable byte buffer (contract C4). A new VKind
# with reference/aliasing semantics, on oracle + IR + emit-C++ + LLVM (the LV_BLOCK
# ABI addendum landed — deferal-char-block-abi.md; corpus_blocks_llvm below, heap
# tag + slice-retains-root ARC in lv_runtime.c). No ELF lane: X64Gen is frozen, so
# M4 (Block on ELF + the sysRead/sysWrite Block I/O overloads) stays deferred.
add_test(NAME corpus_blocks_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/blocks")
add_test(NAME corpus_blocks_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/blocks")
add_test(NAME corpus_blocks_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/blocks")
# Track 03 M4 — zero-copy Block I/O overloads (sysRead/Write/Send/Recv + File).
# Arity-distinct from the string sys forms; deterministic (stdout + /dev/zero).
# All four actively-maintained engines (cpp/llvm siblings below). No ELF lane
# (X64Gen frozen). Send/Recv Block share the write/read paths exercised here.
add_test(NAME corpus_blocks_io_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/blocks_io")
add_test(NAME corpus_blocks_io_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/blocks_io")
# Track 05 M1: the C3 key-equality rule (struct-recursive Map keys). Same
# reasoning as corpus_strings_native_* above: the frozen ELF backend
# (X64Gen.cpp) keeps the pre-C3 identity-only compare deliberately, so this
# would fail (wrong output) rather than pass there. Runs on the same four
# actively-maintained engines (LLVM lane added below, alongside its siblings).
add_test(NAME corpus_set_map_keys_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/set_map_keys")
add_test(NAME corpus_set_map_keys_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/set_map_keys")
add_test(NAME corpus_set_map_keys_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/set_map_keys")
# bug.md #19: Map.with()/without() named-method natives, fixed on LLVM here.
# Same reasoning as corpus_set_map_keys_* above: the frozen ELF backend
# (X64Gen.cpp) still lacks them, so this stays out of the shared ELF-scanned
# corpus (LLVM lane added below, alongside its siblings).
add_test(NAME corpus_map_with_without_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/map_with_without")
add_test(NAME corpus_map_with_without_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/map_with_without")
add_test(NAME corpus_map_with_without_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/map_with_without")
# bug.md #30: a with/without map RETURNED across a frame — the ownership case the
# in-frame map_with_without corpus above never exercised. Same ELF-frozen
# reasoning: out of the shared corpus, runs on oracle/IR/emit-C++/LLVM (LLVM lane
# added below). See designs/complete/techdesign-bug30-map-with-ownership.md.
add_test(NAME corpus_map_return_ownership_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/map_return_ownership")
add_test(NAME corpus_map_return_ownership_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/map_return_ownership")
add_test(NAME corpus_map_return_ownership_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/map_return_ownership")
# bug.md #12: float division by zero is IEEE (inf/nan) on every engine, fixed
# on LLVM here. Kept out of the shared corpus: the frozen ELF backend was
# already IEEE-correct but renders 0.0/0.0 as "nan" (no leading minus) where
# every other engine prints "-nan" — a pre-existing, cosmetic sign-of-NaN
# formatting difference this fix doesn't touch (LLVM lane added below).
add_test(NAME corpus_float_div_zero_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/float_div_zero")
add_test(NAME corpus_float_div_zero_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/float_div_zero")
add_test(NAME corpus_float_div_zero_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/float_div_zero")
# bug.md #32: namespace-local class construction (qualified `ns::Class(...)`,
# bare-inside-the-namespace, and the `C::Label(...)` labeled-ctor
# disambiguation) — designs/techdesign-namespace-ctor-dispatch.md. Full
# coverage: the checker already resolves this for user code, so this is the
# regression guard; the real unchecked-path proof is the regex engine's
# internals living back inside `namespace regex` (tests/corpus/regex_engine).
add_test(NAME corpus_namespace_ctor_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/namespace_ctor")
add_test(NAME corpus_namespace_ctor_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/namespace_ctor")
add_test(NAME corpus_namespace_ctor_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/namespace_ctor")
# bug.md #33: Array<Struct>.at(i) must recognize LLVM's dense-array marker,
# bounds-check against the logical length, and address the inline record rather
# than a boxed 16-byte slot. Kept as a four-maintained-engine regression lane;
# the frozen ELF backend already has its own dense branch.
add_test(NAME corpus_dense_array_at_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/dense_array_at")
add_test(NAME corpus_dense_array_at_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/dense_array_at")
add_test(NAME corpus_dense_array_at_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/dense_array_at")
# F3: bound `obj.method` references are checker-rewritten ordinary lambdas.
# Pin receiver snapshot semantics, overload targeting, runtime dispatch,
# constructor `this`, container context, and source-printer round-tripping.
add_test(NAME corpus_bound_method_refs_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run
                 "${CMAKE_SOURCE_DIR}/tests/corpus/bound_method_refs")
add_test(NAME corpus_bound_method_refs_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir
                 "${CMAKE_SOURCE_DIR}/tests/corpus/bound_method_refs")
add_test(NAME corpus_bound_method_refs_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/bound_method_refs")
# bug.md #53: a lambda's bare-receiver self-method call must dispatch on the
# captured `this`, not the closure occupying r0 in the lambda frame. Four
# active-engine lanes (no ELF — closures aren't a frozen-backend target).
add_test(NAME corpus_lambda_self_call_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run
                 "${CMAKE_SOURCE_DIR}/tests/corpus/lambda_self_call")
add_test(NAME corpus_lambda_self_call_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir
                 "${CMAKE_SOURCE_DIR}/tests/corpus/lambda_self_call")
add_test(NAME corpus_lambda_self_call_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/lambda_self_call")
add_test(NAME corpus_weak_references_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run
                 "${CMAKE_SOURCE_DIR}/tests/corpus/weak_references")
add_test(NAME corpus_weak_references_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir
                 "${CMAKE_SOURCE_DIR}/tests/corpus/weak_references")
add_test(NAME corpus_weak_references_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/weak_references")
add_test(NAME corpus_weak_references_expand
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expand_roundtrip.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/weak_references")
add_test(NAME corpus_bound_method_refs_expand
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expand_roundtrip.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/bound_method_refs")
# LA-31 Stage 2 §8.5: --expand shows the expr::Expr::<Fn>(...) construction and the
# emitted source recompiles byte-identically (no double reification, R1).
add_test(NAME corpus_expr_reify_expand
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expand_roundtrip.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify")
# Explicit generic call arguments are erased after checker resolution. Pin the
# authored/printed syntax plus byte-identical behavior on every active engine;
# frozen ELF is intentionally outside this feature's acceptance set.
add_test(NAME corpus_explicit_generic_call_args_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run
                 "${CMAKE_SOURCE_DIR}/tests/corpus/explicit_generic_call_args")
add_test(NAME corpus_explicit_generic_call_args_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir
                 "${CMAKE_SOURCE_DIR}/tests/corpus/explicit_generic_call_args")
add_test(NAME corpus_explicit_generic_call_args_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/explicit_generic_call_args")
add_test(NAME corpus_explicit_generic_call_args_expand
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expand_roundtrip.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/explicit_generic_call_args")
# bug.md #88 fix: was unwired because two expr::Call construction sites
# sharing a whitelisted name with different receiver kinds (expr_diff_calls.lev's
# string- and Array-receiver "contains") silently corrupted the Array-receiver
# site's tree walk on round-trip; root cause was srcStmtInline (AstPrinter.cpp)
# dropping any While/DoWhile/For/ForIn/Try statement nested directly in a
# lambda/match-arm block, not the reifier/checker. Now green.
add_test(NAME corpus_expr_diff_expand
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_expand_roundtrip.sh"
                 $<TARGET_FILE:leviathan>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/expr_diff")
# LA-18: concrete AST/IR copies for generic `T::member` on class + struct
# tuples, including LA-25 value-position composition, transitive demand, and a
# non-overridden generic instance method. Specializations are ordinary
# functions, so the frozen ELF lane is free (no X64Gen extension).
add_test(NAME corpus_generic_static_members_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/generic_static_members")
add_test(NAME corpus_generic_static_members_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/generic_static_members")
add_test(NAME corpus_generic_static_members_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/generic_static_members")
add_test(NAME corpus_generic_static_members_elf
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_elf.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/generic_static_members")
# F6: interface requirements admit covariant declared-type returns. The
# requirement remains slotless, so the runtime proof is identical on all four
# maintained engines and needs no backend changes.
add_test(NAME corpus_covariant_return_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/covariant_return")
add_test(NAME corpus_covariant_return_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/covariant_return")
add_test(NAME corpus_covariant_return_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/covariant_return")
# --- Composition corpus (designs/complete/techdesign-composition-corpus.md) ---
# Interaction-cluster regression suite: tests/corpus/composition/{fnvalues,
# aggregates,names,generics}/{green,red}. green/ is the must-pass regression
# floor (working neighbors + fixed-bug pins, one add_test per lane below,
# looped over all four cluster subdirectories under the existing
# run_corpus.sh/run_native.sh discipline). red/ (composition_red, in the
# LLVM_FOUND block below since it needs LANG_LVRT_SOURCES) is one minimal
# repro per currently-open bug, run by run_red_corpus.sh with INVERTED exit
# semantics — see that script's header. This track owns
# tests/corpus/composition/**, tests/run_red_corpus.sh, and this delimited
# block; it does not touch src/, packages/, or any other track's corpus.
set(LANG_COMPOSITION_CLUSTERS fnvalues aggregates names generics equality)
list(JOIN LANG_COMPOSITION_CLUSTERS " " LANG_COMPOSITION_CLUSTERS_SP)
add_test(NAME composition_treewalk
         COMMAND bash -c "for d in ${LANG_COMPOSITION_CLUSTERS_SP}; do bash '${CMAKE_SOURCE_DIR}/tests/run_corpus.sh' '$<TARGET_FILE:leviathan>' --run '${CMAKE_SOURCE_DIR}/tests/corpus/composition/'$d/green || exit 1; done")
add_test(NAME composition_ir
         COMMAND bash -c "for d in ${LANG_COMPOSITION_CLUSTERS_SP}; do bash '${CMAKE_SOURCE_DIR}/tests/run_corpus.sh' '$<TARGET_FILE:leviathan>' --ir '${CMAKE_SOURCE_DIR}/tests/corpus/composition/'$d/green || exit 1; done")
add_test(NAME composition_cpp
         COMMAND bash -c "for d in ${LANG_COMPOSITION_CLUSTERS_SP}; do bash '${CMAKE_SOURCE_DIR}/tests/run_native.sh' '$<TARGET_FILE:leviathan>' '${CMAKE_SOURCE_DIR}/tests/corpus/composition/'$d/green || exit 1; done")
# --- end composition corpus (treewalk/ir/cpp legs; red + llvm below) ---
# Track 10 regex engine M1-M5: ordered Pike captures, all v1 syntax and
# prefilters, lazy DFA (including forced budget fallback), comptime programs,
# and batch hooks. The frozen ELF backend is explicitly outside this gate.
add_test(NAME corpus_regex_engine_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/regex_engine")
add_test(NAME corpus_regex_engine_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/regex_engine")
add_test(NAME corpus_regex_engine_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/regex_engine")
add_test(NAME regex_comptime_error
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_regex_comptime_error.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/negative/regex_bad_comptime.lev")
# 005 R4: a type used as a value in a `match` arm (`C::f =>`) is a compile error,
# not a silent `else`. The R4 Checker backstop over the R2 resolver classifier.
add_test(NAME match_pattern_error
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_match_pattern_error.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/negative/match_type_as_value.lev")
# system-binds.md §6.4/§9 M3: a capability injection inside a comptime-folded
# root has no bind in scope (comptime folding predates the checker's
# bind-scope pass) — denied at the injection site.
add_test(NAME system_binds_comptime_error
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_system_binds_comptime.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/negative/system_binds_comptime_inject.lev")
add_test(NAME regex_pathological_linear
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_regex_linear.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/fuzz/regex_linear.lev")
# Track 10 regex library (techdesign-regex-library.md, doc 2): the public
# Regex/Match/Group/RegexOptions/RegexException surface + namespace regex
# conveniences fronting the engine above. Same ELF-excluded gate.
add_test(NAME corpus_regex_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/regex")
add_test(NAME corpus_regex_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/regex")
add_test(NAME corpus_regex_cpp
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/regex")
add_test(NAME regex_unnarrowed_find_error
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_regex_unnarrowed_find_error.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/negative/regex_unnarrowed_find.lev")
# techdesign-regex-linear-gate.md D3: the gate asserts a DETERMINISTIC step-count
# ratio, so this TIMEOUT is a pure hang-guard (catch a genuine superlinear/
# exponential blow-up), not a throughput judge — generous margin over the linear
# in-language run (~5s on --ir) so host speed / parallel load never false-fails it.
set_tests_properties(regex_pathological_linear PROPERTIES TIMEOUT 30)
add_test(NAME corpus_core_treewalk
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --run "${CMAKE_SOURCE_DIR}/tests/corpus/core")
add_test(NAME corpus_ir_verify
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_verify.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus")
add_test(NAME corpus_mem_verify
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_memverify.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus")
add_test(NAME corpus_core_native
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/core")
# Native OO: objects/methods/ctors/operators/accessors compiled to a binary
# via the emit-C++ backend (identical to both interpreters). LLVM object parity
# is a follow-up; LLVM stays on the scalar core.
add_test(NAME corpus_native
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus")
# Pure zero-dependency backend: own x86-64 + ELF, no g++/libc/linker.
add_test(NAME corpus_elf
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_elf.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/elf")
add_test(NAME corpus_elf_core
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_elf.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/core")
# Step 3: the pure backend over the whole corpus — real I/O (files, stdin,
# streams) verified against .expected; event-loop programs still skip until
# their syscall floor lands (each flips from SKIP to verified as it does).
add_test(NAME corpus_elf_full
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_elf.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus")

# The full-corpus runs execute http.ext / async.ext, which bind fixed loopback
# ports (8092/8093). Under `ctest -j`, two engines' servers contend and hang
# each other — and a killed run leaves an orphaned server squatting the port
# for the next one. Serialize exactly these tests on a shared lock; everything
# else still parallelizes.
set_tests_properties(corpus_treewalk corpus_ir corpus_ir_verify
                     corpus_mem_verify corpus_elf corpus_elf_full
                     PROPERTIES RESOURCE_LOCK corpus_net_ports)

# --- TLS (LA-2) --- designs/complete/techdesign-tls-crypto.md §12.
# One consolidated lane (run_tls.sh) covers the tls corpus on oracle + IR + LLVM
# PLUS the RSA round-trip, sysRandom, comptime denial, loud-reject wording, and
# emit-C++ deferral — it sets SSL_CERT_FILE to the hermetic test CA and strips
# the native [heap] trailer, which the generic run_corpus.sh cannot. Gated on
# OpenSSL (the runtime otherwise ships the not-built provider and the script
# self-SKIPs) and locked on corpus_net_ports (TLS corpus binds loopback 8443/8444).
if (OpenSSL_FOUND)
  add_test(NAME tls_integration
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_tls.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tls_integration PROPERTIES
                       RESOURCE_LOCK corpus_net_ports TIMEOUT 240)
endif()
# --- end TLS (LA-2) ---

# --- HTTP streaming responses (LA-HTTP-STREAM) ---
# designs/complete/techdesign-http_streaming_response-*.md §7. One consolidated
# lane (run_http_streaming.sh) drives the http_streaming corpus — head/framing,
# SSE return-before-end, keep-alive across the stream boundary, writer-throw,
# pipelining-abort, bodyless/version, the flush barrier, sink abort, teardown
# churn, and transport backpressure — on oracle / IR / LLVM, then asserts the
# teardown churn's reachable root set is constant in N. Binds loopback
# 8096-8102 / 18097 / 18110 / 18114, so it holds corpus_net_ports.
add_test(NAME http_streaming_integration
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_http_streaming.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}")
set_tests_properties(http_streaming_integration PROPERTIES
                     RESOURCE_LOCK corpus_net_ports TIMEOUT 600)
# --- end HTTP streaming responses ---

# Project / file system (§12): multi-file programs gathered into one logical
# compilation unit via a manifest, resolved through trident into a build plan
# (§3.3) — trident is the front door (GT1); leviathan never parses a manifest.
# Oracle == IR == ELF == the concatenation of the sources; cross-file
# namespaces, re-opened namespaces, and cross-file class use all resolve
# through the whole-program gather.
add_test(NAME corpus_project
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_project.sh"
                 $<TARGET_FILE:leviathan> $<TARGET_FILE:trident>
                 "${CMAKE_SOURCE_DIR}/tests/corpus/project")
# --- Track A: compiler build-plan input (techdesign-toolchain.md §5.1) ---
# Build-plan contract conformance: hand-written plans (no trident involved)
# must drive leviathan --plan to the right result, for a File-entry, a
# Function-entry, and a phantom-dep case — permanent regression insurance for
# the frozen §3.3 wire format.
add_test(NAME plan_contract_conformance
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_plan.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/project")
# --- end Track A ---
# --- Track B: trident package manager (techdesign-toolchain.md §5.2) ---
# H-2: manifest-*format* errors are trident's own diagnostics (semantic errors
# — phantom-dep, entry-rule, use-scoping — stay the compiler's, covered above
# through corpus_project's trident-resolved plans).
add_test(NAME trident_manifest_errors
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_trident_manifest_errors.sh"
                 $<TARGET_FILE:trident> "${CMAKE_SOURCE_DIR}/tests/trident/manifest_errors")
# P2.1e/GT3 (techdesign-package-manager.md §5.5): `trident add`/`build`/`why`
# against a real (offline, H-7) VCS dep — see tests/run_trident_vcs_app.sh's
# own header for the git url.insteadOf redirect this relies on.
add_test(NAME trident_vcs_app
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_trident_vcs_app.sh"
                 $<TARGET_FILE:leviathan> $<TARGET_FILE:trident>
                 "${CMAKE_SOURCE_DIR}/tests/trident/vcs_app"
                 "${CMAKE_SOURCE_DIR}/tests/trident/store/fixture_json.git")
# P2.2 (techdesign-package-manager.md §6, GT4): `trident audit` (clean pass +
# tamper detection) and `trident vendor` + `--vendor` (a genuinely
# network-free build, git shadowed out) — see tests/run_trident_vendor.sh's
# own header. Reuses the vcs_app fixture (same offline VCS dep).
add_test(NAME trident_vendor
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_trident_vendor.sh"
                 $<TARGET_FILE:leviathan> $<TARGET_FILE:trident>
                 "${CMAKE_SOURCE_DIR}/tests/trident/vcs_app"
                 "${CMAKE_SOURCE_DIR}/tests/trident/store/fixture_json.git")
# P2.3/GT5: the optional static proxy is a real ModuleProvider. Git is
# deliberately shadowed out while MVS, fetch, build, and execution succeed.
add_test(NAME trident_proxy
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_trident_proxy.sh"
                 $<TARGET_FILE:leviathan> $<TARGET_FILE:trident>
                 "${CMAKE_SOURCE_DIR}/tests/trident/vcs_app"
                 "${CMAKE_SOURCE_DIR}/tests/trident/store/fixture_json.git")
# P2.3-P2.4 / GT5-GT6: publish + thin index + yank-never-delete, followed by
# a trusted human audit and public-key-verified provenance policy.
add_test(NAME trident_publish_policy
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_trident_publish_policy.sh"
                 $<TARGET_FILE:trident>
                 "${CMAKE_SOURCE_DIR}/tests/trident/vcs_app")
# --- end Track B ---
# ARC leak net (§15): churn programs allocate+drop N escaping objects per
# iteration; the escaping-tier accounting meter's live-at-exit must stay flat
# as N grows (the ARC must reclaim the drops), cross-checked against the
# --mem-verify root set. GREEN since the escaping-tier free landed (71ede9d);
# expected-red future-work targets carry a first-line `// XFAIL:` marker and
# are reported without failing the suite (XPASS = fixed, flip the marker off).
add_test(NAME corpus_churn_leak
         COMMAND python3 "${CMAKE_SOURCE_DIR}/fuzz/churn_leak.py"
                 --leviathan $<TARGET_FILE:leviathan>
                 --corpus "${CMAKE_SOURCE_DIR}/tests/corpus/churn")
if (LLVM_FOUND)
  # Track A (techdesign-portable-backend.md A-M2/A-M3/A-M5): the LLVM lanes link
  # the real runtime v2 (runtime/lv_runtime.c + posix floor) since Track B's
  # B-M1 landed on master — the A-M2-era llvm_stub_runtime.c is retired. The
  # runner compiles the runtime sources once (as C) and reuses the objects per
  # program. A-M5 adds lv_loop.c (event loop) and lv_entry.c (runtime-owned
  # main() — the entry flip emits lv_main, so the linker now needs it).
  # LA-2: the not-built TLS provider resolves the lv_tls_* routing symbols in
  # lv_runtime.c/lv_loop.c for these plaintext corpus lanes (no TLS calls, no
  # OpenSSL dep — compiles with plain cc). The real openssl provider + its link
  # flags ride the tls_integration lane's --build-native path (lvrt.link) instead.
  # LA-30 doc 2: lv_runtime.c's lvrt_await parks through the task substrate, so
  # the shell-lane runtime list carries lv_task.c + the arch-self-guarded .S
  # switches too (the lvrt archive target below already did).
  set(LANG_LVRT_SOURCES
      "${CMAKE_SOURCE_DIR}/runtime/lv_runtime.c"
      "${CMAKE_SOURCE_DIR}/runtime/lv_plat_posix.c"
      "${CMAKE_SOURCE_DIR}/runtime/lv_loop.c"
      "${CMAKE_SOURCE_DIR}/runtime/lv_thread.c"
      "${CMAKE_SOURCE_DIR}/runtime/lv_proc.c"
      "${CMAKE_SOURCE_DIR}/runtime/lv_pty.c"
      "${CMAKE_SOURCE_DIR}/runtime/lv_entry.c"
      "${CMAKE_SOURCE_DIR}/runtime/lv_task.c"
      "${CMAKE_SOURCE_DIR}/runtime/lv_ctx_x86_64.S"
      "${CMAKE_SOURCE_DIR}/runtime/lv_ctx_aarch64.S"
      "${CMAKE_SOURCE_DIR}/runtime/lv_tls_none.c")
  list(JOIN LANG_LVRT_SOURCES " " LANG_LVRT_SOURCES_SP)
  add_test(NAME corpus_core_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/core"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_procedural_macros_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan>
                   "${CMAKE_SOURCE_DIR}/tests/corpus/meta/procedural_macros.lev"
                   ${LANG_LVRT_SOURCES})
  # Item Q: the target-predicate uses-selection on the LLVM leg (see
  # corpus_target_uses_cpp above).
  add_test(NAME corpus_target_uses_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan>
                   "${CMAKE_SOURCE_DIR}/tests/corpus/meta/target_uses.lev"
                   ${LANG_LVRT_SOURCES})
  # bug.md #10: div/mod-by-zero throw on the LLVM backend (see corpus_div_by_zero_*
  # above; no ELF lane since the frozen backend still returns 0).
  add_test(NAME corpus_div_by_zero_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/div_by_zero"
                   ${LANG_LVRT_SOURCES})
  # Track 04 M2-M4 new-native lane (see corpus_strings_native_* above for why
  # this is a separate directory, not the shared ELF-scanned corpus).
  add_test(NAME corpus_strings_native_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/strings_native"
                   ${LANG_LVRT_SOURCES})
  # Track 09 web-foundations LLVM lanes (see corpus_encoding_* above).
  add_test(NAME corpus_encoding_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/encoding"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_datetime_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/datetime"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_digest_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/digest"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_http_parse_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/http_parse"
                   ${LANG_LVRT_SOURCES})
  # bug.md #30 fixed: the recursive-JsonValue object model (Map<string,JsonValue>)
  # built via Map.with and returned across frames now crosses OWNED — the LLVM
  # lane this bug forced off is back. See designs/complete/techdesign-bug30-map-with-ownership.md.
  add_test(NAME corpus_json_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/json"
                   ${LANG_LVRT_SOURCES})
  # Track 05 M1 new-native lane (see corpus_set_map_keys_* above for why this
  # is a separate directory, not the shared ELF-scanned corpus).
  add_test(NAME corpus_set_map_keys_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/set_map_keys"
                   ${LANG_LVRT_SOURCES})
  # bug.md #19: Map.with()/without() on LLVM (see corpus_map_with_without_*
  # above for why this is a separate directory, not the shared ELF-scanned
  # corpus).
  add_test(NAME corpus_map_with_without_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/map_with_without"
                   ${LANG_LVRT_SOURCES})
  # bug.md #30: returned with/without maps — the ownership fix's regression floor
  # (this lane segfaulted / read poison before lvrt_map_with + the consumed-
  # receiver release; see corpus_map_return_ownership_* above).
  add_test(NAME corpus_map_return_ownership_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/map_return_ownership"
                   ${LANG_LVRT_SOURCES})
  # bug.md #12: float division by zero on LLVM (see corpus_float_div_zero_*
  # above for why this is a separate directory, not the shared ELF-scanned
  # corpus).
  add_test(NAME corpus_float_div_zero_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/float_div_zero"
                   ${LANG_LVRT_SOURCES})
  # bug.md #32 LLVM lane (see corpus_namespace_ctor_treewalk above).
  add_test(NAME corpus_namespace_ctor_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/namespace_ctor"
                   ${LANG_LVRT_SOURCES})
  # system-binds.md (Channel 1 capability injection): LLVM lane for
  # di_capabilities.lev — treewalk/ir/native are covered by the generic
  # whole-corpus lanes above (corpus_treewalk/corpus_ir/corpus_native, the
  # last of which SKIPs this file on emit-C++'s unrelated reachability
  # over-marking gap, not an LLVM limitation); a dedicated single-file entry
  # is needed because run_native_llvm.sh has no skip-on-unsupported path.
  add_test(NAME corpus_di_capabilities_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/di_capabilities.lev"
                   ${LANG_LVRT_SOURCES})
  # bug.md #70 LLVM regression lane (see corpus_namespace_ctor_llvm above for
  # why a dedicated entry: treewalk/ir/native are the generic whole-corpus
  # lanes, native SKIPs it on the same emit-C++ reachability gap as
  # di_capabilities, and run_native_llvm.sh has no skip-on-unsupported path).
  add_test(NAME corpus_namespace_shadow_dispatch_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/namespace_shadow_dispatch.lev"
                   ${LANG_LVRT_SOURCES})
  # bug.md #33 LLVM regression lane (see corpus_dense_array_at_treewalk above).
  add_test(NAME corpus_dense_array_at_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/dense_array_at"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_bound_method_refs_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan>
                   "${CMAKE_SOURCE_DIR}/tests/corpus/bound_method_refs"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_lambda_self_call_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/lambda_self_call"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_weak_references_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/weak_references"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_covariant_return_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/covariant_return"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_generic_static_members_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/generic_static_members"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_explicit_generic_call_args_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan>
                   "${CMAKE_SOURCE_DIR}/tests/corpus/explicit_generic_call_args"
                   ${LANG_LVRT_SOURCES})
  # --- Composition corpus LLVM leg + red lane (see the treewalk/ir/cpp block
  # above) — the red runner needs LANG_LVRT_SOURCES for any repro whose
  # .engines lists 'llvm', so both live in this LLVM_FOUND block.
  add_test(NAME composition_llvm
           COMMAND bash -c "for d in ${LANG_COMPOSITION_CLUSTERS_SP}; do bash '${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh' '$<TARGET_FILE:leviathan>' '${CMAKE_SOURCE_DIR}/tests/corpus/composition/'$d/green ${LANG_LVRT_SOURCES_SP} || exit 1; done")
  add_test(NAME composition_red
           COMMAND bash -c "for d in ${LANG_COMPOSITION_CLUSTERS_SP}; do bash '${CMAKE_SOURCE_DIR}/tests/run_red_corpus.sh' '$<TARGET_FILE:leviathan>' '${CMAKE_SOURCE_DIR}/tests/corpus/composition/'$d/red ${LANG_LVRT_SOURCES_SP} || exit 1; done")
  # --- end composition corpus ---
  # Curated object-bearing programs (A-M2's first heap-producing ops: NewObject,
  # RawGet/RawSet fixed-slot, CallDyn resolved-direct calls). Folds into a full
  # corpus_llvm lane at A-M5 per the doc's §10.1 test-lane table.
  add_test(NAME corpus_llvm_objects
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/llvm_objects"
                   ${LANG_LVRT_SOURCES})
  # A-M3 acceptance lane (doc §6): collections, COW value semantics, strings,
  # closures — symlinked from the shared corpus so the .expected bytes stay
  # single-sourced.
  add_test(NAME corpus_llvm_m3
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/llvm_m3"
                   ${LANG_LVRT_SOURCES})
  # A-M4 acceptance lane (doc §7): exceptions — throw / try-catch by type, base,
  # interface / cross-frame propagation / OOB-raise / uncaught-at-top, plus the
  # is/match narrowing that shares the machinery (IsType). Symlinked so the
  # .expected bytes stay single-sourced with the treewalk/ir/elf lanes.
  add_test(NAME corpus_llvm_m4
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/llvm_m4"
                   ${LANG_LVRT_SOURCES})
  # §10.2 timed COW smoke: ~1M indexed stores must finish inside the wall-clock
  # budget — a silent COW regression (in-place path lost) turns this O(n^2) and
  # trips the timeout loudly.
  add_test(NAME corpus_llvm_cow_smoke
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_llvm_timed.sh"
                   $<TARGET_FILE:leviathan>
                   "${CMAKE_SOURCE_DIR}/tests/corpus/llvm_smoke/cow_timed.ext" 2
                   ${LANG_LVRT_SOURCES})
  # A-M5 = Gate G1 (doc §8/§10.1): full IR parity for the LLVM backend.
  # corpus_llvm runs the elf-parity subset (tests/corpus/elf/); corpus_llvm_full
  # runs the whole top-level corpus (the binding G1 checklist, A-M2 step-0 log).
  # Both link the full runtime v2 (event loop + net natives + runtime-owned
  # main() via the A-M5 entry flip) and diff byte-for-byte against the oracle.
  add_test(NAME corpus_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/elf"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_llvm_full
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus"
                   ${LANG_LVRT_SOURCES})
  # Track 03 §2 enum: LLVM lane (full-coverage sibling of corpus_enums_{treewalk,ir,cpp}).
  add_test(NAME corpus_enums_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/enums"
                   ${LANG_LVRT_SOURCES})
  # Track 03 §1 char + §3 Block: LLVM legs of the LV_CHAR/LV_BLOCK ABI addendum
  # (deferal-char-block-abi.md). char is a pure immediate (tag 10); Block a heap
  # type (tag 11) with slice-retains-root ARC + recursive-free in lv_runtime.c.
  # Byte-identical against the oracle .expected, single-sourced with the
  # treewalk/ir/cpp lanes above. (No ELF sibling — X64Gen frozen; M4 deferred.)
  add_test(NAME corpus_chars_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/chars"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_blocks_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/blocks"
                   ${LANG_LVRT_SOURCES})
  # Track 03 M4 — Block I/O overloads on LLVM (lvrt_sys*_block in lv_runtime.c,
  # arity-selected in emitNativeRows). No cpp sibling: emit-C++ defers the
  # file/socket layer by design (sysOpen/sysRead), so the treewalk/ir lanes
  # above pair only with this LLVM lane on the compiled side.
  add_test(NAME corpus_blocks_io_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/blocks_io"
                   ${LANG_LVRT_SOURCES})
  # G-LANG-2 (techdesign-spawn-llvm.md §7.1): the LLVM leg of the spawn golden —
  # same .expected as corpus_sys_spawn_{treewalk,ir} above (byte-identity).
  add_test(NAME corpus_sys_spawn_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/sys_spawn"
                   ${LANG_LVRT_SOURCES})
  # G-PTY2 (designs/pty/ 02 §6.1): the LLVM leg of the pty golden — the SAME
  # .expected as corpus_sys_pty_{treewalk,ir} above. Three-lane byte-identity
  # is the pass condition (same kernel + frozen termios ⇒ achievable, R§D.5).
  add_test(NAME corpus_sys_pty_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/sys_pty"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_threads_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/threads"
                   ${LANG_LVRT_SOURCES})
  # LA-30 doc 5 §4: the LLVM-safe subset of tasks/ (symlinked, single-sourced
  # .expected bytes — the corpus_llvm_m3/m4 convention). With bug #35 fixed
  # (reject route A), order_two_awaits/order_fifo_three (reshaped to TaskGroup)
  # and the two #35 reject pins are now here too. With bug #73 fixed (LlvmGen
  # StoreGlobal now releases the old global value), park_storm's 10000-element
  # global Array<Promise<int>> build no longer exhausts the native heap, so it
  # joins the LLVM lane too. Only recursion_depth (interpreter stack-sizing)
  # stays excluded — see the corpus_tasks_treewalk/_ir comment.
  add_test(NAME corpus_tasks_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/tasks_llvm"
                   ${LANG_LVRT_SOURCES})
  # Track 10 M3b: the flatten/rebuild round-trip on the LLVM C engine, byte-
  # identical to the interpreters (corpus_threads_transfer_{treewalk,ir}).
  add_test(NAME corpus_threads_transfer_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/threads_transfer"
                   ${LANG_LVRT_SOURCES})
  # Track 10 M3c: true OS threads (lv_thread.c). spawn/join on real pthreads with
  # per-worker heaps + eventfd join, BYTE-IDENTICAL to the serial oracle for the
  # shared programs (spawn_join, reject_rethrow), plus the LLVM-only blocked-
  # worker isolation (acceptance #4). Uses pthreads/eventfd — the -lpthread the
  # runner now links is a no-op on glibc >= 2.34 (pthread merged into libc).
  add_test(NAME corpus_threads_native_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/threads_native"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_regex_engine_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/regex_engine"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_regex_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/regex"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_math_transcendental_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/math_transcendental"
                   ${LANG_LVRT_SOURCES})
  # LA-31 Stage 1: LLVM legs of the expr:: prelude surface — byte-identical to
  # the oracle/IR .expected (corpus_expr_{prelude,like,eval}_{treewalk,ir}).
  # The union-in-class-field taxonomy is the tripwire this LLVM lane guards.
  add_test(NAME corpus_expr_prelude_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/expr_prelude"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_expr_like_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/expr_like"
                   ${LANG_LVRT_SOURCES})
  add_test(NAME corpus_expr_eval_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/expr_eval"
                   ${LANG_LVRT_SOURCES})
  # LA-31 Stage 2 — the reifier's LLVM leg (union-array binds + closure + node
  # constructions end-to-end; the layout tripwire the design's §7 pins).
  add_test(NAME corpus_expr_reify_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/expr_reify"
                   ${LANG_LVRT_SOURCES})
  # LA-31 Stage 3 §4 — the differential corpus's LLVM leg.
  add_test(NAME corpus_expr_diff_llvm
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_native_llvm.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/expr_diff"
                   ${LANG_LVRT_SOURCES})
  # http/async/sockets/stale_stream bind fixed loopback ports (8092/8093/8095);
  # serialize the net-binding LLVM lanes with the other engines' full-corpus
  # runs so `ctest -j` never double-binds a port (CMakeLists.txt:149-151).
  set_tests_properties(corpus_llvm corpus_llvm_full
                       PROPERTIES RESOURCE_LOCK corpus_net_ports)
  # A-M5 churn leak net on the LLVM backend (doc §8): live-at-exit must stay flat
  # as N grows. Same corpus as the ELF churn lane; the 3 XFALLs are the inherited
  # dense-append edge plus the two scope-local value-struct programs the A-M6
  # arena tier will reclaim (XFAIL-LLVM markers — GREEN on ELF, see doc-1 §9).
  # Pinned to --no-columnar so this stays the ROW-MAJOR (escape-hatch) leak net
  # after the C-M5 flip; the columnar layout has its own lane below.
  add_test(NAME corpus_churn_leak_llvm
           COMMAND python3 "${CMAKE_SOURCE_DIR}/fuzz/churn_leak.py"
                   --leviathan $<TARGET_FILE:leviathan>
                   --corpus "${CMAKE_SOURCE_DIR}/tests/corpus/churn"
                   --compile-flag=--no-columnar
                   --engine llvm --runtime ${LANG_LVRT_SOURCES})
  # techdesign-columnar-arrays.md plan §3.1: the columnar differential corpus —
  # every program byte-identical across oracle/IR/emit-C++/LLVM AND with
  # --columnar ON and OFF (the staged-flip proof: off == today's behavior).
  add_test(NAME corpus_columnar
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_columnar.sh"
                   $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/columnar"
                   ${LANG_LVRT_SOURCES})
  set_tests_properties(corpus_columnar PROPERTIES TIMEOUT 400)
  # plan §3.2: the columnar churn family under --columnar (leak-flatness). One
  # XFAIL (columnar_index_set) mirrors the inherited dense standalone-operand
  # leak; the three greens prove ColGet is alloc-free, the columnar buffer frees
  # as one blob, and columnar thread transfer stays flat.
  add_test(NAME corpus_churn_leak_columnar_llvm
           COMMAND python3 "${CMAKE_SOURCE_DIR}/fuzz/churn_leak.py"
                   --leviathan $<TARGET_FILE:leviathan>
                   --corpus "${CMAKE_SOURCE_DIR}/tests/corpus/churn_columnar"
                   --compile-flag=--columnar
                   --engine llvm --runtime ${LANG_LVRT_SOURCES})
  set_tests_properties(corpus_churn_leak_columnar_llvm PROPERTIES TIMEOUT 200)
  # Track 10 M6 (techdesign-threads-3 §9): the thread twin of the churn-leak net.
  # Sweeps spawn/join, channel, and adversarial-graph churn through the true-
  # thread runtime (lv_thread.c) and asserts live-at-exit flat, reaps == spawns,
  # transfer-outstanding == 0, and fd flatness (a low-RLIMIT re-run). Compiles a
  # handful of programs at two N sizes, so it takes ~40s — generous timeout.
  add_test(NAME thread_leak_llvm
           COMMAND python3 "${CMAKE_SOURCE_DIR}/fuzz/thread_leak.py"
                   --leviathan $<TARGET_FILE:leviathan>
                   --corpus "${CMAKE_SOURCE_DIR}/fuzz/thread_churn"
                   --small 200 --large 2000 --runtime ${LANG_LVRT_SOURCES})
  set_tests_properties(thread_leak_llvm PROPERTIES TIMEOUT 200)
  # LA-30 B2 (doc 06 §10): the cancel-storm twin of the churn-leak net — a
  # fresh TaskGroup + cancelled child per iteration (registry add/remove,
  # closure capture, catch-unwind) must leave live-at-exit flat as N grows.
  # LLVM-only (TaskGroup requires the task scheduler, unavailable on the
  # frozen ELF backend — never gated on an ELF finding, standing rule);
  # its own directory (not tests/corpus/churn/) so the ELF-swept
  # corpus_churn_leak lane never sees it.
  add_test(NAME corpus_churn_leak_tasks
           COMMAND python3 "${CMAKE_SOURCE_DIR}/fuzz/churn_leak.py"
                   --leviathan $<TARGET_FILE:leviathan>
                   --corpus "${CMAKE_SOURCE_DIR}/fuzz/task_churn"
                   --engine llvm --runtime ${LANG_LVRT_SOURCES})
endif()
add_test(NAME corpus_core_ir
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_corpus.sh"
                 $<TARGET_FILE:leviathan> --ir "${CMAKE_SOURCE_DIR}/tests/corpus/core")

# --- Track B: runtime v2 / driver (techdesign-portable-backend-2.md §8) ---
# B-M1: the runtime core is plain C, needs no LLVM, and is tested standalone
# via runtime_selftest — no leviathan/langfront dependency. B-M2 replaces this
# ad hoc executable-from-sources with a proper `liblvrt.a` CMake target that
# --build-native links against; B-M1 only needs the selftest green.
enable_language(C)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(LANG_RT_SANITIZE "Build the lvrt archive and runtime_selftest with -fsanitize=address,undefined" OFF)

# B-M2: the portable runtime archive. --build-native links this + -lm against
# the LLVM-emitted object; runtime_selftest also links it now, proving the
# archive (not just the loose sources) links and passes. B-M3 adds lv_loop.c
# (event loop + net natives) to the same target. B-M4 adds lv_entry.c (the
# runtime-owned main(), doc-2 §2.9) — a passive archive member pulled only
# once Track A's A-M5 emits lv_main() in place of its own main(); until then
# the generated object's / selftest's own main() wins and this stays unpulled.
# LA-2 (techdesign-tls-crypto.md §5.2): exactly ONE TLS provider compiles into
# the archive — lv_tls_openssl.c when OpenSSL was found (top of file), else the
# not-built stub lv_tls_none.c (same select-at-build pattern as the plat floors).
# lvrt.link records the extra flags the native link path must append beside
# liblvrt.a (main.cpp --build-native reads it; empty file => old behavior).
if (OpenSSL_FOUND)
  set(LVRT_TLS_SRC runtime/lv_tls_openssl.c)
  set(LVRT_LINK_FLAGS "-lssl -lcrypto")
else()
  set(LVRT_TLS_SRC runtime/lv_tls_none.c)
  set(LVRT_LINK_FLAGS "")
endif()

# LA-30 (designs/suspension/techdesign-01-task-substrate.md §12): the stackful
# task substrate compiles into both the runtime archive and the compiler (the
# oracle/IR legs host interpreter recursion on the same fibers, doc 3). The .S
# context switches self-guard by arch (each compiles empty elsewhere), so both
# are listed unconditionally. LANG_CTX_UCONTEXT selects the portable
# makecontext/swapcontext fallback instead (G1: a sigprocmask syscall per switch
# on glibc — bring-up/porting path only, never the shipping default).
enable_language(ASM)
option(LANG_CTX_UCONTEXT "lv_task: use the portable ucontext switch fallback (slow; porting only)" OFF)
set(LV_TASK_SRCS
  runtime/lv_task.c
  runtime/lv_ctx_x86_64.S
  runtime/lv_ctx_aarch64.S)
add_library(lvrt STATIC
  runtime/lv_runtime.c
  runtime/lv_plat_posix.c
  runtime/lv_loop.c
  runtime/lv_thread.c
  runtime/lv_proc.c
  runtime/lv_pty.c
  runtime/lv_entry.c
  ${LV_TASK_SRCS}
  ${LVRT_TLS_SRC})
target_include_directories(lvrt PUBLIC runtime)
# The compiler binary hosts the interpreter-leg fibers on the same substrate (§12).
target_sources(langfront PRIVATE ${LV_TASK_SRCS})
target_include_directories(langfront PRIVATE runtime)
# LA-30 doc 3 §5 (G9): interpreter frames are fat — one eval step is several
# C++ frames with Value/shared_ptr locals. Measured on the depth probe
# (tasks/recursion_depth.lev, hwm_committed at depth 1000): ~5.9 KiB per
# Leviathan frame on the oracle, ~1.4 KiB on the IR engine — so 10k frames
# need ~59 MiB. The fibers hosting Eval/IrInterp recursion therefore default
# to 128 MiB VIRTUAL (≈2× the oracle's 10k-frame need), not the compiled
# leg's 1 MiB: lazy commit keeps RSS at touched pages, the guard page keeps
# overflow loud, and LANG_TASK_STACK still overrides at runtime. (The pump's
# ceiling was the one ~8 MiB OS stack shared by ALL nested dispatch — deep
# recursion in a callback segfaulted there too; tasks make it a per-task,
# sized, diagnosable budget.)
target_compile_definitions(langfront PRIVATE "LV_TASK_STACK_DEFAULT=((size_t)128u<<20)")
if (LANG_CTX_UCONTEXT)
  target_compile_definitions(lvrt PRIVATE LV_CTX_UCONTEXT)
  target_compile_definitions(langfront PRIVATE LV_CTX_UCONTEXT)
endif()
target_compile_options(lvrt PRIVATE -Wall -Wextra)
target_link_libraries(lvrt PUBLIC m pthread)   # fmod (lvrt_arith float %); Track 10 workers
if (OpenSSL_FOUND)
  target_link_libraries(lvrt PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
# Written beside liblvrt.a in the build dir; the native link path reads it.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/lvrt.link" CONTENT "${LVRT_LINK_FLAGS}\n")
if (LANG_RT_SANITIZE)
  target_compile_options(lvrt PRIVATE -fsanitize=address,undefined -g)
  target_link_options(lvrt PRIVATE -fsanitize=address,undefined)
endif()

add_executable(runtime_selftest runtime/selftest.c)
target_include_directories(runtime_selftest PRIVATE runtime)
target_compile_options(runtime_selftest PRIVATE -Wall -Wextra)
target_link_libraries(runtime_selftest PRIVATE lvrt)
if (LANG_CTX_UCONTEXT)
  # the §9 #2 switch-rate gate exempts the ucontext fallback (G1: syscall/switch)
  target_compile_definitions(runtime_selftest PRIVATE LV_CTX_UCONTEXT)
endif()
if (LANG_RT_SANITIZE)
  target_compile_options(runtime_selftest PRIVATE -fsanitize=address,undefined -g)
  target_link_options(runtime_selftest PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME runtime_selftest COMMAND runtime_selftest)

# LA-30 §10/§12: runtime_selftest under valgrind proves the task substrate's
# VALGRIND_STACK_REGISTER annotations — without them valgrind floods with
# "invalid write below stack" on the first fiber switch. Feature-detected like
# the qemu/wine lanes so hosts without valgrind stay green (and NOT under a
# sanitizer build — ASan and valgrind are mutually exclusive). --error-exitcode=1
# fails on any memcheck error; --leak-check=no because the substrate pools task
# stacks for the thread's lifetime by design (doc 2 §7 owns teardown). The
# selftest self-skips its fork/deliberate-fault task tests under RUNNING_ON_VALGRIND.
find_program(LVRT_VALGRIND NAMES valgrind)
if (LVRT_VALGRIND AND NOT LANG_RT_SANITIZE)
  add_test(NAME runtime_selftest_valgrind
           COMMAND ${LVRT_VALGRIND} --error-exitcode=1 --leak-check=no
                   --track-origins=yes $<TARGET_FILE:runtime_selftest>)
  set_tests_properties(runtime_selftest_valgrind PROPERTIES TIMEOUT 300)
  message(STATUS "LA-30: runtime_selftest valgrind lane ENABLED (${LVRT_VALGRIND})")
else()
  message(STATUS "LA-30: runtime_selftest valgrind lane SKIPPED "
                 "(valgrind not found, or sanitizer build)")
endif()

# B-M3: keep the platform-floor audit in CI (doc-2 §5: "stays in CI as a
# script if convenient") instead of a one-off manual grep.
add_test(NAME rt_platform_audit
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_rt_platform_audit.sh"
                 "${CMAKE_SOURCE_DIR}")

# Track W (techdesign-02-backend-column.md §7): the wasm corpus lane.
# gated_*.lev files are NEGATIVE pins for the hard-03 capability-gate
# diagnostic (compile must fail with the pinned text — needs only the LLVM
# wasm backend, no archive). Non-gated files run end-to-end under a headless
# Node host (tests/wasm_node_run.mjs) and are skipped inside the script until
# the §6 wasm runtime archive, wasm-ld, or node are missing.
#
# The pure-compute + console cluster below is the same curated subset the
# W-M0 spike differential already validated end-to-end (tests/spike-wasm/
# run.sh's default_corpus) — house rule (doc 00 §6): portable pins "simply
# gain the wasm lane" rather than being duplicated under tests/corpus/wasm/.
# time_random is doc 03 §5's time/random cluster (shape pins, not golden
# values — the only corpus file whose output crosses the floor's
# now_ms/now_ns/random imports).
set(LVRT_WASM_PURE_COMPUTE_CORPUS
    math structs structs_array generics collections loops match match_nested
    classes oo floats literals const sieve strcmp bitops optional exceptions
    cow maps_set namespaces qualified use seq iterator readonly
    class_dispatch method_refs named_defaults time_random
    # Track W W-M2 (techdesign-04-async-jspi.md §6): the async cluster —
    # await/then-chains/timers/interleaving/throw-across-await through the
    # JSPI lane, diffed against --ir. The LA-30 doc-5 pins reused verbatim:
    # C1 completion order (order_two_awaits), C2 failure routing
    # (throw_uncaught_callback), C3 loud drain (drained_await_throws), C4
    # ready-no-yield, plus the B2 cancellation set. The tasks files that use
    # std::spawn/Channel stay off this lane (gated on wasm, doc 04 §7).
    tasks/drained_await_throws tasks/throw_uncaught_callback
    tasks/ready_no_yield tasks/cancelled_exception tasks/order_two_awaits
    tasks/b2_group_cancel_timeout tasks/b2_group_exit_edges
    tasks/b2_nested_group_shield)
set(LVRT_WASM_CORPUS_FILES "")
foreach(name ${LVRT_WASM_PURE_COMPUTE_CORPUS})
  foreach(ext ext lev)
    if (EXISTS "${CMAKE_SOURCE_DIR}/tests/corpus/${name}.${ext}")
      list(APPEND LVRT_WASM_CORPUS_FILES "${CMAKE_SOURCE_DIR}/tests/corpus/${name}.${ext}")
    endif()
  endforeach()
endforeach()
add_test(NAME corpus_wasm
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_wasm.sh"
                 $<TARGET_FILE:leviathan> "${CMAKE_SOURCE_DIR}/tests/corpus/wasm"
                 ${LVRT_WASM_CORPUS_FILES})

# B-M4: aarch64-linux cross corpus lane under qemu-user (doc-2 §6 item 4).
# Feature-detected: it materializes ONLY when the full cross chain is present
# — qemu-aarch64 to run the binaries, and a cross C compiler (clang, which
# retargets, or aarch64-linux-gnu-gcc) to build the per-triple runtime archive
# via runtime/build-triple.sh. Absent any piece, the lane is skipped so the
# suite stays green on hosts with no cross environment. run_qemu_cross.sh
# cross-builds the corpus with `leviathan --build-native --target` and diffs qemu
# output against .expected. Set LVRT_SYSROOT in the test env if the target's
# loader/libs are not on qemu's default search path.
find_program(LVRT_QEMU_AARCH64 NAMES qemu-aarch64 qemu-aarch64-static)
find_program(LVRT_CROSS_AARCH64 NAMES clang aarch64-linux-gnu-gcc)
if (LVRT_QEMU_AARCH64 AND LVRT_CROSS_AARCH64)
  add_test(NAME corpus_core_aarch64_qemu
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_qemu_cross.sh"
                   $<TARGET_FILE:leviathan> "${LVRT_QEMU_AARCH64}"
                   "aarch64-linux-gnu" "${CMAKE_SOURCE_DIR}/tests/corpus/core")
  # The Debian/Ubuntu aarch64-linux-gnu-gcc cross package bakes its sysroot
  # into the compiler driver (headers/libs resolve at compile+link time with
  # no --sysroot flag needed), but qemu-user does NOT consult gcc's specs —
  # it needs an explicit -L to find /lib/ld-linux-aarch64.so.1 and libc at
  # their real location under /usr/aarch64-linux-gnu. Only relevant to the
  # gcc cross path; clang's -target build already threads LVRT_SYSROOT itself
  # (build-triple.sh) when the caller sets it.
  if (EXISTS "/usr/aarch64-linux-gnu" AND NOT LVRT_CROSS_AARCH64 MATCHES "clang")
    set_tests_properties(corpus_core_aarch64_qemu PROPERTIES
                          ENVIRONMENT "LVRT_SYSROOT=/usr/aarch64-linux-gnu")
  endif()
  message(STATUS "B-M4: aarch64 qemu-user corpus lane ENABLED "
                 "(qemu=${LVRT_QEMU_AARCH64}, cc=${LVRT_CROSS_AARCH64})")
else()
  message(STATUS "B-M4: aarch64 qemu-user corpus lane SKIPPED "
                 "(need qemu-aarch64 + a cross C compiler; not found)")
endif()

# B-M5: Windows cross corpus lane under wine (doc-2 §7's two deferred driver
# items — `-lws2_32`/.exe output in src/driver/main.cpp, and this lane). Feature-
# detected on `wine` (to run the .exe) and the MinGW-w64 cross toolchain (to
# build the per-triple runtime archive via runtime/build-triple.sh); absent
# either, the lane is skipped so the suite stays green on hosts without a
# Windows cross environment. Scope is core + objects subsets only (doc-2 §7's
# acceptance) — net-corpus programs (timers/sockets/http/async) are excluded
# per the milestone's own instruction (they'd need the RESOURCE_LOCK
# corpus_net_ports group this lane does not carry).
find_program(LVRT_WINE NAMES wine)
find_program(LVRT_CROSS_MINGW64 NAMES x86_64-w64-mingw32-gcc)
if (LVRT_WINE AND LVRT_CROSS_MINGW64)
  add_test(NAME corpus_win_wine
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_wine_cross.sh"
                   $<TARGET_FILE:leviathan> "${LVRT_WINE}" "x86_64-pc-windows-gnu"
                   "${CMAKE_SOURCE_DIR}/tests/corpus/core"
                   "${CMAKE_SOURCE_DIR}/tests/corpus/llvm_objects")
  message(STATUS "B-M5: Windows wine corpus lane ENABLED "
                 "(wine=${LVRT_WINE}, cc=${LVRT_CROSS_MINGW64})")
else()
  message(STATUS "B-M5: Windows wine corpus lane SKIPPED "
                 "(need wine + a MinGW-w64 cross compiler; not found)")
endif()

# G-PTY3 (designs/pty/ 03 §6): the Windows ConPTY behavioral lane. NEVER byte
# goldens — ConPTY re-renders, so the asserts are post-VT-strip content, API
# result codes, and "it finished at all". The script self-skips in tiers: the
# MinGW floor compile is the CI-enforced part (the B-M5 standard), the quoting
# table (W4) and the pre-1809 degrade need only a runner, and the content
# cases need a live ConPTY bridge — wine spawns and reaps but does not route
# the child's output through the pseudoconsole pipes, so they skip LOUDLY
# there (risk W5) instead of failing or hanging.
if (LVRT_CROSS_MINGW64)
  add_test(NAME pty_win_conpty
           COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run_pty_win.sh"
                   $<TARGET_FILE:leviathan> "x86_64-pc-windows-gnu")
  set_tests_properties(pty_win_conpty PROPERTIES TIMEOUT 900)
  message(STATUS "G-PTY3: Windows ConPTY lane ENABLED (cc=${LVRT_CROSS_MINGW64})")
else()
  message(STATUS "G-PTY3: Windows ConPTY lane SKIPPED "
                 "(need a MinGW-w64 cross compiler; not found)")
endif()

# ---------------------------------------------------------------------------
# refactor_1 session 07 (techdesign-07): backend-agnostic conformance corpus.
# One ctest test per (leg x program) over tests/conformance/<area>/<name>.lev,
# on three legs: interp (--run), cgen (--build), llvm (--build-native).
# There is NO x64/ELF leg — ever. Per-leg skips live in
# tests/conformance/skip.<leg>.txt (reason REQUIRED per line); run.sh prints
# the reason and exits 77, which ctest reports as Skipped — visible, never
# silent. LEVIATHAN_BIN points run.sh at this build's compiler.
file(GLOB_RECURSE CONFORMANCE_PROGRAMS
     "${CMAKE_SOURCE_DIR}/tests/conformance/*.lev")
list(SORT CONFORMANCE_PROGRAMS)
foreach(prog ${CONFORMANCE_PROGRAMS})
  file(RELATIVE_PATH prog_rel "${CMAKE_SOURCE_DIR}/tests/conformance" "${prog}")
  string(REPLACE ".lev" "" tname "${prog_rel}")
  string(REPLACE "/" "_" tname "${tname}")
  foreach(leg interp cgen llvm)
    add_test(NAME "conf_${leg}_${tname}"
             COMMAND bash "${CMAKE_SOURCE_DIR}/tests/conformance/run.sh"
                     "${leg}" "${prog}")
    set_tests_properties("conf_${leg}_${tname}" PROPERTIES
                         SKIP_RETURN_CODE 77
                         TIMEOUT 300
                         ENVIRONMENT "LEVIATHAN_BIN=$<TARGET_FILE:leviathan>")
  endforeach()
endforeach()

# techdesign-07 Part A completeness gate: every enumerator of `enum class Op`
# in src/ir/Ir.hpp must appear as an exact-name heading in docs/ir-spec.md.
add_test(NAME ir_spec_complete
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/conformance/check_ir_spec.sh")
