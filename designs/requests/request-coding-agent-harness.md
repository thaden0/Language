# Agentic Coordinator Harness — Project Description & Feature Set

## Owner Notes
Project name: Siren
Project goes in /examples/siren

> **Status:** Design-ready specification. Working title only; the product is unnamed. Placeholder used throughout: **the Coordinator**.

---

## 1. Thesis

Most agentic coding tools are **developers**: their primitives are edits and tool calls, and they operate one task at a time against a working tree. The Coordinator sits one level up. Its primitives are **work items** and **agents**, and actual file mutation lives a layer down in a *leaf harness* (Claude Code, Codex, or any other). The Coordinator is a **coordinator/architect**, not a developer. It owns orchestration; the leaf owns execution.

The user does not manually prompt agents turn by turn. The user **designs a workflow** — a graph of agents and deterministic steps with wired success/failure paths — seeds it with a feature request (free-text or a structured form), and **turns it on**. The system runs the work item through its stages unattended, works within the user's LLM subscription limits, pauses when budgets are exhausted, and resumes automatically when capacity returns.

This positioning is not cosmetic. It dictates every primitive below. The single hardest rule the whole design defends: **the Coordinator never reaches through the layering boundary to steer a specific edit.** The moment it does, it has collapsed back into being a developer harness.

---

## 2. The Layering Boundary

Two kinds of state, owned by two different layers, meeting at one narrow contract.

- **Orchestration state** — owned by the Coordinator: the workflow graph, the cursor (where execution currently sits), the failure/throw stack, task queues, accumulated work-item artifacts, comm-bus messages, daemon registry.
- **Execution state** — owned by the leaf harness: the context window, the tool-call loop, sandboxing and permissions, individual edits, retries within a single agent turn.

**The contract across the boundary is exactly three things:** status, artifacts, and completion/failure (as a typed throw). Not individual tool calls. The Coordinator observes what a leaf produced and how it terminated; it does not watch or direct the leaf's internal steps.

**Governance stays in the leaf.** Sandboxing philosophy (kernel-enforced vs. harness-enforced), permission prompts, and network policy are the leaf's concern. The Coordinator does not own them. This is the primary named temptation to violate the boundary and must be resisted.

**MCP is the one sanctioned hole in the wall** (see §9): the leaf *pulls* capability upward through an internal MCP server. The Coordinator never *pushes* into the leaf's execution.

---

## 3. Execution Model — Frames All The Way Down

A run is not a static plan being walked. It is a **call stack being executed.**

- A **stage is a frame.**
- A **synchronous subagent is a nested call** — the parent frame stays on the stack, blocked, awaiting return.
- **Failure propagates like an exception:** a node throws, and the nearest enclosing handler catches. No handler at this level → it unwinds to the parent frame, and so on up. Reaching the top uncaught = escalate-to-human.
- A frame's body **can itself be an entire graph with its own handlers.** There is no separate "subagent type" or "pipeline type" — it is frames within frames, and one executor runs every level. This is what makes the protocol **stackable infinitely** and where its genericity comes from.

**The router is not privileged.** It is simply the LLM call that runs in the outermost frame's body to author that frame's children. A subagent that spawns its own sub-pipeline is running its own local router in its own frame. "Coordinator, not developer" is therefore *recursive* — every level coordinates the level below with the identical protocol; there is no distinguished orchestrator, only the top frame.

### 3.1 Typed Throws

The failure channel carries a **typed payload**, and `catch` dispatches on the type. A `bug-found` throw, a `token-exhausted` throw, and a `leaf-crashed` throw are different exception types. A frame catches some and lets others unwind. This is the same schema discipline as the value channel, applied to the error channel.

The closed set of throwable types is a **design deliverable** (see Open Questions). It splits into two bands:

- **Work-semantic throws** (`research-gap`, `file-missing`, `design-invalid`, `bug-found`, …) — open scoping: any frame may catch these.
- **System-liveness throws** (`token-exhausted`, `human-abort`, `budget-exceeded`, …) — **reserved band**: they unwind past every inner handler straight to their designated frame (scheduler / top), regardless of what handlers are installed below. This buys back the scheduling/abort invariants that a buggy inner handler could otherwise strand.

