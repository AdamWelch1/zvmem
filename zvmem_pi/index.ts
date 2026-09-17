import { join } from "node:path";
import type { ExtensionAPI, ExtensionContext } from "@earendil-works/pi-coding-agent";
import { StringEnum } from "@earendil-works/pi-ai";
import { Type } from "typebox";

/**
 * zvmem_pi — persistent project memory for pi.
 *
 * Bakes the entire zvmem skill into a first-class `zvmem` tool (init/add/
 * search/get/update/delete/list/stats over <cwd>/.zvmem) and wires lifecycle
 * recall:
 *
 *   - New session / resume: asks the user (confirm dialog) whether to load the
 *     saved session state + relevant memories. On yes, recall runs
 *     deterministically via the CLI and is injected as a visible custom message
 *     WITHOUT triggering an LLM turn — zero cost, becomes context for the first
 *     prompt, and never starts coding work on its own.
 *   - After compaction: automatic (no confirm) — same deterministic recall,
 *     injected with resume guidance; delivered via steer when mid-run so it
 *     lands in the next LLM call (including overflow-retry).
 *
 * Plan + progress: zvmem memory id 'zvmem-pi-overhaul'.
 */

const COLLECTION_DIR = ".zvmem";
const MAX_OUTPUT_CHARS = 20_000;
const EXEC_TIMEOUT_MS = 90_000; // embedding calls can be slow (30s read timeout each)

// Memory-save reminders: invisible to the user, injected into a single LLM
// request via the context event — never persisted, never shown in transcript.
const TEN_MINUTES_MS = 10 * 60_000;            // staleness threshold for the time-based trigger
const MIN_NUDGE_GAP_MS = 30_000;               // global floor: no two reminders closer than this (any trigger)
const STALE_REMINDER_COOLDOWN_MS = 5 * 60_000; // repeat cooldown for the time-based trigger only
const REMINDER_TEXT =
  "[Automated zvmem reminder — not typed by the user] You have done work since your last memory save. " +
  "If you learned anything non-obvious (a decision and why, a gotcha, an environment quirk), persist it now with the zvmem tool: " +
  "one focused memory per fact; keep session-state concise with pointers to other memories. If nothing warrants saving, just continue.";

// ---------------------------------------------------------------------------
// zvmem CLI plumbing — shared by the tool and lifecycle recall
// ---------------------------------------------------------------------------

class ZvmemCliError extends Error {
  constructor(message: string, public readonly code: number) {
    super(message);
  }
}

interface ZvmemParams {
  action: "init" | "add" | "search" | "get" | "update" | "delete" | "list" | "stats";
  id?: string;
  ids?: string[];
  summary?: string;
  content?: string;
  tags?: string[];
  query?: string;
  k?: number;
  mode?: "hybrid" | "dense" | "sparse" | "fts";
  filter?: string;
  includeContent?: boolean;
  rerank?: boolean;
  candidates?: number;
  limit?: number;
}

function errMsg(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

function sleep(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve) => {
    const t = setTimeout(resolve, ms);
    signal?.addEventListener("abort", () => {
      clearTimeout(t);
      resolve();
    }, { once: true });
  });
}

async function runOnce(
  pi: ExtensionAPI,
  cwd: string,
  argv: string[],
  signal?: AbortSignal,
): Promise<{ code: number; stdout: string; stderr: string }> {
  // pi.exec spawns without a shell — array args go straight to execve, so long
  // content strings need no quoting.
  return pi.exec("zvmem", ["--path", join(cwd, COLLECTION_DIR), ...argv], {
    signal,
    timeout: EXEC_TIMEOUT_MS,
  });
}

/** Raw exec with SKILL.md retry policy: rc1 (internal) retry once; rc3 (lock busy) wait + retry. */
async function runZvmem(
  pi: ExtensionAPI,
  cwd: string,
  argv: string[],
  signal?: AbortSignal,
): Promise<{ code: number; stdout: string; stderr: string }> {
  let res = await runOnce(pi, cwd, argv, signal);
  if (res.code === 1) res = await runOnce(pi, cwd, argv, signal);
  if (res.code === 3) {
    await sleep(2000, signal);
    res = await runOnce(pi, cwd, argv, signal);
  }
  return res;
}

