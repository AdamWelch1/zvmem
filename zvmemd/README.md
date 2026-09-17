# zvmemd

Embedding + re-ranking daemon for **zvmem** (see `../PLAN.md` §5). Serves the
embedding-service contract over HTTP+JSON:

- `POST /v1/embeddings` — bge-m3 dense (1024-dim, cosine) + sparse vectors
- `POST /v1/rerank` — bge-reranker-v2-m3 cross-encoder scores
- `GET /health` — liveness + model/device info

Runs on a single machine with an NVIDIA GPU (CUDA). The whole folder is
self-contained: copy it to the GPU server and run `./run.sh`.

## Quickstart

```bash
# On the GPU server (Python 3.12 required):
cp -r zvmemd /opt/          # or anywhere
cd /opt/zvmemd
./run.sh                    # creates .venv, installs deps, serves on :8080
```

`run.sh` uses `python3.12` to create the venv; point it at a different
interpreter with `ZVMEMD_PYTHON=/path/to/python ./run.sh` if needed.

First start downloads both models from Hugging Face (~4.5 GB total) into the
HF cache (`~/.cache/huggingface`). Point zvmem at it:

```bash
export ZVMEM_EMBED_URL=http://<gpu-server>:8080
zvmem add --summary "..." --content "..."
```

### Air-gapped / pre-downloaded models

Download the weights on a machine with internet access, copy them over, and
point the env vars at local directories:

```bash
huggingface-cli download BAAI/bge-m3 --local-dir /opt/models/bge-m3
huggingface-cli download BAAI/bge-reranker-v2-m3 --local-dir /opt/models/bge-reranker-v2-m3

ZVMEMD_EMBED_MODEL=/opt/models/bge-m3 \
ZVMEMD_RERANK_MODEL=/opt/models/bge-reranker-v2-m3 \
./run.sh
```

## Configuration (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `ZVMEMD_HOST` | `0.0.0.0` | bind address |
| `ZVMEMD_PORT` | `8080` | port |
| `ZVMEMD_DEVICE` | `auto` | `cuda`, `cpu`, a specific GPU (`cuda:0`, `cuda:1`, ...), or auto-detect |
| `ZVMEMD_PYTHON` | `python3.12` | interpreter used by `run.sh` to create the venv |
| `ZVMEMD_EMBED_MODEL` | `BAAI/bge-m3` | HF repo id or local dir |
| `ZVMEMD_RERANK_MODEL` | `BAAI/bge-reranker-v2-m3` | HF repo id or local dir |
| `ZVMEMD_MAX_LENGTH` | `8192` | max tokens per input (longer inputs are truncated) |
| `ZVMEMD_BATCH_SIZE` | `16` | internal batch size for embedding calls |
| `ZVMEMD_FP16` | `true` | fp16 inference on CUDA |
| `ZVMEMD_FAKE` | `false` | deterministic fake models — no GPU/torch needed (contract testing) |

## Contract examples

```bash
curl -s localhost:8080/v1/embeddings \
  -H 'Content-Type: application/json' \
  -d '{"texts": ["How we fixed the worker pool deadlock"]}'
# → {"model":"BAAI/bge-m3","metric":"cosine",
#    "dense":[[...1024 floats...]],
#    "sparse":[{"indices":[12,987,...],"values":[0.4,0.9,...]}]}

curl -s localhost:8080/v1/rerank \
  -H 'Content-Type: application/json' \
  -d '{"query":"deadlock in worker pool","documents":["...doc1...","...doc2..."]}'
# → {"model":"BAAI/bge-reranker-v2-m3","scores":[0.98,0.31]}

curl -s localhost:8080/health
```

## Behavior notes

- **Startup loading:** both models load (and warm up) before the server starts
  accepting traffic, so GPU memory is reserved immediately — check `/health`
  (`models_loaded`) and `nvidia-smi` right after startup. If loading fails, the
  daemon exits non-zero instead of serving 503s.
- **Truncation:** inputs longer than `ZVMEMD_MAX_LENGTH` tokens are truncated by
  the tokenizer (bge-m3 and bge-reranker-v2-m3 both support up to 8192).
- **Batching:** each request's texts are batched internally; concurrent requests
  are serialized on a single inference lock. For agent-memory traffic this is
  plenty; if you ever need higher QPS, the next step is cross-request batching
  or TensorRT — the HTTP contract stays unchanged.
- **Metric:** bge-m3 dense vectors are L2-normalized, so `metric` is reported as
  `cosine` (equivalent to inner product for normalized vectors). zvmem validates
  this against its collection config.
- **Errors:** model load/inference failures return HTTP 503 with a message;
  malformed requests return 422.

## Testing without a GPU

```bash
ZVMEMD_FAKE=1 ZVMEMD_PORT=8931 ./run.sh
# deterministic bag-of-words stand-in — same contract, no torch/GPU needed.
```

This is what the zvmem smoke tests use; it's also a reference for exactly what
`/v1/embeddings` must return (dense list + sparse index/value pairs).
