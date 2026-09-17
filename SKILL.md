---
name: zvmem
description: Persistent memory store for coding agents. Use it at the start of a session to recall relevant project knowledge, and save non-obvious facts, decisions, and current state immediately as you learn them — without delay — so nothing is lost when auto-compaction happens. Memories survive across sessions; your context does not.
---

# zvmem — persistent memory for coding agents

`zvmem` is a CLI that stores memories in a local vector database (dense + sparse
embeddings + full-text search). Your conversation context fills up, gets
compacted, and is lost — anything you want to remember **must** be written here.
Use it as often as needed; one call is far cheaper than re-deriving knowledge later.

## Setup

`zvmem` must be installed and on your PATH (verify with `which zvmem`).

Every command needs two things:

- **An embedding service** running somewhere you can reach over HTTP, serving
  bge-m3 embeddings and bge-reranker-v2-m3 re-ranking. Point at it:
  ```bash
  export ZVMEM_EMBED_URL="http://<your-embedding-host>:8080"
  curl -s "$ZVMEM_EMBED_URL/health"   # → {"status":"ok", ...}
  ```
  Required for `add`, `update`, and all search modes except `fts`.
- **A memory location**: one collection per project, at `.zvmem` in the project root.
  Pass it as `--path .zvmem` on every command (or export `ZVMEM_PATH=.zvmem`).
  Add `.zvmem/` to the project's `.gitignore`.

First use in a project:

```bash
zvmem --path .zvmem init
```

Running `init` again on an existing collection is harmless: it changes nothing and
exits 6. If you see that, the collection already exists — just continue (it is not
an error to fix). To start a project's memory over from scratch, delete `.zvmem/`
first; `init` will never do that for you.

All examples below assume you are in the project root and include `--path .zvmem`.
Global flags (`--path`, `--embed-url`, ...) may appear before or after the command.

## Workflow — when to call it

**Core rule: save without delay.** Auto-compaction can happen at any time without
warning, and there is no reliable "about to compact" signal to flush state in
time. Anything not yet written to zvmem may be lost at any moment — so treat every
non-obvious fact as worth saving the moment you learn it.

1. **Session start / before starting a task.** Recall first, then work:
   ```bash
   zvmem --path .zvmem search --query "<what you're about to do>" -k 5
   ```
   Read the summaries; pull full content only for hits that matter
   (`get <id>`, or add `--content` to the search).

2. **Save as you learn — immediately, not batched "for later".** The moment you
   discover something non-obvious (a decision and *why*, a gotcha, an environment
   quirk, a workaround), write it down right away:
   ```bash
   zvmem --path .zvmem add \
     --summary "1-2 paragraph summary written to be found by search" \
     --content "full raw payload: code, logs, exact commands, rationale" \
     --tags postmortem,concurrency
   ```
   Test: if you'd want to know this ten minutes from now — or in your next session —
   save it now.

3. **Keep `session-state` continuously fresh.** Compaction can strike between any
   two steps, so don't wait for a compaction warning. Maintain one stable memory
   holding the current state (decisions made, work done, open questions, next
   steps) and `update` it after every meaningful step:
   ```bash
   zvmem --path .zvmem add --id session-state \
     --summary "Session state: what was done, what is pending" \
     --content "- decided X because Y\n- open question Z\n- next: ..."
   # after each meaningful step:
   zvmem --path .zvmem update session-state \
     --summary "Session state (updated): ..." --content "..."
   ```
   After compaction — or at the start of any new session — re-read it to resume:
   `zvmem --path .zvmem get session-state`.

4. **When information changes, `update` — don't add near-duplicates.** Stale
   memories poison search results. If a memory is wrong or obsolete, `update` it
   (re-embeds automatically) or `delete` it.

If your agent supports lifecycle hooks, wire them to this workflow: run step 1 at
session start and re-read `session-state` after compaction. Hooks reduce the risk
of loss — they don't replace saving as you go.

## Writing good memories

- **summary** (1–2 paragraphs): this is what gets *embedded* and matched
  semantically. Make it self-contained: name the entities, the problem, and the
  outcome in plain language. A future you searching "why did the worker pool hang"
  must find a summary that says exactly that — even if your query uses different words.
- **content** (any size): raw payload — code snippets, stack traces, exact commands,
  full rationale. It is *full-text indexed only*, not embedded: it's found by keyword
  overlap (fts/hybrid), not by meaning. Put searchable key terms in the summary too.
- **tags**: short lowercase labels for filtering (`postmortem`, `db`, `ci`).
- **--id SLUG**: give stable, meaningful ids to memories you'll reference or update
  later (`session-state`, `auth-redesign`). Without it, a ULID is generated — fine
  for one-shot facts.

