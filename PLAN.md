# zvmem — Plan

A standalone C++ CLI tool that gives coding agents a memory interface over a local
vector database. Agents create, read, update, and delete "memories" — each backed by
dense + sparse embeddings produced by a separate GPU embedding service.

**Status:** M1 + M2 complete.
- `zvmem` (C++) builds via CMake against the pinned zvec v0.7.0 prebuilt SDK; all
  eight commands work end-to-end, including all four search modes, `--rerank`, and
  every documented exit code.
- **M2 verified against the real GPU service** (`zvmemd` on CUDA): adds embed at ~250 ms;
  dense mode ranks a zero-keyword-overlap semantic query correctly (real bge-m3,
  not stubs); hybrid RRF, FTS and sparse modes all rank as expected; `--rerank`
  cross-encoder scores cleanly separate relevant from irrelevant docs; update
  re-embeds while preserving `created_at`; stats report full index completeness.
- **M3a done:** agent-facing usage doc at `SKILL.md` (public, infra-free); every
  documented flow verified against the real service. This surfaced two CLI parser
  bugs, now fixed: values starting with `-` (e.g. markdown-list content) were
  rejected, and global flags only worked after the command — both orderings work.
- **S1 done:** zvec v0.7.0 builds from source and links statically into a single
  self-contained ~50 MB binary; full command suite verified with scores identical to
  the shared-library build. Key gotcha: the four internal archives must be linked
  with `--whole-archive` — indexers self-register via static initializers (ailego
  Factory pattern) and plain archive linking drops them, causing runtime "Create
  vector column indexer failed" (the exact failure zvec's own factory.h documents).
- **Static integration done:** `ZVMEM_ZVEC_MODE` option in CMakeLists — `static`
  (default) builds zvec from source via add_subdirectory and links with
  `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>` on the four internal libs; `shared` keeps the
  prebuilt-SDK path. Both modes verified end-to-end against the GPU service.
Remaining: M3 distribution/packaging (README, cross-platform builds), crash-recovery
test.

## 1. Goals & non-goals

**Goals**
- Single self-contained binary (`zvmem`), no runtime services of our own.
- Local, durable storage: memories survive process exits and machine reboots; safe under concurrent agent invocations.
- Hybrid recall: dense (semantic) + sparse (token-weighted) + full-text keyword search, fused in one query.
- Machine-readable contract: JSON on stdout, stable exit codes — designed to be driven by agents, not humans.

**Non-goals (v1)**
- No embedding generation inside the tool (delegated to the remote GPU service).
- No multi-user/auth story for the local store; single machine, one or more agent processes sharing a directory.
- No replication/backup tooling beyond what zvec's file layout gives us for free (copy the directory).

## 2. Architecture

```
 coding agent
      |
      v
   zvmem (C++ binary)
     |            \
     |             \--- HTTP+JSON ---> embedding service (GPU server, built later)
     v                                     returns dense + sparse vectors
  zvec collection on disk
  (WAL + segments, file-locked; multi-reader / single-writer)
```

- **zvmem** is a thin orchestrator: parse args → (if needed) fetch embeddings from the
  remote service → read/write the local zvec collection → emit JSON.
- **zvec** is embedded in-process (linked statically, see §8). A "database" is just a
  directory on disk; each CLI invocation opens it, does its work, and closes it.
- **Embedding service** is a separate application on a GPU server (out of scope here;
  pinned models: BAAI/bge-m3 for dense+sparse embeddings, BAAI/bge-reranker-v2-m3
  for optional re-ranking). The HTTP contract is fixed now — §5.

## 3. Data model

One zvec collection per memory store (one directory). Schema:

| Field        | Type                  | Index            | Purpose |
|--------------|-----------------------|------------------|---------|
| `pk`         | string (doc primary key, not a schema field) | — | CLI-generated ULID; optional agent-supplied slug via `--id` |
| `summary`    | STRING                | plain            | The 1–2 paragraph search text the agent wrote; this is what gets embedded. Stored for provenance and re-embedding on update. |
| `content`    | STRING                | **FTS** (`FtsIndexParams`) | Raw memory payload, keyword-searchable via BM25. zvec treats indexed scalar fields as forward (stored) fields too — the text is returned by fetch/query, so no duplicate field is needed (confirmed in zvec schema impl; re-verified end-to-end in M0). |
| `dense`      | VECTOR_FP32(dim)      | FLAT (default) or HNSW, metric per §7 | Dense embedding of the summary. |
| `sparse`     | SPARSE_VECTOR_FP32    | inverted         | Sparse embedding of the summary (uint32 indices + float values). |
| `created_at` | INT64                 | inverted         | Epoch millis; filterable. |
| `updated_at` | INT64                 | inverted         | Epoch millis; set on add and update. |
| `model`      | STRING                | plain            | Embedding model identifier from the service response; enables selective re-embedding if the model changes later. |
| `tags`       | ARRAY_STRING          | inverted         | Optional agent-supplied labels for filtered search. |

Notes:
- The summary is embedded, not the content — content can be large and is matched via
  FTS instead of vectors.
- Every doc carries both a dense and a sparse vector from the *same* embedding call,
  so one `add` costs one round-trip to the GPU service.
- Model profile (bge-m3): dense dimension **1024**, dense metric **COSINE** (bge-m3
  normalizes its dense vectors), sparse metric **IP** — zvec forces IP on all sparse
  fields regardless of configuration. `init` applies the profile; explicit overrides
  exist for future models.

## 4. CLI surface

Global flags (all commands): `--path DIR` (collection directory; else `$ZVMEM_PATH`;
else `~/.zvmem/default`), `--embed-url URL`, `--embed-token TOKEN` (or env vars, §5).

```
zvmem init    [--model bge-m3] [--index flat|hnsw] [--dim N] [--dense-metric cosine|ip]
              Create the collection. The model profile fixes dim + metrics (bge-m3 →
              1024, dense=cosine); explicit overrides exist for future models.

zvmem add     --summary TEXT --content TEXT [--id SLUG] [--tags a,b,c]
              Embeds the summary (one round-trip), upserts the doc, prints its id.
              Without --id: CLI generates a ULID. With --id: slug must be unique;
              collision is an error (exit 2).

zvmem search  --query TEXT [-k N] [--mode hybrid|dense|sparse|fts]
              [--filter EXPR] [--content] [--rerank] [--candidates N]
              Embeds the query, runs the selected mode. Default mode: hybrid
              (dense + sparse + FTS fused via RRF MultiQuery). `--mode fts` needs no
              embedding service — pure keyword fallback. `--content` includes full
              content in results (default: summary only, to keep output small).
              `--rerank`: two-stage search — retrieve top `--candidates` (default
              max(3k, 50)), re-score with bge-reranker-v2-m3 via the embedding service,
              return top k by rerank score. Opt-in: adds one network round-trip +
              cross-encoder inference per search.

zvmem get     ID [ID...]
              Fetch full docs by id.

zvmem update  ID --summary TEXT [--content TEXT] [--tags a,b,c]
              Always re-embeds the (new) summary and replaces dense+sparse vectors;
              updates content/tags/updated_at as given. Stale vectors are never kept.

zvmem delete  ID [ID...]
              Deletes by id. Reports per-id outcome: {"deleted":[...], "not_found":[...]}.

zvmem list    [--limit N]
              Full scan via DocIterator (no embedding needed). JSON array of docs.

zvmem stats
              Collection stats from zvec (doc count, segment info) + schema summary.
```

Design rules:
- Text in, ranked memories out — agents never handle raw vectors; the CLI owns all
  vector plumbing.
- Every result object always includes `id`, so agents can immediately `get`/`update`/
  `delete` what they found.
- `search --mode fts` and `list` work with the embedding service completely offline.

## 5. Embedding service contract (HTTP + JSON)

Pinned models: **BAAI/bge-m3** (embeddings: dense 1024-dim + sparse, max 8192 tokens)
and **BAAI/bge-reranker-v2-m3** (cross-encoder re-ranking). Fixed now so the GPU app
can be built against it later.