The split falls where "who is the only frame that can correctly handle this" has a fixed answer (reserved) versus a contextual one (open).

### 3.2 Dynamic Handler Scoping (Inherit / Update)

A catch installed at an inner frame **shadows** the outer one for throws originating below it. The router's failure handler is the default *only because it sits outermost*, not because it is special. When an inner agent installs its own catch, that handler is nearer on the unwind path and intercepts first.

- **Inherit** = install nothing; the outer handler catches.
- **Update** = install a *narrower* handler that pattern-matches the subset of throw types this frame owns, and **re-raise the rest** upward.

Example: the Design frame owns research failures. It installs a handler for `research-gap` / `file-missing` and lets `token-exhausted` keep unwinding to the scheduler and `cannot-proceed` unwind to the router. "Not finding a research file" is Design's problem to solve locally; "the feature cannot be implemented" is the router's. The typed throw is what makes this selective instead of all-or-nothing.

### 3.3 Re-raise vs. Translate

When a frame catches, it has three moves:

1. **Handle and continue** — correct the situation in-frame (e.g., define a corrective research task, run it synchronously, proceed). The throw dies here.
2. **Handle and re-throw the same type** — pass it up unchanged.
3. **Handle and translate** — catch a low-level throw and raise a *different, higher-level* one. Design catches `file-missing`, concludes the feature assumes something false, and throws `design-invalid` upward.

Translation is the real payoff of per-frame handlers: **each layer speaks failures in its own vocabulary.** The router never has to understand `file-missing`; it only ever hears `design-invalid`, because Design translated on the way up. Same discipline as the value channel carrying stage-appropriate schema — the error channel is re-typed at each frame boundary.

### 3.4 Contingency Tiers

Three tiers, ascending in cost:

- **Pre-wired edge** — the failure class was foreseen at authoring time; a wired failure path handles it. No new LLM reasoning.
- **In-node sync subagent** — handled *inside* a frame, never touching the parent plan (e.g., Fable hits a research gap, calls the research agent as a sync tool, blocks, continues).
- **Re-plan** — genuinely novel situation returns control to a router-bearing frame for another authoring pass. The only tier that re-spends a routing LLM call.

Design intent leans toward keeping the re-plan tier **thin or empty**: the router anticipates failure classes and wires them; truly unforeseen situations escalate to human rather than silently re-planning. Whether re-plan exists at all is a policy switch, expressed simply as *whether a high frame installs a handler that re-invokes the router.*

---

## 4. The Node Primitive

There is **one node primitive**: `model-in → model-out`. Every executable thing in a workflow is a node.

- **model-in** — the accumulated work-item state this node consumes (see §5).
- **model-out** — the produced artifact, plus an optional **branch selector** (an enum naming which outgoing edge to take), plus an optional **typed throw**.

Nodes differ by a single flag — **did a leaf LLM run inside this frame** — and by which adapter executes them:

| Node kind | LLM? | Executes via |
|---|---|---|
| **Agent** | yes | a leaf harness (Path A) or a direct model call (Path B) |
| **Gate / Op** | no | deterministic evaluator — grep a diff, check an exit code, zip, search text, branch on a condition |
| — | — | *(agent-vs-gate is orthogonal to the stack mechanics)* |

> *Naming note for the non-LLM node kind:* candidates in the register of **gate** or **op** (a node that transforms or branches without judgment). Held loosely; owner's call.

**Branch selectors unify routing and gating.** A routing decision is *expensive* exactly when its branch field can only be produced by a leaf LLM ("if it's hard"). It is *free* when a gate can compute it structurally ("if the diff touches `/parser`"). The whole cost optimization is **pushing branch selectors down to nodes that can compute them without an LLM.** "If it touches a certain part of the code" is a gate returning a label; "if it's hard" is an agent whose entire output schema is that same enum. Same slot, different cost.