## Commands

Output: one JSON document on stdout; terse errors on stderr. Run `zvmem --help`
for the full reference.

```bash
# Create the collection (once per project). bge-m3 profile: dim 1024, cosine metric.
zvmem --path .zvmem init

# Add a memory → prints {"id": "...", "created_at": ...}
zvmem --path .zvmem add --summary TEXT --content TEXT [--tags a,b,c] [--id SLUG]

# Search (default mode: hybrid = dense+sparse+FTS fused)
zvmem --path .zvmem search --query TEXT [-k N] \
  [--mode hybrid|dense|sparse|fts] [--filter EXPR] [--content] [--rerank] [--candidates N]

# Fetch full docs by id (one or more) → {"docs": [...], "not_found": [...]}
zvmem --path .zvmem get ID [ID...]

# Replace a memory's summary/content/tags; re-embeds, keeps created_at
zvmem --path .zvmem update ID --summary TEXT [--content TEXT] [--tags a,b,c]

# Delete by id (one or more) → {"deleted": [...], "not_found": [...]}
zvmem --path .zvmem delete ID [ID...]

# Full scan, newest first (no embedding service needed)
zvmem --path .zvmem list [--limit N] [--content]

# Collection stats: doc count, index completeness, schema
zvmem --path .zvmem stats
```

### Choosing a search mode

| Mode | Use when | Needs embedding service |
|---|---|---|
| `hybrid` (default) | General purpose — best recall overall. | yes |
| `dense` | Paraphrased/semantic queries ("why did it hang" ↔ "deadlock"). | yes |
| `fts` | Exact terms, identifiers, error strings that live in content. **Works offline.** | no |
| `sparse` | Lexical overlap with summaries (shared technical vocabulary). | yes |

- Add `--rerank` when precision matters: retrieves extra candidates (`--candidates N`,
  default 20) and re-scores them with a cross-encoder. Results carry both the original
  `score` and a `rerank_score`; ordering follows `rerank_score`. Costs one extra service call.
- Add `--content` to include full content in results (default is summary-only, to keep output small).

### Filters (`--filter EXPR`)

SQL-like expressions, applied after ranking:

```bash
# tags is an array — use contain_any(...) with one or more values (= does not work on arrays):
zvmem --path .zvmem search --query "locks" \
  --filter "tags contain_any('postmortem','concurrency')"

# numeric comparisons on timestamps (epoch millis):
zvmem --path .zvmem search --query "migration" --filter "created_at > 1789618000000"

# combine with and:
--filter "tags contain_any('db') and created_at > 1789618000000"
```

### Exit codes and how to react

| rc | meaning | do this |
|---|---|---|
| 0 | ok | — |
| 1 | internal error | retry once; if it persists, report the stderr message |
| 2 | usage error | fix the command (see `--help`) |
| 3 | write lock busy (another writer active) | wait a few seconds and retry |
| 4 | embedding service error | check `$ZVMEM_EMBED_URL/health`; fall back to `--mode fts` if it's down |
| 5 | id not found | the memory was deleted or the id is stale — re-`search`/`list` for a current one |
| 6 | collection/schema mismatch | wrong `--path`, collection made with a different model profile, or re-running `init` on an existing collection (harmless — continue) |

## Example session flow

```bash
# 1. Start of session: recall context for the task at hand
zvmem --path .zvmem search --query "worker pool deadlock fix" -k 5

# 2. Mid-session: learned something non-obvious → save it now
zvmem --path .zvmem add \
  --summary "Queue acquisition must use a timeout; the global lock ordering alone did not prevent the worker pool deadlock because metrics collection re-enters the queue lock." \
  --content "<full postmortem text, code diff, reproduction steps>" \
  --tags postmortem,concurrency

# 3. Made meaningful progress → keep session-state fresh (compaction can hit any time)
zvmem --path .zvmem update session-state \
  --summary "Session state: deadlock fix implemented and verified; rollback plan pending review." \
  --content "- done: lock ordering + timeout\n- pending: rollback review, load test"

# 4. After compaction / next session: resume from the flushed state
zvmem --path .zvmem get session-state
```

## Operational notes

- Memories are stored **in plaintext** in `.zvmem/` on this machine — never store
  secrets, tokens, or credentials as memories.
- One writer at a time (advisory lock; writers wait up to `--lock-timeout`, default
  10 s → exit 3). Readers (`search`, `get`, `list`) never block and work while a
  write is in progress.
- `fts` search, `get`, `list`, `stats` need no embedding service — they still work if
  the embedding server is down.
- If you ever re-init or change the embedding model, old vectors become incompatible
  (exit 6) — plan to re-add memories after a model migration.
