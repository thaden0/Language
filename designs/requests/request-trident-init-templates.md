# Request: `trident init` Project Templates

**From:** Atlantis framework (DX / R3 ruling). **Date:** 2026-07-06.
**Priority:** P2 — wanted by AG-8 (1.0-preview, 2027-02); the copyable
`examples/atlantis-demo/` covers DX until then.

## 1. Context

`designs/proposal-package-manager.md` §8.2 lists `lang init # scaffold project.ext` (owner
pointed Atlantis at this CLI as the sanctioned home for scaffolding — ruling R3), but
`init` was dropped when the CLI was promoted into `designs/complete/techdesign-package-manager.md`
§5.5. Meanwhile Trident's invariants (rightly) forbid the framework from shipping its own
installable CLI: packages are source-only, fetching runs zero dependency code. Result:
there is **no `dotnet new` equivalent anywhere in the toolchain**. This ticket asks for
`init` back, with template support, in a shape that keeps every Trident STOP invariant.

## 2. Requirement

```
trident init                                   # bare: minimal trident.toml + main.lev
trident init --template <local-path | vcs-path[@version]>
trident init --template atlantis               # name form, IF/when the P2.3 name index exists
```

- **R-1. A template is an ordinary Trident package** (fetched by the existing local-path /
  git-tag machinery, content-addressed store, checksummed — no new fetch path) containing a
  `template/` directory subtree.
- **R-2. Materialization is a pure file copy + token substitution.** Copy `template/**`
  into the target directory; substitute `{{name}}` (project name) in file contents and
  file names. **No hooks, no scripts, zero code execution** — the §0.3c STOP invariant is
  preserved by construction; a template is data.
- **R-3. Refuses to overwrite** a non-empty directory without `--force`. Prints the file
  list it created.
- **R-4. The produced project builds**: `trident init --template X && trident build .`
  succeeds (template's `trident.toml` declares its own deps — e.g. the Atlantis template
  depends on `atlantis` + `atlantis-mysql`; `init` runs the ordinary resolve/lock/fetch
  afterward, exactly as `trident add` would).

## 3. What Atlantis does with it

Ships a `template/` in the `atlantis` repo realizing the blessed scaffold (overview §4:
`.env`, `trident.toml`, `main.lev`, `app/controllers`, `app/models/{dtos,entities}`,
`config/`, `middleware/`, `views/`, `public/`, `tests/`). `trident init --template
atlantis && trident run .` = a running secure-by-default web app in two commands — the
"easy setup" bar ASP.NET/Rails set, hit without adding a single new binary to the
toolchain.

## 4. Acceptance

1. `trident init --template ./packages/atlantis-template myapp` produces a directory that
   `trident build` compiles and `trident run` serves.
2. Grep-proof: the init path contains no exec/spawn of fetched content (same class of
   proof as GT1's `grep -rnE 'parseManifest|…' src/` discipline).
3. `{{name}}` substitution covers file content + names; a template with no tokens is a
   plain copy.
4. Trident's existing lock/store behavior is unchanged for non-init commands (corpus
   green).

## 5. Interim fallback (already designed in)

`examples/atlantis-demo/` is the documented copy-me template; docs say "copy the demo,
rename in trident.toml."