function buildArgv(action: ZvmemParams["action"], p: ZvmemParams): string[] {
  const targets = p.ids ?? (p.id ? [p.id] : []);
  switch (action) {
    case "init":
      return ["init"];
    case "add": {
      if (!p.summary || !p.content) throw new ZvmemCliError("add requires summary and content", 2);
      const argv = ["add", "--summary", p.summary, "--content", p.content];
      if (p.tags?.length) argv.push("--tags", p.tags.join(","));
      if (p.id) argv.push("--id", p.id);
      return argv;
    }
    case "search": {
      if (!p.query) throw new ZvmemCliError("search requires query", 2);
      const argv = ["search", "--query", p.query, "-k", String(p.k ?? 10)];
      if (p.mode && p.mode !== "hybrid") argv.push("--mode", p.mode);
      if (p.filter) argv.push("--filter", p.filter);
      if (p.rerank) argv.push("--rerank");
      if (p.candidates != null) argv.push("--candidates", String(p.candidates));
      if (p.includeContent) argv.push("--content");
      return argv;
    }
    case "get": {
      if (!targets.length) throw new ZvmemCliError("get requires id or ids", 2);
      return ["get", ...targets];
    }
    case "update": {
      if (targets.length !== 1) throw new ZvmemCliError("update requires exactly one id", 2);
      if (!p.summary) throw new ZvmemCliError("update requires summary", 2);
      const argv = ["update", targets[0], "--summary", p.summary];
      if (p.content != null) argv.push("--content", p.content);
      if (p.tags?.length) argv.push("--tags", p.tags.join(","));
      return argv;
    }
    case "delete": {
      if (!targets.length) throw new ZvmemCliError("delete requires id or ids", 2);
      return ["delete", ...targets];
    }
    case "list":
      return ["list", ...(p.limit != null ? ["--limit", String(p.limit)] : [])];
    case "stats":
      return ["stats"];
    default:
      throw new ZvmemCliError(`unknown action ${String(action)}`, 2);
  }
}

/** Run one zvmem command, apply the exit-code policy, and return display text. */
async function zvmemCall(
  pi: ExtensionAPI,
  cwd: string,
  params: ZvmemParams,
  signal?: AbortSignal,
): Promise<{ text: string; details: Record<string, unknown> }> {
  const details: Record<string, unknown> = {};
  let res = await runZvmem(pi, cwd, buildArgv(params.action, params), signal);

  // rc4 on search (not already fts): embedding service may be down — fall back once.
  if (res.code === 4 && params.action === "search" && params.mode !== "fts") {
    details.ftsFallback = true;
    res = await runZvmem(pi, cwd, buildArgv("search", { ...params, mode: "fts" }), signal);
  }

  switch (res.code) {
    case 0:
      break;
    case 6:
      if (params.action === "init") break; // collection already exists — harmless
      throw new ZvmemCliError(`collection/schema mismatch (rc 6): ${res.stderr.trim() || res.stdout.trim()}`, 6);
    case 5:
      if (params.action === "get") break; // not_found is informative — return the JSON below
      throw new ZvmemCliError(`id not found (rc 5): ${res.stderr.trim() || res.stdout.trim()}`, 5);
    default:
      throw new ZvmemCliError(res.stderr.trim() || `zvmem exited with code ${res.code}`, res.code);
  }

  details.exitCode = res.code;
  let text = res.stdout.trim();
  if (!text) text = "(no output)";
  if (text.length > MAX_OUTPUT_CHARS) {
    text = `${text.slice(0, MAX_OUTPUT_CHARS)}\n…[truncated ${text.length - MAX_OUTPUT_CHARS} chars]`;
    details.truncated = true;
  }
  return { text, details };
}

// ---------------------------------------------------------------------------
// Deterministic lifecycle recall (no LLM involved)
// ---------------------------------------------------------------------------

async function buildRecallText(pi: ExtensionAPI, cwd: string): Promise<string> {
  // Ensure the collection exists (init is idempotent; rc6 = already there).
  const stats = await runZvmem(pi, cwd, ["stats"]);
  if (stats.code === 6) {
    const initRes = await runZvmem(pi, cwd, ["init"]);
    if (initRes.code !== 0 && initRes.code !== 6) {
      throw new Error(`zvmem init failed: ${initRes.stderr.trim() || `rc ${initRes.code}`}`);
    }
    return "Memory store initialized at .zvmem — no memories saved yet. Save state as you work using the zvmem tool.";
  }
  if (stats.code !== 0) throw new Error(`zvmem stats failed: ${stats.stderr.trim() || `rc ${stats.code}`}`);

  // Read session-state (rc5 = not found, which is fine).
  const st = await runZvmem(pi, cwd, ["get", "session-state"]);
  let stateDoc: { summary?: string; content?: string } | null = null;
  if (st.code === 0 || st.code === 5) {
    try {
      stateDoc = JSON.parse(st.stdout).docs?.[0] ?? null;
    } catch {
      /* fall through with no state */
    }
  } else {
    throw new Error(`zvmem get session-state failed: ${st.stderr.trim() || `rc ${st.code}`}`);
  }

  if (!stateDoc) return "No saved session state yet — nothing has been written to zvmem in this project.";

  const parts = ["## Session state", (stateDoc.content ?? stateDoc.summary ?? "").trim()];

  // Retrieve relevant memories using the state's summary as the query.
  const searchParams: ZvmemParams = { action: "search", query: stateDoc.summary ?? "", k: 5 };
  let sr = await runZvmem(pi, cwd, buildArgv("search", searchParams));
  if (sr.code === 4) {
    // Embedding service down — fts fallback.
    sr = await runZvmem(pi, cwd, buildArgv("search", { ...searchParams, mode: "fts" }));
  }
  if (sr.code === 0) {
    try {
      const hits = (JSON.parse(sr.stdout).results ?? []).filter((r: { id?: string }) => r.id !== "session-state");
      if (hits.length) {
        parts.push("## Relevant memories", ...hits.map((h: { id: string; summary?: string }) => `- ${h.id}: ${h.summary ?? ""}`));
      }
    } catch {
      /* ignore parse issues */
    }
  }

  parts.push("", "(Use the zvmem tool to get full content by id, and keep saving as you work.)");
  return parts.join("\n");
}