Both kinds emit the same **node result shape** — `model-out` + optional branch selector + optional typed throw — so the runtime reads the result and picks the exit without caring whether Sonnet wrote it or `grep` did.

---

## 5. Work Items & The Flowing Artifact

A **work item** is the unit of work. Its representation **accumulates** as it moves through stages — it does not replace itself each stage.

- The work item is a **bundle** with a **current-stage pointer**: `prompt → research doc → tech design → (unit tests) → implementation report`, with each prior artifact retained. The bug-fix loop and any re-design need the design doc and the research that fed it, not just the latest report.
- The router **dispatches on the typed bundle**: it advances a work item through stages, each stage having an owning agent and an expected output shape.

### 5.1 The Schema Seam (produce-side = consume-side)

**A stage's input form *is* the schema for the payload it receives; a stage's output is written in the next stage's input schema.** Produce-side artifact and consume-side form are one schema seen from two ends. This answers "what slice of the bundle does each stage get" *structurally*: each agent declares the shape it eats, and the Coordinator serves exactly that — lean context without a separate slicing decision. It also gives the router a clean thing to dispatch on: match produced-artifact shape to consuming-agent input shape (the same typed dispatch the user already runs in a JSON-schema orchestration harness).

### 5.2 Two Injection Points (kept distinct)

- **`promptInput`** (`default | form`) — how a work item is *seeded*. `default` = free-text, like most agents. `form` = structured fields with required-field enforcement, so nothing gets missed. Enables a formalized user-story-style intake and makes the system usable by non-devs once a dev has set the form up.
- **Payload carry (e.g., `Read {workitem}`)** — how the accumulated *model* is *carried into* a stage. The Coordinator materializes the current bundle (or its stage-scoped slice) to a path; the leaf harness reads it with its own file tools rather than the Coordinator inlining it. The `prompt` field is a **template with slots** the Coordinator fills at dispatch, e.g. `Read {workitem}` → Coordinator writes the bundle to that path → the agent's leaf reads it. The prompt stays small and stable while the payload varies per item.

Convention example already in use: the research agent knows all research goes in `/research`; downstream agents read from there.

---

## 6. Agents

An agent is a configured node. Core attributes:

```json
{
  "name": "Design Agent",
  "model": "Fable 5",
  "harness": "Claude Code",
  "prompt": "Act as a software engineer and write a detailed tech design. Read {workitem}.",
  "promptInput": "default",
  "tools": [],
  "subagents": [],
  "budgets": {
    "loopBudget": null,
    "taskTokenBudget": null,
    "effort": "high"
  }
}
```

Additional per-agent attributes:

- **Subagent spawn graph** — each agent may open a *subset* of agents as subagents. This is a **capability graph**, and it is **not acyclic**: auto bug-reporting-and-fixing puts impl → research/design → impl cycles in it. Example constraint: a Design Agent may spawn Research agents but **not** Implementation agents.
- **Sync vs. async subagents** — a sync subagent blocks the parent frame (Fable calling Research to fill a gap). An async subagent runs in the background (e.g., a monitor) and communicates over the comm bus (§8).
- **Loop-termination policy** — because the spawn graph has cycles, each agent/frame carries a loop budget: max iterations, escalate-to-human, and/or a per-work-item budget cap, so a pair of agents cannot grind all night on a bug they cannot close. **Budgets live on the frame, not the run** — a runaway research↔design loop is capped at its own scope while the parent run continues.
- **Per-stage model/budget policy** — cost gradient is explicit: wait for the expensive model on a design task; downgrade a lint-fix. Multiple implementation agents are allowed provided the routing condition assigning work to each is **cleanly evaluable** (ideally by a gate, no LLM): "works on a certain part of the code" is clean; "if it's hard" is not, and forces an LLM call.

Example tiering already in use: **Fable/Opus** for design and architectural reasoning; **Sonnet** for implementation of fully-specified work and for unit tests; **Haiku** for lighter tasks. Fable is token-expensive and usage-limited, so it does **not** do its own further research — instead it has access to Research agent tool calls: it defines a research task, Sonnet finds out what is needed, and returns findings synchronously.

