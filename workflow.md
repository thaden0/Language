# Work Flow
A work in progress file.

## Pre-Design:
- Feature Discussion (Fable or Opus)
- Project State Research (Sonnet)
- Request File (Opus)

## Design 
- Data Collections (Read Request, and related files.) (Sonnet)
- Project State Update Research (Sonnet)

## Design Write (Fable/Opus)
- Write the Design (Fable)

During the design phase, context is initially built up by a sonnet model collecting all our pre-researched data. If after the
switch to Fable to write the design, it is clear that more research is needed, Fable may look in 1 or 2 more locations before writing the design, but if this would require more then 2 additional tool calls, then Fable should stop working, and write a research details plan to a file.

Designs should be highly details and include resolutions for any foreseeable difficulties. Areas dealing with highly complex areas of the code should be marked as sensative content, so that implementing models can stop working and allow a stronger model to make the high risk changes.

A Sonnet (or at the request of the Fable model, an Opus) model will follow the directions in the research plan, and save the research findings to that file. Once research is complete, Fable will read the research file and continue (or set up another research plan if more is needed.)

## Design Implementation