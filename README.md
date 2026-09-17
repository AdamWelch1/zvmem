# zvmem — persistent memory for coding agents

**zvmem** gives coding agents a durable memory that survives context compaction and session restarts. It is a small C++ CLI that stores memories in a local vector database ([zvec](https://github.com/alibaba/zvec)) on the machine where the agent runs, with dense + sparse embeddings produced by a separate GPU service (**zvmemd**) running BAAI's [bge-m3](https://huggingface.co/BAAI/bge-m3) and [bge-reranker-v2-m3](https://huggingface.co/BAAI/bge-reranker-v2-m3).

- **Memories stay local.** The database is plain files on the client machine; only summary text ever leaves it (sent to the embedding service).
- **Hybrid retrieval.** Semantic (dense), lexical (sparse) and full-text (FTS) search, fused with RRF — plus optional cross-encoder re-ranking for precision.
- **One binary to run.** The default build is a single self-contained ~50 MB executable; the client side runs no services of its own.

## Components

| Component | What it is |
|---|---|
| `zvmem` (C++ CLI) | The memory interface: `init`, `add`, `search`, `get`, `update`, `delete`, `list`, `stats`. Talks to a local zvec collection and the remote embedding service. JSON on stdout, terse errors on stderr, stable exit codes. |
| [`zvmemd/`](zvmemd/) (Python daemon) | Self-contained GPU service serving `/v1/embeddings` (bge-m3 dense + sparse) and `/v1/rerank` (cross-encoder). Copy the folder to any NVIDIA-GPU machine, run `./run.sh`, done. |
| [`SKILL.md`](SKILL.md) | The agent-facing skill: when to save/recall, how to write good memories, full command reference, exit-code handling. Install it wherever your coding agent discovers skills. |
| [`zvmem_pi/`](zvmem_pi/) (pi extension) | A pi coding-agent extension that runs the zvmem skill automatically at session start, on resume, and after compaction — so agents recall state without being prompted. |

## Quick start

### 1. Start the embedding service (on a GPU machine)

```bash
cp -r zvmemd /opt/zvmemd && cd /opt/zvmemd
./run.sh    # creates .venv, installs deps, downloads both models (~4.5 GB), serves on :8080
curl http://<gpu-host>:8080/health   # → {"status":"ok", ...}
```

### 2. Build `zvmem` (on the machine where your agent runs)

```bash
git clone https://github.com/AdamWelch1/zvmem && cd <repo>

# zvec source, pinned v0.7.0 with submodules:
git clone https://github.com/alibaba/zvec.git zvec-src
cd zvec-src && git checkout v0.7.0 && git submodule update --init --recursive && cd ..

cmake -S . -B build && cmake --build build -j
./build/zvmem --help
```

### 3. Create your first memory (in any project)

```bash
export ZVMEM_EMBED_URL="http://<gpu-host>:8080"
cd /path/to/your/project
zvmem --path .zvmem init
zvmem --path .zvmem add \
  --summary "Postmortem: worker pool deadlock from circular lock ordering" \
  --content "<full raw payload: code, logs, rationale>" \
  --tags postmortem,concurrency
zvmem --path .zvmem search --query "why did our threads hang"
```

### 4. Wire it into your agent

- **Any agent:** copy `SKILL.md` to where the agent discovers skills (e.g. `~/.pi/agent/skills/zvmem/SKILL.md`). The skill teaches the workflow: recall at session start, save as you learn, keep a stable `session-state` memory fresh.
- **pi users:** additionally enable [`zvmem_pi/index.ts`](zvmem_pi/) so the skill runs automatically on new sessions, resumes, and after compaction (see its README).

## Using zvmem

All commands print one JSON document to stdout; errors go to stderr with a stable exit code. `zvmem --help` has the full reference — this is an overview.

```bash
zvmem [--path DIR] <command> [args]     # global flags may also follow the command

init      create the collection (once per project)
add       --summary TEXT --content TEXT [--tags a,b,c] [--id SLUG]
search    --query TEXT [-k N] [--mode hybrid|dense|sparse|fts]
          [--filter EXPR] [--content] [--rerank] [--candidates N]
get       ID [ID...]                    full docs by id
update    ID --summary TEXT [...]       re-embeds, keeps created_at
delete    ID [ID...]
list      [--limit N] [--content]       full scan (no embedding service needed)
stats     doc count, index completeness, schema
```

Search modes: `hybrid` (default; dense + sparse + FTS fused via RRF), `dense` (semantic — good for paraphrased queries), `fts` (exact terms in content; **works with no GPU service**), `sparse` (lexical overlap with summaries). Add `--rerank` to re-score candidates with the cross-encoder.

### Configuration

| Env var | Flag | Default | Purpose |
|---|---|---|---|
| `ZVMEM_PATH` | `--path DIR` | `~/.zvmem/default` | Collection directory. Convention: `.zvmem` inside each project root (gitignore it). |
| `ZVMEM_EMBED_URL` | `--embed-url URL` | — | Base URL of the embedding service. Required for `add`, `update`, and non-FTS search. |
| `ZVMEM_EMBED_TOKEN` | `--embed-token TOKEN` | — | Bearer token sent with requests (the daemon itself is unauthenticated; useful if you put it behind an auth proxy). |
| — | `--lock-timeout MS` | `10000` | How long writers wait for the advisory write lock before failing. |

Exit codes: `0` ok · `1` internal · `2` usage · `3` lock busy (retry) · `4` embedding service error · `5` not found · `6` collection/schema mismatch. The [skill](SKILL.md) documents how an agent should react to each.

## Using zvmemd

Deploy the self-contained folder on any machine with an NVIDIA GPU:

```bash
cp -r zvmemd /opt/zvmemd && cd /opt/zvmemd
./run.sh    # venv + deps + models, then serves; config via env vars below
```

| Env var | Default | Purpose |
|---|---|---|
| `ZVMEMD_HOST` / `ZVMEMD_PORT` | `0.0.0.0` / `8080` | Bind address. |
| `ZVMEMD_DEVICE` | `auto` | `cuda`, `cpu`, or a specific GPU: `cuda:0`, `cuda:1`, … |
| `ZVMEMD_EMBED_MODEL` | `BAAI/bge-m3` | Model name or local directory (air-gapped boxes). |
| `ZVMEMD_RERANK_MODEL` | `BAAI/bge-reranker-v2-m3` | Same. |
| `ZVMEMD_MAX_LENGTH` | `8192` | Token limit per input (both models support 8192). |
| `ZVMEMD_BATCH_SIZE` | `16` | Internal inference batch size. |
| `ZVMEMD_FP16` | `1` | Half-precision on CUDA (~halves memory, no quality loss in practice). |
| `ZVMEMD_FAKE` | — | Set to `1` for a deterministic no-GPU stand-in (testing without a GPU). |

Both models load at startup and stay resident in GPU memory; `/health` reports status, device, and model-load state. Endpoints: `POST /v1/embeddings`, `POST /v1/rerank`, `GET /health`. See [`zvmemd/README.md`](zvmemd/README.md) for the full contract and air-gapped setup.

## Building

Requirements: CMake ≥ 3.26, a C++20 compiler (GCC ≥ 11 or Clang ≥ 14), libcurl development headers (`libcurl4-openssl-dev` on Debian/Ubuntu).

### Static build (default) — one self-contained binary

```bash
cmake -S . -B build && cmake --build build -j
# → build/zvmem (~50 MB, no shared-library dependencies beyond system libs)
```

This builds [zvec](https://github.com/alibaba/zvec) v0.7.0 from source (expected at `./zvec-src`, override with `-DZVEC_SOURCE_DIR=`) and links it statically into the binary. A fresh build takes a few minutes because zvec's vendored dependencies (Arrow, RocksDB, …) compile from source; incremental rebuilds of just `zvmem` are fast.

### Dynamic build — prebuilt SDK, fastest builds

```bash
cmake -S . -B build -DZVMEM_ZVEC_MODE=shared && cmake --build build -j
# → build/zvmem (~400 KB) + build/lib/libzvec.so (shipped next to the binary)
```

Downloads the pinned prebuilt zvec SDK at configure time and links against `libzvec.so` (rpath set so the binary finds it). Use this when you don't want to compile zvec's dependencies, e.g. for quick development iterations.

## Third-party code

| Component | Role | License |
|---|---|---|
| [zvec](https://github.com/alibaba/zvec) v0.7.0 | Vector database (dense + sparse + FTS, hybrid search). Built from source and statically linked in the default build; its vendored deps (Apache Arrow, RocksDB, ANTLR4, glog/gflags, cppjieba, …) are compiled in as well. | Apache-2.0 |
| [nlohmann/json](https://github.com/nlohmann/json) v3.11.3 | JSON parsing/serialization (single header). | MIT |
| [libcurl](https://curl.se/) | HTTP client for the embedding service. | curl license (MIT-style) |

The static build embeds zvec and its dependencies into the binary; their licenses continue to apply to those components (Apache-2.0 requires preserving copyright notices — see `zvec-src/LICENSE` / `NOTICE`). No copyleft code is involved, so this project's own code remains MIT-licensed.

## License

This project's code is licensed under the [MIT License](LICENSE). Third-party components are covered by their own licenses as listed above.