---

## 7. The Scheduler

Scheduling is **resource-constrained, not merely priority-ordered.**

- **Priority axis** — task queues deploy in priority order; the user writes work items, not turn-by-turn prompts.
- **Resource axis** — each task needs a specific model/provider budget, and those budgets refill on **wall-clock windows** tied to the user's subscriptions. A P0 task that wants the expensive model can be blocked while that model is walled, even though a cheaper model sits idle.

**Backoff behavior:** when budget is exhausted, the system pauses (parks the cursor on the current node) and resumes automatically when capacity returns. This is the unattended-overnight guarantee.

**Downgrade-vs-wait is per-flow policy**, driven by the reserved `token-exhausted` throw reaching the scheduler frame: wait for the expensive model on a design task; downgrade to a cheaper model on a lint-fix. The scheduler frame is the *only* frame that can correctly handle `token-exhausted` — hence its reserved status (§3.1).

---

## 8. Comm Network & Daemons

- **Agent comm network** — all agents communicate over one messaging bus. Sync subagents return values directly; async subagents (monitors, background workers) post messages.
- **Two lifecycles on one bus:**
  - **Task-scoped agents** — spawn, do a stage, die. Their state lives *inside* the plan/cursor durability model.
  - **Daemons** — background/monitoring agents that **outlive any single work item** and just post messages. They are **project-scoped state living beside the plan, not inside it.** The durability model must hold both; the plan+cursor triple alone does not cover daemons.

---

## 9. LLM Call Paths & Reentry

There are **two call paths with different shapes.** Only one needs a client library.

### 9.1 Path A — Call *through* a harness (the common case)

Hand a leaf harness a task; it owns the loop, tool-calling, context, and intra-turn retries. The Coordinator gets back status + artifacts + a typed throw. On this path the Coordinator is **not an LLM client — it is a process supervisor**: spawn, stream-parse, detect completion/failure, kill-on-budget. No message array, no provider SDK.

### 9.2 Path B — Call a model *directly*

Local models, and gate-adjacent cases wanting a raw completion without spinning up a whole harness. This is the **only** path needing a client. It needs the thin sliver only — a **model-client shim** ("LangChain-Lite" oversells it):

- provider interface with a few implementations (Anthropic API, OpenAI-compatible, local via llama.cpp / Ollama),
- normalized message and tool-call shapes across providers,
- streaming,
- structured-output parsing.

A few hundred lines in Leviathan. The heavy things a framework like LangChain sells — the graph runtime, checkpointing, memory, the agent loop — are **built natively** as the frames/cursor/event-log spine and must **not** be duplicated from a library.

### 9.3 One calling convention: `NodeExecutor`

Both paths, plus gates, emit the **same node result shape**. The interface the executor sees is not "LLM client" — it is `NodeExecutor`, with (at least) three implementations:

- **spawn-harness** (Path A),
- **call-model** (Path B),
- **run-gate** (no LLM).

Agent-vs-gate-vs-local becomes an **adapter choice under one convention.**

### 9.4 Internal MCP — the upward reentry channel

The Coordinator runs **internal MCP servers** that give leaves access to the correct tool calls. This is how coordinator-level capability is injected *down into a Path-A leaf* — and, crucially, how a leaf reaches *back up* to ask the Coordinator to run another frame:

- **Downward:** the Coordinator spawns the harness (supervise).
- **Upward:** the harness calls a Coordinator MCP tool (reenter) — e.g., Fable inside Claude Code calls a research tool, and the Coordinator turns that into a spawned Research frame.

The leaf **pulls**; the Coordinator does not push. This is why MCP stays a *clean* hole in the layering wall.

### 9.5 Local-model scope (a real fork)

MCP is the harness's tool-calling protocol, not the Coordinator's. A Path-B local model does **not** get the internal MCP servers for free. If a local model is allowed to reenter (spawn subagents), the Coordinator must itself run that model's tool-call loop — becoming a mini-harness for it. **Design intent: local models are leaf-only workers** (consistent with tiered routing putting local models on light tasks). This keeps the shim thin. If reentry for local models is ever wanted, that is a scoped decision to build a minimal tool-loop — not a default.

