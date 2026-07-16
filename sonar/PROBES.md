# Sonar language probes

Executed 2026-07-12 with the repository's `build/leviathan`. All probes use one
shared `.expected` result across the tree-walk oracle, IR interpreter, emit-C++
native binary, and LLVM native binary.

| probe | oracle | IR | emit-C++ | LLVM | fallback invoked |
|---|---|---|---|---|---|
| P1 inherited field satisfies interface | green | green | green | green | no |
| P2 direct closure-field call | green | green | green | green | no |
| P3 class `is` narrowing | green | green | green | green | no |
| P4 heterogeneous interface dispatch | green | green | green | green | no |
| P5 recursive struct auto-construction | green | green | green | green | no |
| P6 distinct methods + qualification | green | green | green | green | no |
| P7 diamond shares one core | green | green | green | green | no |
| P8 later collapsed method wins | green | green | green | green | no |
| P9 negative enum carrier | green | green | green | green | no |
| P10 labeled struct constructor | green | green | green | green | no |
| P11 diamond field initialization | green | green | green | green | no |

## T11 reactivity — R-1 accessor-injection probes (2026-07-14)

The T11 gate (design §4 M1 / §6): can a Layer-B rule inject a `set`-view onto an
`@`-attributed field, and does a bare write route through it on every engine?

| probe | oracle | IR | emit-C++ | LLVM | fallback invoked |
|---|---|---|---|---|---|
| P12 rule-injected set-view over an attributed field | green | green | green | green | no |
| P13 per-type reactive rules (`where f.type == ...`) | green | green | green | green | field-type hole |
| P14 field initializer writes raw (bypasses the view) | green | green | green | green | no |

**Findings (drive the T11 mechanism).** Accessor injection is GREEN and
byte-identical on all four engines — `set $f(...) { $f = v; ... }` splices the
field identifier in both accessor-selector position and assignment-target
position, and `$f = v` inside the view is raw-slot access (§6, non-recursive:
the ping fires exactly once per write). The initializer writes the raw slot,
so no ping fires before the first explicit write (probe issue #2 resolved:
initial render happens at bind time, not construction).

Two design assumptions in §2's setter template do NOT hold against the landed
rule engine, and are worked around (no compiler change — reference.md §rules
confirms both are absent, they are the filed LA-15/LA-16 asks):

1. **No field-type hole.** `<T>` / `$f.type` is not a reifier (`$f.type`
   parses as `<fieldname>.type`, a runtime member access). Worked around by
   **per-type rules** gated `where f.type == "int" | "string" | "bool" |
   "float"` — one typed `set`-view apiece (P13). Non-scalar reactive fields are
   out of v1 scope (add a rule per type as needed).
2. **No field-name string.** For a *match-bound* field subject, `"$f"` stays
   the literal `"$f"` and `$f.name` reifies to `<fieldname>.name` (only a
   `$for`-bound `meta::Field` *value* stringizes via `.name`). So the injected
   set-view cannot spell `this.__sonarFieldChanged("count")`. **Forced
   deviation:** the view calls a nullary `this.__sonarNotify()` and the
   Component registry does **global fan-out** (every reactive write re-runs
   every binding on that host; a single re-entrancy bool settles chains in one
   pass — §3's cycle-settle intent, observationally identical for all goldens).
   The string-keyed registry is retained for `--expand` legibility (§5 #5) and
   to make per-field precision a one-line change once LA-15's name-stringize
   lands.
