# Project Policies

## Post Work

When work is complete:
- Stage (take ownership of found files).
- Commit to your own branch — never commit directly to master.
- Merge origin/master into your branch.
- Run the appropriate test suite (see Testing) and confirm it is green.
- Push your branch.
- Merge your branch into master.
- If master moved since you last pulled it (another agent merged in the meantime), merge the new master into your branch again and re-run the test suite before finishing. Never leave master holding a combination of branches that no one has tested.

In the event of a conflict, Sonnet models should not resolve code-based conflicts. Instead, stop work and let the user know there is a conflict that needs to be resolved. Documentation conflicts can be resolved by all models — but the known-bugs files need extra care (see Bug Numbering).

When you are done working with a document, and that document is fully implemented, move the document to the appropriate complete folder. Fix any references to the moved file so links still resolve.

## Testing

Language changes — anything in the Leviathan compiler/source — must pass the full ctest suite before push.

Anything written in Leviathan itself — libraries (including the standard library), demo applications, frameworks, TUIs — must have Sonar unit tests written for it. Sonar is the unit-test framework (formerly Harpoon; currently in the `harpoon/` folder). The only exception is the Language code itself, which is covered by ctest rather than Sonar.

Before pushing code, pull master, then run the appropriate suite: ctest for the Language, or Sonar unit tests for anything else.

A bug is not closed until its minimized repro is committed to the test corpus as a regression test. The fix and its regression test land in the same commit.

The three backends (oracle, IR, LLVM) must agree on the output of any valid program. Any disagreement between backends is automatically a bug — file it in the known_bugs files even if it is not the thing you were working on. No hand-written expectation is needed to prove it; the disagreement itself is the evidence.

## Review

Before merging an opus- or fable-complexity change into master, have a fresh agent — one that shares no context with the implementer — review the diff. The implementer must not review its own work. Sonnet-complexity changes do not require this pass.

## Gotchas

docs/gotchas.md is the shared landmine list: one line per entry, describing traps that are not derivable from the code (e.g. identifiers that break codegen, stale-build hazards, files that must be edited together).

- Read docs/gotchas.md before starting any implementation or bug-hunting work.
- When you discover a new landmine, add a one-line entry in the same commit as the work that surfaced it.
- Gotchas live in the repo, not in agent memory, so every parallel agent inherits them on the next pull.

## Folders

Main design folder
designs/

Complete designs
designs/complete/

Design requests
designs/requests/

Design requests processed
designs/requests/accepted
designs/requests/rejected

Documents
docs/

Processed research documents
docs/complete

## Tech Designs

When writing a Tech Design, separate the design into files based on a single session. A design that has 4 or more files in its design should be put in its own folder within the design folder.

Techdesign files should use the following format:

techdesign-{design-name}-{complexity}.md

design-name is the name of the design in snake case.

complexity is based on what the code is doing.

Any code that works with Machine Language, Assembly, Destination Ownership and ARC in LlvmGen.cpp, Cross-target TLS, or sections of similar complexity should be marked as fable complexity.

Any code designs that work within a Library, including the Standard Library, tests, demo applications, frameworks, or if the design can be implemented without any decision making, mark the design complexity as sonnet.

All other designs should be marked opus complexity.

## Research

Collect all the needed information you would need to know about the code in order to make the tech design for the requested feature.

You should be confident on where in the code your changes would need to be to meet all the requirements of the feature requested. But also any features that are expected in a programming language or similar feature like this one. For example, we would not add a RegEx library that only has replace, because that's the only functionality the request needs implemented.

Any requested features that cannot be implemented should mean that you include documentation on what in the code is blocking it, and why that is there. Include code snippets of key parts of the existing code that are important to the design.

When you're done writing the research document, move the request document to designs/requests/accepted

## Requests

Include all the details of the requested feature, and what you are working on that requires the feature. Include what wider things this feature would allow. Include all the requirements for both your minimum implementation as well as your maximum implementation.

## Bug Numbering

Parallel agent branches independently assign numbers in the known_bugs files, so the same bug can land under two different numbers on merge, or two different bugs can collide on one number.

To prevent this, prefix every new bug number with your branch name. A bug found on agent1 is `agent1-101`; on agent3 it is `agent3-054`. Each branch keeps its own sequence, so numbers never collide across branches. When branches merge, the prefixes stay unique; the user renumbers or consolidates during review if desired.

## Design Implementation

After implementing a tech design, move that design file to designs/complete. Fix any references to the moved file so links still resolve.

If your changes update any language functionality, update docs/reference.md to match the new state.

Before pushing code, pull master, and then run the appropriate test suite (see Testing).

## Orchestration

Orchestration Agents should not look to do the work themselves, but to create sub-agents to resolve parts of the larger task.

It is important for you to understand the task at hand, so read all the relevant documentation to understand the project and the designs.

Before starting, you will need to decide if you are going to run a single path, or if you will run parallel sub-agents.
- In the event that you need another environment, create a git worktree environment at ~/code/{agent-name}-{task-or-bug-id}. The name must be deterministic and unique so parallel runs never race on the same path.
- Do not use one of the existing environments as they are being used by parallel agents.

Use parallel agents when the requirements for more than one design is met at one time, and these changes do not interact in the same functions, ideally not the same files.

Use the correct model for the design. Designs should have the Sonnet, Opus, Fable designations going forward, but for designs that do not include this rating:
- Changes to the Leviathan source: Opus
- Changes to a Library, including the standard library: Sonnet
- Bug Fixes: Same As Above per Bug
- The user may specify special exceptions at prompt time.


Bug Fixing Workflow
- Keep this branch as your branch.
- Start by creating 2 worktree environments in ~/code/
- Identify two different bugs that are seemingly not related.
- Assign a sub agent with the task of identifying and fixing the bug, for each of the bugs.
- When a sub agent finishes its code, merge its code into your branch, and your branch into that environment.
- Assign a new bug to a new sub agent for this environment, ideally different from the bug being worked in parallel.

Bug Hunting Sub-Agent Workflow
- Read any documentation provided on the bug, as well as any other documentation you need to understand the project and the request. Read docs/gotchas.md.
- Rebuild the compiler from HEAD before running any repro probe. The build output is often stale relative to HEAD because parallel agents commit continuously — a stale binary produces false bug reports.
- Reproduce the bug, make sure it's still behaving as it was described. If you cannot reproduce it, move it to the file "nonReproducable.md" in the root. Then report your results.
- Once the bug is reproduced, find the code that is producing the error. Use appropriate techniques depending on the bug in question.
- You should ideally be able to test that this is the relevant code. Don't skip the test when you can. You don't want to redesign systems only to realize the bug was upstream and now it's broken in 2 places.
- Once the errored code is identified first check:

Was the error originally reported as in a library, and so you are a Sonnet model? Write a file "sub-agentreport.md" in the project root with all the details you have on the bug, and your steps to this point. Then report to the orchestrator that the bug is actually in the main language source, and that you are tagging in an Opus agent, and that it should read the document you just wrote.

- Once the bug is found in the code, plan out the new design that patches the original bug, without introducing another elsewhere. Step through your logic so you know how it works.
- Once you have your new design, implement it.
- Once implemented run the appropriate tests to ensure the bug is fixed, and commit the minimized repro as a regression test alongside the fix (see Testing).
- Then finish by reporting to the orchestration agent the results of the bug fix.

An attempt, for the rule below, is one edit-test cycle: you change code and run the relevant test. After 5 failed attempts to identify a bug, write a report of your attempts in "sub-agentreport.md" and return to the orchestrator and request that the next higher level model be used to resolve this issue.