---

## 10. State & Durability

Durability is the trust boundary — the line between a toy and something left running overnight. **One substrate per state layer, bound at a seam the Coordinator owns.**

### 10.1 Execution layer — real git via plumbing

Worktree file state uses **real git objects** (via libgit2 / direct plumbing), **not** shelled-out porcelain:

- The unwind keep-or-discard fork *is* git: **keep** = the checkpoint ref stays and the next attempt branches from it; **discard** = reset the worktree to the frame's entry ref.
- Worktrees already share one object store → parallel implementation agents get cheap branching and automatic content-addressed dedup for free.
- Checkpoint commits go in a **separate ref namespace** (e.g. `refs/checkpoints/…`) so mechanical per-node snapshots never pollute the user's semantic branch history. The user's real commits mean something; the Coordinator's are machine bookkeeping; the two do not share a log.
- Plumbing (not `git commit` on a cadence) avoids per-invocation index-lock / hook-fire / gc overhead at checkpoint-per-node frequency across many worktrees.

**Not rolled-my-own here:** writing a custom file-snapshot layer reinvents content-addressed storage (worse) and then has to reimplement branch/reset to recover the unwind semantics git already provides. The IDE precedent for "simplified VCS" doesn't transfer — IDEs checkpoint *editor/buffer state* (undo stacks, unsaved edits), which isn't file-tree-shaped; the Coordinator checkpoints *committed worktree states between agent runs*, which is precisely git's job.

### 10.2 Orchestration layer — append-only event log

Plan, cursor, throw stack, queues, comm messages are high-frequency structured deltas that git models badly. Use **event-sourcing**: an append-only log (over SQLite). This is where "write your own simplified thing" is correct — but it is a *different substrate for a differently-shaped state*, not simplified git. It pays back **time-travel debugging** for free (replay the log to any point) — the same capability the field treats as table stakes.

### 10.3 The seam (the thing actually owned)

A **checkpoint is a pair:** an orchestration event (cursor at node N, throw stack in state S) tagged with the **git ref** naming the worktree snapshot at that instant. Neither substrate knows about the other; the binding — **event ↔ ref** — is Coordinator code, and it is small. **That binding is the entire checkpoint format:** an event log whose entries carry git refs.

**Resume** = load the event, check out the ref, re-attempt the node the cursor sits on. Crash recovery re-attempts the node under the cursor. Build this **first**, not last.

> Open sub-question: does the throw stack live *in* the log as events, or is it reconstructed by replay? This decides whether resume-after-crash can recover a half-unwound stack. (See Open Questions.)

---

## 11. User Interface

Five sections, IDE-familiar:

- **L (Left panel)** — file lists, project tree, etc. (VS Code-like). Movable to the right; pure UI, low-stakes. Also the natural home for the **global workflow tree** (the "forest") so position isn't lost while zoomed into one node.
- **R (Right panel)** — where AI agents conventionally show.
- **T (Top panel)** — default: the **selected agent.**
- **B (Bottom panel)** — terminal (opposite T).
- **M (Main center)** — splittable into up to **4 columns, M1–M4.** Default: up to 4 **subagents of the selected agent.**

**Navigation is tree-walking:** the default layout shows *selected agent + its children.* Clicking a subagent in (say) M2 makes it the new **T**, and its own subagents repopulate M1–M4. Because that view only ever shows one node's neighborhood, the **full tree** persists in L (or a dedicated map panel) to preserve global position.

**The tree view shows orchestration state.** Drilling into a leaf shows *that leaf's own stream* (its native output) — never the Coordinator reaching into the leaf's tool calls.

**A simple built-in IDE** is included for editing — deliberately *not* competing with VS Code; that is not the product's wheelhouse. The center of gravity is the **Agent section**, where the full tree of agents and subagents is monitored and any node can be interacted with.

---

## 12. Cross-Cutting Mechanisms (confirmed present)