```
POST {embed-url}/v1/embeddings
Authorization: Bearer <token>          # token from --embed-token / $ZVMEM_EMBED_TOKEN
Content-Type: application/json

Request:
{
  "model": "bge-m3",                   // optional; server default if omitted
  "texts": ["summary paragraph ..."]   // batch of 1..N texts
}

Response (200):
{
  "model": "bge-m3-v1.2",              // resolved model id — stored per doc as `model`
  "metric": "cosine",                  // metric the model was trained with: cosine|ip
  "dense":  [[0.1, -0.4, ...]],        // one fp32 vector per text, len == dim
  "sparse": [{"indices": [12, 987], "values": [0.4, 0.9]}]   // parallel to texts
}

Errors: non-2xx → zvmem exits 4 with the server's message on stderr.

POST {embed-url}/v1/rerank
Authorization: Bearer <token>
Content-Type: application/json

Request:
{
  "query":     "search text ...",
  "documents": ["doc text 1", "doc text 2", ...]   // candidate texts, parallel to scores
}

Response (200):
{
  "model":  "bge-reranker-v2-m3",
  "scores": [0.98, 0.31, ...]                     // relevance per document, higher = better
}
```

Client behavior: connect timeout 2 s, read timeout 30 s (embedding can be slow), one
retry on network-level failures only. The response `metric` is validated against the
collection's dense metric at first use; mismatch → exit 6 with a clear message.
Rerank input texts are truncated to ~8k chars per doc before sending (bge-reranker-v2-m3
supports long context, but we keep HTTP payloads bounded).
zvmemd implementation notes (verified against FlagEmbedding 1.4.2):
- The embedder class is `BGEM3FlagModel` (the old `BGEM3FlagEmbeding` name no longer
  exists); max lengths are **constructor** params (`query_max_length`/
  `passage_max_length`, default 512 — must be set to 8192).
- Sparse output key is `lexical_weights`: list of dicts keyed by **str(token_id)** →
  weight (ReLU'd sparse-linear head, max-pooled across tokens, special tokens zeroed,
  only positive weights kept). zvmemd converts keys back to ints for the contract.

## 6. Output contract

- **stdout:** exactly one JSON document per invocation, nothing else.
  - `add` → `{"id": "...", "created_at": ...}`
  - `search` → `{"results": [{"id","score"(,"rerank_score"),"summary","tags","model","updated_at"(,"content")}, ...]}` sorted by rerank score (if `--rerank`) else fused retrieval score, desc
  - `get` → `{"docs": [ {full doc} ]}` (unknown ids listed under `"not_found"`)
  - `update` → `{"id": "...", "updated_at": ...}`
  - `delete` → `{"deleted": [...], "not_found": [...]}`
  - `list`/`stats` → JSON object/array as above
- **stderr:** terse one-line errors (`error: <what> — <hint>`), never JSON.
- **Exit codes (stable):**

| Code | Meaning |
|------|---------|
| 0    | success (including partial delete with `not_found` reported) |
| 1    | unexpected/internal error |
| 2    | usage/argument error, id collision on `add --id` |
| 3    | write lock busy past timeout (another process holds the collection) |
| 4    | embedding service unreachable or returned an error |
| 5    | requested id(s) not found (`get`/`update`) |
| 6    | collection missing, or schema/config mismatch (dim/metric/index vs stored schema) |

## 7. Index type & metric decisions

- **Index:** FLAT by default — exact search, zero tuning, single-digit-ms at expected
  scale; the embedding round-trip dominates query latency anyway. `--index hnsw` at
  `init` for large deployments (approximate, ~95–99% recall, sub-ms into the millions).
  IVF/DiskANN/quantized indexes are out of scope but available in zvec if a collection
  ever grows to billion scale.
- **Metric:** per-field, fixed by the model profile at `init` (bge-m3 → dense COSINE;
  sparse is always IP — zvec enforces this on all sparse fields). Must match what the
  embedding model was trained with — enforced by validating against the service
  response (§5). Stored in the collection schema, so every query uses a consistent
  comparison. (zvec also offers L2; rarely right for text embeddings.)

## 8. Build & distribution

- C++20, CMake. zvec pinned to a specific release (v0.7.x at planning time), fetched as
  an artifact — never "latest". External deps: zvec + nlohmann/json (single header)
  + libcurl for the embedding-service client.
- **Default (current):** build zvec v0.7.0 from source (`./zvec-src` checkout with
  submodules, or `-DZVEC_SOURCE_DIR=`) and link statically → one fully self-contained
  ~50 MB binary (~2 min fresh build on a 24-core box). The four internal archives are
  linked with `--whole-archive` so factory-registration static initializers survive.
