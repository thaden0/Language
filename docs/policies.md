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