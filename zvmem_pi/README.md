# zvmem_pi — pi extension for persistent project memory

Bakes the entire `zvmem` skill into a first-class **`zvmem` tool** and wires
lifecycle recall. Plan + progress: zvmem memory id `zvmem-pi-overhaul`.

## The `zvmem` tool

One unified tool with an action enum over `<project>/.zvmem`:

| Action | Notes |
|---|---|
| `init` | Create the store (idempotent — "already exists" is not an error) |
| `add` | `summary` + `content` required; optional stable `--id` slug and `tags[]` |
| `search` | `query`; `k` (default 10), `mode` hybrid/dense/sparse/fts, `filter`, `rerank`, `candidates`, `includeContent` |
| `get` / `delete` | one or more ids (`id` or `ids`) |
| `update` | exactly one id + new `summary`; optional `content`/`tags` (re-embeds) |
| `list` / `stats` | offline — no embedding service needed |

The tool runs the CLI via `pi.exec` (spawn, no shell), applies SKILL.md's exit-code
policy (rc1 retry once; rc3 wait+retry; rc4 on search auto-falls back to fts once;
rc5 on get is returned as data; rc6 on init is ok), and truncates output at 20k chars.
The tool description + prompt guidelines carry the skill's workflow rules:
search memory before deep-diving into code/docs (and save what you discover),
one focused memory per fact/decision/event, concise `session-state` with pointers
instead of one big catch-all, track information in memory unless the user
explicitly asks for a file, save without delay, no near-duplicates.

## Lifecycle recall

| Scenario | Behavior |
|---|---|
| New session start / resume (`/resume`, `/fork`, or startup with history) | **Asks first**: a confirm dialog ("zvmem: recall memory?"). On yes, recall runs deterministically via the CLI (init if missing → `get session-state` → search with its summary as query) and is injected as a visible custom message **without** triggering an LLM turn — zero cost, becomes context for your first prompt, never starts coding work. |
| After compaction (`/compact`, threshold, overflow recovery) | Automatic (no confirm). Same deterministic recall, injected with resume guidance; delivered via `steer` when mid-run so it lands in the next LLM call (including overflow-retry), appended otherwise. No autonomous turn is triggered. |

## Memory-save reminders

The model often ignores the skill's "save without delay" instruction, so the
extension nudges it — **invisible to the user and never added to session context**.
Reminders are appended to a single LLM request via the `context` event (per-request
transform; nothing is persisted or shown in the transcript):

| Trigger | Condition |
|---|---|
| Per-turn | The previous turn had ≥1 tool result but no successful `zvmem add`/`update` → one nudge on the next LLM call |
| Time-based | ≥10 min since the last save (timer starts at session start) and ≥5 min since the last time-based nudge |

A global 30-second floor applies to **all** reminders, so rapid successive tool
calls never produce back-to-back nudges. Gated to `tui` mode like everything
else. If per-turn nudging ever induces junk memories, throttle it in `index.ts`
(`turn_end` handler / `MIN_NUDGE_GAP_MS`).

Only active in interactive (`tui`) mode; scripted print/json/rpc runs are left
alone. Recall entries are custom messages, so they never pollute `/resume` session
titles.

## Prerequisites

1. `zvmem` on your PATH.
2. `ZVMEM_EMBED_URL` pointing at a running embedding service (full URL with scheme).
   `init`/`get`/`list`/`stats` and fts search work without it; the tool auto-falls
   back to fts for searches when the service is down.

## Installing the extension

**Recommended:** copy `index.ts` into pi's global auto-discovered extensions
directory, in its own subdirectory:

```bash
mkdir -p ~/.pi/agent/extensions/zvmem
cp index.ts ~/.pi/agent/extensions/zvmem/index.ts
```

The extension then loads for every project — it operates on `<cwd>/.zvmem`, so
each project keeps its own memory store. Auto-discovered extensions can be
hot-reloaded with `/reload`.

**Project-local alternative:** the same layout under a project's `.pi/`
directory (`.pi/extensions/zvmem/index.ts`) loads it only for that project,
after the project is trusted. You can also list an explicit path in
`.pi/settings.json` (`"extensions": [...]`; relative paths resolve against
`<project>/.pi/`).

**Quick test without installing:** `pi -e ./zvmem_pi/index.ts`.
