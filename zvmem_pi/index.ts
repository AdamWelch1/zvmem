import type { ExtensionAPI, ExtensionContext } from "@earendil-works/pi-coding-agent";

/**
 * zvmem_pi — runs the `zvmem` skill at key session lifecycle points, exactly as
 * if the user had typed `/skill:zvmem <scenario>` themselves.
 *
 *   1. New session start          → init memory (if missing), read session state,
 *                                   recall relevant memories for that state
 *   2. Resuming an existing session → read session state from memory
 *   3. After compaction           → re-read session state + recall relevant memories
 *
 * The skill itself must be installed where pi discovers it (e.g. a SKILL.md in
 * ~/.pi/agent/skills/, or .pi/skills/). This extension only triggers the skill;
 * all zvmem logic lives in the skill's instructions.
 */

const SKILL = "zvmem";

// Scenario prompts appended to `/skill:zvmem`. The skill content is expanded
// inline into the user message and these arguments tell the agent which
// workflow step to execute (SKILL.md has no per-scenario sections).
const SCENARIOS = {
  newSession:
    "Starting a NEW session. Initialize the memory store if it does not exist yet (init), check memory for the current session state (get session-state), and retrieve relevant memories based on that state. Once done, report to the user - do not start any coding work.",
  resume:
    "Resuming an EXISTING session. Read the session state from memory (get session-state) and report back to the user - do not start any coding work.",
  afterCompaction:
    "Context was just COMPACTED. Re-read the session state from memory (get session-state) and retrieve relevant memories based on the current session state. If you were in the middle of a task when compaction occured, resume working. Otherwise, report to the user and wait for instructions.",
} as const;

type Scenario = keyof typeof SCENARIOS;

function errMsg(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

/** True when the zvmem skill is loaded, i.e. `/skill:zvmem` will expand. */
function skillLoaded(pi: ExtensionAPI): boolean {
  return pi.getCommands().some((c) => c.source === "skill" && c.name === `skill:${SKILL}`);
}

/** Send `/skill:zvmem <scenario>` as a user message, as if the user typed it. */
async function runSkill(pi: ExtensionAPI, ctx: ExtensionContext, scenario: Scenario): Promise<void> {
  const text = `/skill:${SKILL} ${SCENARIOS[scenario]}`;
  try {
    if (ctx.isIdle()) {
      // Idle: sends immediately and triggers a turn.
      await pi.sendUserMessage(text, { expandPromptTemplates: true });
    } else {
      // Agent is busy: queue it to run as its own turn once things settle.
      await pi.sendUserMessage(text, { expandPromptTemplates: true, deliverAs: "followUp" });
    }
  } catch (err) {
    if (ctx.hasUI) ctx.ui.notify(`zvmem_pi: failed to run /skill:${SKILL}: ${errMsg(err)}`, "error");
  }
}

export default function (pi: ExtensionAPI) {
  // --- Scenarios 1 & 2: new session vs. resume ------------------------------
  pi.on("session_start", async (event, ctx) => {
    // Only drive interactive sessions; scripted print/json/rpc runs submit their
    // own prompts and would collide with an auto-triggered turn.
    if (ctx.mode !== "tui") return;
    // Same session continuing after /reload — nothing changed in memory terms.
    if (event.reason === "reload") return;

    let scenario: Scenario;
    if (event.reason === "new") {
      scenario = "newSession";
    } else if (event.reason === "resume" || event.reason === "fork") {
      // /resume, /fork, /clone all reopen an existing conversation.
      scenario = "resume";
    } else {
      // reason === "startup": plain `pi` opens a fresh session, while
      // `pi -c`, `--session <path>`, and `-r` reopen an existing one —
      // distinguishable by whether the branch already has messages.
      const hasHistory = ctx.sessionManager.getBranch().some((e) => e.type === "message");
      scenario = hasHistory ? "resume" : "newSession";
    }

    if (!skillLoaded(pi)) {
      ctx.ui.notify(
        `zvmem_pi: skill "${SKILL}" is not loaded — install its SKILL.md (e.g. ~/.pi/agent/skills/)`,
        "error",
      );
      return;
    }

    // Fire-and-forget so startup isn't blocked while the skill turn runs.
    void runSkill(pi, ctx, scenario);
  });

  // --- Scenario 3: after compaction ------------------------------------------
  pi.on("session_compact", async (_event, ctx) => {
    if (ctx.mode !== "tui") return;

    // Defer to the next macrotask. For manual /compact, prompt() throws while
    // the session's compaction controller is still set — it is cleared right
    // after this event completes. For auto-compaction we are mid-run, so
    // runSkill queues the skill turn as a follow-up that starts once the
    // current turn (or an overflow-retry) settles.
    setTimeout(() => {
      void runSkill(pi, ctx, "afterCompaction");
    }, 0);
  });
}
