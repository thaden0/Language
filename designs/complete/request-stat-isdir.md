# Summary: Directory-type predicate on the filesystem floor (`sysStat` field 3 = isDir)

Filed 2026-07-15 by the Sonar DOM suite (`designs/sonar/dom/techdesign-07-dialogs.md` §3). The
filesystem floor can enumerate (`sysListDir`) and stat (`sysStat` fields 0=exists, 1=size,
2=mtime) but cannot answer "is this path a directory?" without abusing `sysListDir(path) != None`
— one full directory read per query.

## Request Details

`Sonar::Dialogs::FileDialog` classifies every entry of a listing as file-vs-directory (dirs sort
first, render with a `/` suffix, and activate as navigation). With N entries that is N
`sysListDir` probes per listing — N opendir/readdir/closedir cycles where N one-word stat reads
would do. Locally tolerable, visibly slow on network filesystems; also semantically wrong at the
edges (an unreadable directory lists as None and misclassifies as a file).

## Requested Specific Feature

Extend the existing native — no new symbol: **`sysStat(path, 3)` returns 1 when the path is a
directory, 0 when it exists and is not, -1 when missing** — the `st_mode`/`S_ISDIR` word is already
in the buffer the native fills. Prelude convenience alongside the existing wrappers:
`bool isDir(string path) => sysStat(path, 3) == 1;`. Engine coverage matching the current `sysStat`
lanes (oracle/IR full; the standing coverage-diagnostic story elsewhere).

## Known Warnings

- Symlinks: whatever `sysStat` already does (follow, per stat(2)) is correct here too — document,
  don't special-case. A future `lstat` field is out of scope.
- Field numbering is append-only ABI: 3 must never be reused for anything else.

## Acceptance Criteria

- `isDir` on: an existing directory → true; a file → false; a missing path → false (via -1).
- FileDialog's per-entry classification drops its `sysListDir` probes for one `sysStat` per entry
  (the swap is one helper, `techdesign-07-dialogs.md` §3).
- Corpus rows in the filesystem natives' existing test home; unreadable-directory edge pinned.

## Intrim Fallback

Shipping today in FileDialog: `sysListDir(child) != None` as the classifier, probed lazily with a
logged degradation above 256 entries. Correct, portable, slow — the request is an optimization +
edge-correctness ask, not a blocker.

## Other

Adjacent asks deliberately NOT bundled: `lstat`/symlink surface, batch-stat, file watches (the T08
theming doc already notes watch natives as a separate future). Small, append-only, one native
touched.
