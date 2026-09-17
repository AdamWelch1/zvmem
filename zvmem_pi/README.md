# zvmem_pi — pi extension for the zvmem skill

Runs the `zvmem` skill automatically at three lifecycle points, exactly as if you
had typed `/skill:zvmem <scenario>` yourself (the skill content is expanded into
a user message and triggers an agent turn):

| Scenario | pi event | What it tells the skill to do |
|---|---|---|
| New session start | `session_start` (`reason: "new"`, or `"startup"` with no prior messages) | init memory if missing, read `session-state`, recall relevant memories |
| Resuming an existing session | `session_start` (`reason: "resume"/"fork"`, or `"startup"` with prior messages — i.e. `pi -c`, `-r`, `--session`) | read `session-state` from memory |
| After compaction | `session_compact` (manual `/compact`, threshold, or overflow recovery) | re-read `session-state`, recall relevant memories |

Notes:
- Only active in interactive (`tui`) mode; scripted `pi -p` / JSON / RPC runs are
  left alone. Remove the `ctx.mode !== "tui"` guards to change that.
- The after-compaction run is deferred one tick and queued as a follow-up, so it
  never collides with an in-flight turn or an overflow-retry; it starts right
  after things settle.
- If the skill isn't loaded you get a notification instead of a literal
  `/skill:zvmem` message being sent to the model.

## Prerequisites

1. `zvmem` on your PATH (the skill's commands invoke it).
2. The zvmem skill installed where pi discovers it, e.g.:
   ```bash
   mkdir -p ~/.pi/agent/skills/zvmem
   cp SKILL.md ~/.pi/agent/skills/zvmem/SKILL.md
   # (a bare ~/.pi/agent/skills/SKILL.md also works)
   ```

## Enabling the extension

`./zvmem_pi/` is not an auto-discovered location, so enable it one of two ways:

**Option A — project settings** (`.pi/settings.json` in this repo):

```json
{
  "extensions": ["/home/user/code/agentrag/zvmem_pi/index.ts"]
}
```

(Relative paths in `extensions` resolve against `<project>/.pi/`, so
`"../zvmem_pi"` works too.)

**Option B — CLI flag** for quick tests:

```bash
pi -e ./zvmem_pi/index.ts
```

Extensions in auto-discovered locations (`~/.pi/agent/extensions/`, `.pi/extensions/`)
can be hot-reloaded with `/reload`; this one can be re-loaded the same way once
enabled via settings.