- **Opt-in fast build:** `-DZVMEM_ZVEC_MODE=shared` uses the prebuilt SDK tarball
  (downloaded at configure time, shipped next to the binary with `$ORIGIN` rpath) —
  ~4 s builds; keeps the old two-file layout.
- Targets: Linux x86_64/arm64, macOS arm64/x86_64 first; Windows later. Plain HTTP for
  v1 (LAN GPU server); TLS can be added via libcurl/openssl if ever needed.

## 9. Concurrency & durability

- zvec file locking gives multi-reader / single-writer semantics across processes —
  exactly what concurrent agent invocations need. Read commands (`search`/`get`/`list`/
  `stats`) open the collection read-only and never block on writers.
- Writer UX: before opening a write handle, zvmem takes its own advisory exclusive lock
  (flock on `<path>/.zvmem.lock`, non-blocking retry loop) for up to `--lock-timeout`
  (default 10 s); on timeout it exits 3 with a stderr hint ("another process is writing;
  retry shortly"). Holding our lock first guarantees zvec's internal write lock won't
  block us, so the timeout behavior is deterministic.
- WAL guarantees durability across crashes/power loss; no extra work needed from us.

## 10. Open questions / spike list

| # | Question | How to resolve |
|---|----------|----------------|
| S1 | ~~Static zvec build → clean single binary~~ **Done:** Release build of v0.7.0 from source (~3 min, 24 cores) + link recipe in `/tmp/zvmem-static-test/link.sh` produces a 50 MB self-contained binary (vs ~47 MB total as binary+`.so`). Requires `--whole-archive` on libzvec/libzvec_core/libzvec_ailego/libzvec_turbo.a for factory-registration static initializers. Verified: all commands, scores identical to shared build. |
| S2 | ~~Are FTS-indexed fields also returned by fetch/query output?~~ **Resolved & verified:** zvec's schema treats indexed scalar fields as forward (stored) fields — `content` is indexed directly and its text comes back from fetch/search in the M1 smoke test. No duplicate field needed. |
| S3 | ~~Exact index params for the sparse field~~ **Resolved:** use `FlatIndexParams(MetricType::IP)` for the sparse field (zvec test helpers force IP on all sparse fields; IVF/RaBitQ/DiskANN don't support sparse at all). Verify end-to-end in M0. |
| S4 | `update()` semantics: full-doc replace vs partial field update? | Check zvec docs; our CLI always sends the complete doc, so either works — pick whichever is simpler to drive correctly. |
| S5 | ~~Filter expression syntax~~ **Done:** verified live — array fields need the function form `tags contain_any('a','b')` (`=` is rejected on arrays); scalars take normal comparisons (`created_at > N`); combine with `and`. Documented in SKILL.md. |
| S6 | ~~ULID generation~~ **Done:** `src/ulid.h`, ~30 lines, no deps. |

v0.7.0 API quirks found during implementation (pinned SDK, not main branch):
- `fetch()` returns a map entry with a **null doc pointer** for missing ids (the Python
  binding omits them) — always null-check before dereferencing (`Store::get/exists`).
- `delete_(pks)` returns `Result<WriteResults>` (per-doc statuses), not a bare `Status`.

## 11. Milestones

- **M0 — Spike (S1, S4–S6):** static-link feasibility; update() semantics; filter
  syntax subset for help text; ULID impl. (S2/S3 resolved during planning.)
- **M1 — Core CRUD + offline search:** `init/add/get/update/delete/list/stats` and all
  four search modes against a stub embedder (canned vectors). Full output contract,
  exit codes, advisory write lock. Smoke test end-to-end.
- **M2 — Real embedding service (zvmemd):** DONE as a Python daemon in `zvmemd/`
  (FastAPI + FlagEmbedding 1.4.2 `BGEM3FlagModel` for bge-m3 dense+sparse, plain
  transformers for bge-reranker-v2-m3). Verified on CPU with real models; deploy to
  the GPU server via `zvmemd/run.sh`. zvmem's client side (retries/timeouts/metric
  validation/`--rerank`) was already exercised against the stub in M1.
- **M3 — Hardening & distribution:** lock-timeout behavior under concurrent load,
  crash-recovery test (kill -9 mid-write), cross-platform builds, README + agent-facing
  usage doc (the "skill" text agents will read).
