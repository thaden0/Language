# Project Policies

## Post Work

When work is complete:
Stage (Take ownership of found files.),
Commit,
Pull origin master
Push to origin master

In the event of a conflict sonnet models should not resolve code based conflicts. Instead stop work and let the user know their is a conflict that needs to be resolved. Docuemntation can be resolved by all models.

When you are done working with a document, and that document is fully implemented. Move the document to the approprate complete folder.

## Folders

Main design folder
designs/

Complete designs
designs/complete/

Design requests
designs/requests/

Design requests procesed
designs/requests/accepted
designs/requests/rejected

Documents
docs/

Processed research documents
docs/complete

## Tech Designs

When writing a Tech Design, seperate the design into files based on a single session. A design that has 4 or more files in its design should be put in its own folder within the design folder.

Techdesign files should use the following format:

techdesign-{design-name}-{complexity}.md

design-name is the name of the design in snake case.

complexity is based on what the code is doing. 

Any code that works with Machine Language, Assembly, Destination Ownership and ARC in LlvmGen.cpp, Cross-target TLS, or sections of similar complexity should be marked as fable complexity.

Any code designs that work within a Library, incuding the Standard Library, tests, demo applications, frameworks, or if the design can be implemented with out any decision making, mark the design complexity as sonnet.

All other designs should be marked opus complexity.

## Research

Collect all the needed information you would need to know about the code in order to make the tech design for the requested feature. 

You should be confedent on where in the code your changes would need to be to meet all the requirements of the feature requested. But also any features that are expected in a programing language or similar feature like this one. For example, we would not add a RegEx library that only has replace, because that's the only functionality the request needs implemented. 

Any requested features that can not be implemented should mean that you include documentation on what in the code is blocking it, and why that is there. Include code snippets of key parts of the existing code that are important to the design. 

When your done writing the research document, move the request document to designs/requests/accepted

## Requests

Include all the details of the requested feature, and what you are working on that requires the feature. Include what wider things this feature would allow. Include all the requirments for both your minimum implementation as well as your maximum implementation. 

## Design Implementation

After implementing a tech design, move that design file to designs/complete

If your changes update any language functionality, update docs/reference.md to match the new state.

Before pushing code, pull master, and then run the approprate test suite. Such as ctest for for the Langauge, or unit tests for another projected.

All unit tests should be done using Harpoon, found in the harpoon folder with the exception of the Language code.

## Orchestration

Orchestration Agents should not look to do the work themselves, but to create sub-agents to resolve parts of the larger task.

It is important for you to understand the task at hand, so read all the relevant documentation to understand the project and the designs.

Before starting, you will need to decide if you are going to run a single path, or if you will run parallel sub-agents. 
- In the event that you need another environment, create a git worktree environment at ~/code/{your-name-here}
- Do not use one of the existing environments as they are being used by parallel agents.

Use parallel agents when the requirements for more then one design is met at one time, and these changes do not interact in the same functions, ideally not the same files.

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
- When a sub agent finishes it's code, merge it's code into your branch. And your branch into that environment. 
- Assign a new bug to a new sub agent for this environment, ideally different from the bug being worked in paralle.

Bug Hunting Sub-Agent Workflow
- Read any documentation provided on the bug. As well as any other documentation you need to understand the project and the request.
- Reproduce the bug, make sure it's still behaving as it was described. If you can not reproduce it, move it to the file "nonReproducable.md in the root. Then report your results.
- Once the bug is repoduced, find the code that is producing the error. Use approprate techniques depending on the bug in question.
- You should ideally be able to test that this is the relevant code. Don't skip the test when you can. You don't want to redesign systems only to realize the bug was up stream and now it's broke in 2 places.
- Once the errored code is identified first check:

Was the error originally reported as in a library, and so you are a Sonnet model? Write a file "sub-agentreport.md" in the project root with all the details you have on the bug, and your steps to this point. Then report to the orchestrator that the bug is actually in the main language source, and that you are tagging in an Opus agent, and that it should read the document you just wrote.

- Once the bug is found in the code, plan out the new design that patches the original bug, with out introducing another elsewhere. Step through your logic so you know how it works.
- One you have your new design, implement it.
- Once implemented run the approprate tests to ensure the bug is fixed,
- Then finigh be reporting to the orchestration agent the results of the bug fix.

After 5 failed attempts to identify a bug, write a report of your attempts in sub-agentreport.md" and return to the orchestrator and request that the next higher level model be used to resolve this issue.




