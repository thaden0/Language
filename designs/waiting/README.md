# `designs/waiting/` — designs blocked on a concrete future event

A design lives here when **all three** hold:

1. It is **fully decided** — every choice is locked; it is not an open question or a
   half-design. (Undecided designs are a policy violation, not a waiting item — see
   `feedback_no-deferrals-in-designs`.)
2. It **will** be built — there is no "if", only "when".
3. It is blocked on **one concrete, nameable event** — a landed gate, a shipped
   prerequisite, a filed bug-tag, or a **measured threshold with an actual number**.
   "When we get around to it" / "if it ever regresses" is *not* an event.

## Naming — the event is in the filename

```
<topic>--waits-on-<event>.md
```

One glance answers *what it is*, *what it waits on*, and *that nobody forgot it*.
Examples:

- `trident-post-v1--waits-on-selfhost-g5.md`
- `metaprog-provenance-ids--waits-on-first-ambiguous-diagnostic.md`

## Not allowed here

- **A design with an "Open questions" section.** Decide it first (surface the choices to
  the owner *before* writing), then it either ships or waits — it never waits *undecided*.
- **A "demand-gate" with no number or named gate.** If the trigger is a measurement,
  the threshold goes in the filename and the doc, or it does not belong here. A vague
  demand-gate is buried indecision wearing a schedule's clothes.

## When the event fires

Move the file to `designs/` (now active/schedulable) or straight into implementation;
when it lands, it goes to `designs/complete/` like any other design.
