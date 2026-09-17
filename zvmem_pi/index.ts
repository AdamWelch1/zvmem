import { readFile } from "node:fs/promises";
import { dirname } from "node:path";
import type { ExtensionAPI, ExtensionContext } from "@earendil-works/pi-coding-agent";
import { stripFrontmatter } from "@earendil-works/pi-coding-agent";

/**
 * zvmem_pi — runs the `zvmem` skill at key session lifecycle points.
 *
 *   1. New session start          → init memory (if missing), read session state,
 *                                   recall relevant memories for that state
 *   2. Resuming an existing session → read session state from memory
 *   3. After compaction           → re-read session state + recall relevant memories
 *
 * The skill content is injected as a CUSTOM message (not a user message) with
 * triggerTurn: true — the LLM receives exactly what `/skill:zvmem <scenario>`
 * would expand to, and acts on it. Because it is not a user-role entry it never
 * becomes the session's "first message", so /resume shows your real first prompt
 * (or "(no messages)") instead of the skill block. display: false keeps the chat
 * transcript clean too; a short notify gives feedback that the run started.
 *
 * The skill itself must be installed where pi discovers it (e.g. a SKILL.md in
 * ~/.pi/agent/skills/, or .pi/skills/). This extension only triggers the skill;
 * all zvmem logic lives in the skill's instructions.
 */

const SKILL = "zvmem";

// Scenario prompts appended after the expanded skill block — they tell the agent
// which workflow step to execute (SKILL.md has no per-scenario sections).
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

/** Find the loaded zvmem skill command (gives us its file path). */
function findSkill(pi: ExtensionAPI) {
  return pi.getCommands().find((c) => c.source === "skill" && c.name === `skill:${SKILL}`);
}

/** Build the same prompt text pi would produce for `/skill:zvmem <scenario>`. */
async function buildSkillPrompt(pi: ExtensionAPI, scenarioText: string): Promise<string> {
  const cmd = findSkill(pi);
  if (!cmd?.sourceInfo.path) throw new Error(`skill "${SKILL}" is not loaded`);
  const raw = await readFile(cmd.sourceInfo.path, "utf8");
  const body = stripFrontmatter(raw).trim();
  const baseDir = cmd.sourceInfo.baseDir ?? dirname(cmd.sourceInfo.path);
  const block = `<skill name="${SKILL}" location="${cmd.sourceInfo.path}">\nReferences are relative to ${baseDir}.\n\n${body}\n</skill>`;
  return `${block}\n\n${scenarioText}`;
}

/** Inject the skill as a custom message and trigger an agent turn. */
async function runSkill(pi: ExtensionAPI, ctx: ExtensionContext, scenario: Scenario): Promise<void> {
  let text: string;
  try {
    text = await buildSkillPrompt(pi, SCENARIOS[scenario]);
  } catch (err) {
    if (ctx.hasUI) ctx.ui.notify(`zvmem_pi: ${errMsg(err)} — install its SKILL.md (e.g. ~/.pi/agent/skills/)`, "error");
    return;
  }

  const message = { customType: "zvmem-skill", content: text, display: false };
  try {
    if (ctx.isIdle()) {
      // Idle: triggers a turn immediately.
      await pi.sendMessage(message, { triggerTurn: true });
    } else {
      // Agent is busy: queue it to run as its own turn once things settle.
      await pi.sendMessage(message, { deliverAs: "followUp", triggerTurn: true });
    }
    if (ctx.hasUI) ctx.ui.notify(`zvmem_pi: running /skill:${SKILL} (${scenario})`, "info");
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

    // Fire-and-forget so startup isn't blocked while the skill turn runs.
    void runSkill(pi, ctx, scenario);
  });

  // --- Scenario 3: after compaction ------------------------------------------
  pi.on("session_compact", async (_event, ctx) => {
    if (ctx.mode !== "tui") return;

    // Defer to the next macrotask so we never race the tail of a manual /compact.
    // For auto-compaction we are mid-run, so runSkill queues the skill turn as a
    // follow-up that starts once the current turn (or an overflow-retry) settles.
    setTimeout(() => {
      void runSkill(pi, ctx, "afterCompaction");
    }, 0);
  });
}