// ---------------------------------------------------------------------------
// Extension
// ---------------------------------------------------------------------------

const zvmemSchema = Type.Object({
  action: StringEnum(["init", "add", "search", "get", "update", "delete", "list", "stats"]),
  id: Type.Optional(Type.String({ description: "Memory id/slug — stable slug for add; target for get/update/delete" })),
  ids: Type.Optional(Type.Array(Type.String(), { description: "Multiple memory ids (get, delete)" })),
  summary: Type.Optional(Type.String({ description: "1-2 paragraph searchable summary (required for add and update)" })),
  content: Type.Optional(Type.String({ description: "Full raw payload text — code, logs, exact commands, rationale (required for add; optional for update)" })),
  tags: Type.Optional(Type.Array(Type.String(), { description: "Short lowercase labels, e.g. postmortem, db" })),
  query: Type.Optional(Type.String({ description: "Search query text (search)" })),
  k: Type.Optional(Type.Number({ description: "Number of results to return (search; default 10)" })),
  mode: Type.Optional(StringEnum(["hybrid", "dense", "sparse", "fts"])),
  filter: Type.Optional(Type.String({ description: "SQL-like post-rank filter, e.g. tags contain_any('db') and created_at > 1789618000000" })),
  includeContent: Type.Optional(Type.Boolean({ description: "Include full content in search results (search)" })),
  rerank: Type.Optional(Type.Boolean({ description: "Re-score candidates with the cross-encoder reranker (search)" })),
  candidates: Type.Optional(Type.Number({ description: "Extra candidates to retrieve before reranking (search)" })),
  limit: Type.Optional(Type.Number({ description: "Max docs for list" })),
});