- **Hooks** — leaf-exposed lifecycle hooks (pre/post tool-use, prompt-submit, stop, subagent start/stop) that run in-process **without consuming context.** These are the natural home for **gate nodes**: a post-tool-use hook that greps a diff and emits a branch label is a zero-LLM edge evaluator running for free.
- **Internal MCP servers** — coordinator-served tools giving leaves the correct tool calls; also the upward-reentry channel (§9.4).
- **Per-agent budgets** — loop budgets, single-task token budgets, effort levels — all present, all frame-scoped.

---

## 13. Build Sequencing & Discipline

Field consensus and the design's own shape agree: **start with the simplest thing that works, instrument it fully, add complexity only in response to observed failure modes — not anticipated ones.**

Concretely:

1. **Checkpointing first** — the event-log ↔ git-ref seam (§10). Everything trustworthy depends on it.
2. **One linear chain end-to-end** — `research → design → impl` on a single work item, Path A only, before building any recursion.
3. **The node/`NodeExecutor` convention** — spawn-harness + run-gate, then call-model.
4. **Typed-throw lattice and reserved band** — *specified* now, *proven* late. The throw types and handler-scoping can be designed up front but need not be exercised until the linear chain runs.
5. **Recursion, daemons, local-model Path B** — added after the linear chain is solid.

The protocol is infinitely stackable *by design*; the discipline is not to build the recursion before one linear chain runs clean.

---

## 14. Open Design Questions (the real forks)

Each of these is a genuine fork the design deliberately leaves open for the design phase:

1. **The throw-type lattice** — the *closed set* of things a frame can throw, and precisely which types sit in the reserved (non-catchable-below) band vs. open scoping.
2. **Re-plan tier existence** — does any high frame install a router-reinvoking handler, or is every unforeseen failure an escalate-to-human? (Design leans: thin/empty re-plan.)
3. **Throw stack durability** — stored as events in the log vs. reconstructed by replay; determines half-unwound-stack recovery.
4. **Local-model reentry** — leaf-only workers (thin shim) vs. allowed to spawn subagents (Coordinator becomes a mini-harness for them). (Design leans: leaf-only.)
5. **Downgrade-vs-wait defaults** — per-flow policy table mapping stage → behavior on `token-exhausted`.
6. **Bundle slice granularity** — whole-bundle tempfile vs. schema-scoped slice per stage (design leans: schema-scoped, since produce-side output == consume-side form).
7. **Daemon durability** — how project-scoped daemon state is persisted beside (not inside) the plan/cursor model.
8. **Gate/Op naming** — the label for the non-LLM node kind.
9. **Product name** — the tool is currently unnamed.

---

## 15. Glossary

- **Coordinator** — this product; the meta-harness that orchestrates leaf harnesses.
- **Leaf harness** — Claude Code, Codex, or similar; owns execution state.
- **Frame** — one stage on the execution call stack; may contain a whole sub-graph.
- **Node** — the single executable primitive; `model-in → model-out`. An **agent** (LLM ran) or a **gate/op** (no LLM).
- **Branch selector** — enum field in `model-out` naming the outgoing edge; produced by an LLM (expensive) or a gate (free).
- **Throw** — typed failure payload on the error channel; caught by the nearest enclosing handler, dispatched on type.
- **Reserved band** — throw types (system-liveness) that unwind past inner handlers to a designated frame regardless of installed handlers.
- **Work item** — the unit of work; an accumulating bundle with a current-stage pointer.
- **Schema seam** — the identity between a stage's output schema and the next stage's input form.
- **`promptInput`** — how a work item is seeded (`default` free-text | `form` structured).
- **Payload carry** — how the accumulated bundle is materialized to disk for a leaf to read (`Read {workitem}`).
- **NodeExecutor** — the one calling convention; implementations: spawn-harness (Path A), call-model (Path B), run-gate.
- **Internal MCP** — coordinator-served tools; the sanctioned upward-reentry channel across the layering boundary.
- **Checkpoint** — an orchestration event tagged with a git ref; the unit of durability and resume.
- **Daemon** — a project-scoped, long-lived background agent on the comm bus, outliving any single work item.