export default function (pi: ExtensionAPI) {
  // Reminder state — fresh per session instance (extensions rebind on /new, /resume).
  let lastWriteAt = Date.now(); // timer starts at session start
  let turnHadWrite = false;
  let nudgePending = false;
  let lastNudgeAt = 0;        // last reminder of any kind — enforces MIN_NUDGE_GAP_MS
  let lastStaleNudgeAt = 0;   // last time-based (trigger B) reminder

  // --- The zvmem tool — the whole skill as a first-class pi tool ------------
  pi.registerTool({
    name: "zvmem",
    label: "zvmem memory",
    description:
      "Persistent project memory store — memories survive context compaction and across sessions. " +
      "Actions: init (create the .zvmem store), add, search (hybrid/dense/sparse/fts; optional rerank/filter), " +
      "get, update, delete, list, stats. Search it before deep-diving into code/docs for information that may already be known; " +
      "recall relevant knowledge before starting work; save non-obvious facts as you learn them.",
    promptSnippet: "Persistent project memory: recall/save/update/delete memories that survive compaction and sessions",
    promptGuidelines: [
      "Search zvmem before deep-diving into code/docs for information that may already be known (prior decisions, environment quirks, how things work in this project); if you find it in code/docs instead and it's worth remembering, save it as a memory.",
      "Store knowledge as discrete memories — one focused memory per fact, decision, or event (separate small notes beat one big catch-all). Use stable descriptive ids and short tags.",
      "Keep the 'session-state' memory concise: current status, open questions, next steps, and pointers to other memories by id. Details live in their own memories — update it after every meaningful step.",
      "Track information in zvmem unless the user explicitly asks for a file (e.g. a roadmap or plan document) — then create the file instead of a memory.",
      "Save immediately when you learn something non-obvious (a decision and why, a gotcha, an environment quirk) — never batch saves 'for later'.",
      "When information changes, use zvmem update (or delete) on the existing id instead of adding near-duplicates — stale memories poison search results.",
      "Prefer the zvmem tool over raw bash calls to the zvmem CLI.",
    ],
    parameters: zvmemSchema,
    async execute(_toolCallId, params, signal, _onUpdate, ctx) {
      const result = await zvmemCall(pi, ctx.cwd, params as unknown as ZvmemParams, signal);
      if (params.action === "add" || params.action === "update") {
        lastWriteAt = Date.now();
        turnHadWrite = true;
      }
      return {
        content: [{ type: "text", text: result.text }],
        details: { ...result.details, action: params.action },
      };
    },
  });

  // --- New session / resume: opt-in recall via confirm dialog --------------
  pi.on("session_start", async (event, ctx) => {
    if (ctx.mode !== "tui") return; // scripted print/json/rpc runs are left alone
    if (event.reason === "reload") return;

    const scenario = detectScenario(event, ctx);

    // Defer so the TUI is fully interactive before showing the dialog.
    setTimeout(async () => {
      try {
        if (!ctx.hasUI) return;
        const ok = await ctx.ui.confirm(
          "zvmem: recall memory?",
          scenario === "new"
            ? "New session — load saved session state and relevant memories from zvmem?"
            : "Resuming a previous session — load its saved state from zvmem?",
        );
        if (!ok) return;
        const text = await buildRecallText(pi, ctx.cwd);
        // Visible in the transcript, but no LLM turn: becomes context for the
        // user's first prompt and never starts work on its own.
        await pi.sendMessage(
          {
            customType: "zvmem-recall",
            content: `zvmem recall (${scenario === "new" ? "new session" : "resumed session"}):\n\n${text}`,
            display: true,
          },
          { triggerTurn: false },
        );
      } catch (err) {
        if (ctx.hasUI) ctx.ui.notify(`zvmem_pi: recall failed: ${errMsg(err)}`, "error");
      }
    }, 0);
  });

  // --- Memory-save reminders (invisible, per-request only) -------------------
  pi.on("turn_start", () => {
    turnHadWrite = false;
  });

  pi.on("turn_end", (event) => {
    // Work happened this turn (>=1 tool result) but nothing was saved.
    if (!turnHadWrite && event.toolResults.length > 0) nudgePending = true;
  });

  pi.on("context", async (event, ctx) => {
    if (ctx.mode !== "tui") return; // leave scripted runs alone
    const now = Date.now();
    // Global floor: rapid successive tool calls must never produce back-to-back nudges.
    if (now - lastNudgeAt < MIN_NUDGE_GAP_MS) return;
    const staleSinceWrite = now - lastWriteAt >= TEN_MINUTES_MS;
    const staleCooldownOk = now - lastStaleNudgeAt >= STALE_REMINDER_COOLDOWN_MS;
    // Trigger A: previous turn did work without saving. Trigger B: long stretch
    // since the last save (its own cooldown prevents nagging every LLM call).
    if (!nudgePending && !(staleSinceWrite && staleCooldownOk)) return;

    nudgePending = false;
    lastNudgeAt = now;
    if (staleSinceWrite) lastStaleNudgeAt = now;
    // Append to this one request only — session state and transcript untouched.
    return {
      messages: [
        ...event.messages,
        { role: "user" as const, content: [{ type: "text" as const, text: REMINDER_TEXT }], timestamp: now },
      ],
    };
  });

  // --- After compaction: automatic recall ------------------------------------
  pi.on("session_compact", async (_event, ctx) => {
    if (ctx.mode !== "tui") return;

    // Defer to the next macrotask so we never race the tail of a manual /compact.
    setTimeout(async () => {
      try {
        const text = await buildRecallText(pi, ctx.cwd);
        const content = `Context was just compacted. Recalled from zvmem:\n\n${text}\nIf you were in the middle of a task before compaction, continue from this state.`;
        if (ctx.isIdle()) {
          // Idle: append without triggering a turn — available for the next prompt.
          await pi.sendMessage({ customType: "zvmem-recall", content, display: true }, { triggerTurn: false });
        } else {
          // Mid-run (auto-compaction / overflow retry): land in the next LLM call.
          await pi.sendMessage({ customType: "zvmem-recall", content, display: true }, { deliverAs: "steer" });
        }
      } catch (err) {
        if (ctx.hasUI) ctx.ui.notify(`zvmem_pi: post-compaction recall failed: ${errMsg(err)}`, "error");
      }
    }, 0);
  });
}

/** Distinguish a fresh session from one reopened with history. */
function detectScenario(
  event: { reason: string },
  ctx: ExtensionContext,
): "new" | "resume" {
  if (event.reason === "new") return "new";
  if (event.reason === "resume" || event.reason === "fork") return "resume";
  // reason === "startup": plain `pi` opens a fresh session, while `pi -c`,
  // --session <path>, and -r reopen an existing one.
  const hasHistory = ctx.sessionManager.getBranch().some((e) => e.type === "message");
  return hasHistory ? "resume" : "new";
}
