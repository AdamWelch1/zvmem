"""Inference engines for zvmemd.

RealEngine: bge-m3 (dense + sparse) via FlagEmbedding, bge-reranker-v2-m3 via
transformers. Models load lazily on first use; all inference is serialized by a
lock (requests within one call are batched internally).

FakeEngine: deterministic bag-of-words stand-in with the same interface and no
GPU/torch dependency — used for contract testing (ZVMEMD_FAKE=1) and as a
reference for what /v1/embeddings must return.
"""

import hashlib
import math
import re
import threading
from collections import Counter


class EngineError(RuntimeError):
    """Model load or inference failure → HTTP 503."""


# Metric each supported embedding model was trained with (zvmem validates this).
EMBED_METRIC = {
    "BAAI/bge-m3": "cosine",  # dense vectors are L2-normalized
}

_FAKE_DIM = 1024
_FAKE_VOCAB = 250_000


def _words(text: str):
    return re.findall(r"[a-z0-9_]+", text.lower())


class FakeEngine:
    """Deterministic bag-of-words engine (no torch, no GPU).

    Shared words → similar dense vectors and overlapping sparse indices, so
    search ranking through zvmem is meaningful in contract tests.
    """

    def __init__(self, cfg) -> None:
        self.cfg = cfg
        self.embed_model_name = "bge-m3-fake"
        self.rerank_model_name = "bge-reranker-v2-m3-fake"
        self.device_label = "fake"

    def preload(self) -> None:
        pass  # nothing to load

    @property
    def loaded(self):
        return {"embeddings": True, "reranker": True}

    @property
    def metric(self) -> str:
        return EMBED_METRIC.get(self.cfg.embed_model, "cosine")

    def _dense_vec(self, text: str):
        v = [0.0] * _FAKE_DIM
        for w in _words(text):
            h = int(hashlib.md5(w.encode()).hexdigest(), 16)
            idx = h % _FAKE_DIM
            mag = 1.0 + ((h >> 8) % 100) / 100.0
            sign = -1.0 if (h >> 20) & 1 else 1.0
            v[idx] += sign * mag
        norm = math.sqrt(sum(x * x for x in v)) or 1.0
        return [x / norm for x in v]

    def _sparse_vec(self, text: str):
        counts = Counter(_words(text))
        items = []
        for w, cnt in sorted(counts.items()):
            h = int(hashlib.md5(w.encode()).hexdigest(), 16)
            weight = math.log(1 + cnt) * (0.5 + ((h >> 4) % 100) / 200.0)
            items.append((h % _FAKE_VOCAB, weight))
        return {"indices": [i for i, _ in items], "values": [v for _, v in items]}

    def embed(self, texts):
        dense = [self._dense_vec(t) for t in texts]
        sparse = [self._sparse_vec(t) for t in texts]
        return dense, sparse

    def rerank(self, query: str, documents):
        qw = set(_words(query))
        scores = []
        for doc in documents:
            dw = set(_words(doc))
            inter = len(qw & dw)
            union = max(1, len(qw | dw))
            scores.append(round(inter / union + 0.1 * min(len(dw), 5) / 5, 4))
        return scores


class RealEngine:
    """bge-m3 + bge-reranker-v2-m3 on CUDA (or CPU fallback)."""

    def __init__(self, cfg) -> None:
        self.cfg = cfg
        self._lock = threading.Lock()
        self._embed_model = None
        self._rerank_model = None
        self._rerank_tokenizer = None
        self.embed_model_name = cfg.embed_model
        self.rerank_model_name = cfg.rerank_model

    @property
    def device_label(self) -> str:
        return self.cfg.resolved_device

    @property
    def metric(self) -> str:
        return EMBED_METRIC.get(self.cfg.embed_model, "cosine")

    @property
    def loaded(self):
        return {
            "embeddings": self._embed_model is not None,
            "reranker": self._rerank_model is not None,
        }

    # ------------------------------------------------------------- lifecycle

    def preload(self) -> None:
        """Load both models and run a tiny warmup inference so the CUDA context
        and workspaces are allocated up front — GPU memory is reserved at
        startup, not on first request."""
        with self._lock:
            if self._embed_model is None:
                try:
                    self._load_embed_model()
                except Exception as exc:
                    raise EngineError(f"failed to load embedding model: {exc}") from exc
            if self._rerank_model is None:
                try:
                    self._load_rerank_model()
                except Exception as exc:
                    raise EngineError(f"failed to load reranker model: {exc}") from exc
        # Warm up both inference paths (allocates cuDNN/cuBLAS workspaces).
        self.embed(["warmup"])
        self.rerank("warmup", ["warmup document"])

    # ------------------------------------------------------------------ embed

    def _load_embed_model(self):
        from FlagEmbedding import BGEM3FlagModel

        device = self.cfg.resolved_device
        # Max lengths are constructor params (default 512) — bge-m3 supports 8192.
        model = BGEM3FlagModel(
            self.cfg.embed_model,
            devices=[device],
            use_fp16=self.cfg.fp16 and device.startswith("cuda"),
            query_max_length=self.cfg.max_length,
            passage_max_length=self.cfg.max_length,
            return_dense=True,
            return_sparse=True,
            return_colbert_vecs=False,  # we only need dense + sparse
        )
        self._embed_model = model

    def embed(self, texts):
        with self._lock:
            if self._embed_model is None:
                try:
                    self._load_embed_model()
                except Exception as exc:  # download failure, bad path, no GPU...
                    raise EngineError(f"failed to load embedding model: {exc}") from exc

            # Return flags are set at construction time (see _load_embed_model).
            try:
                out = self._embed_model.encode(
                    texts,
                    batch_size=self.cfg.batch_size,
                    max_length=self.cfg.max_length,
                )
            except EngineError:
                raise
            except Exception as exc:
                raise EngineError(f"embedding inference failed: {exc}") from exc

        dense = [[float(x) for x in vec] for vec in out["dense_vecs"]]
        sparse = []
        # lexical_weights: list of dicts keyed by str(token_id) -> weight
        # (max-pooled across tokens, special tokens excluded).
        for sv in out["lexical_weights"]:
            items = sorted((int(k), float(v)) for k, v in sv.items())
            sparse.append(
                {"indices": [k for k, _ in items], "values": [v for _, v in items]}
            )
        return dense, sparse

    # ----------------------------------------------------------------- rerank

    def _load_rerank_model(self):
        import torch
        from transformers import AutoModelForSequenceClassification, AutoTokenizer

        device = self.cfg.resolved_device
        self._rerank_tokenizer = AutoTokenizer.from_pretrained(self.cfg.rerank_model)
        model = AutoModelForSequenceClassification.from_pretrained(
            self.cfg.rerank_model
        )
        model.to(device)
        if self.cfg.fp16 and device.startswith("cuda"):
            model.half()
        model.eval()
        self._rerank_model = model

    def rerank(self, query: str, documents):
        with self._lock:
            import torch

            if self._rerank_model is None:
                try:
                    self._load_rerank_model()
                except Exception as exc:
                    raise EngineError(f"failed to load reranker model: {exc}") from exc

            pairs = [[query, doc] for doc in documents]
            encodings = self._rerank_tokenizer(
                pairs,
                padding=True,
                truncation=True,
                max_length=self.cfg.max_length,
                return_tensors="pt",
            ).to(self.cfg.resolved_device)
            try:
                with torch.no_grad():
                    scores = (
                        self._rerank_model(**encodings).logits[:, 0].float().cpu().tolist()
                    )
            except Exception as exc:
                raise EngineError(f"rerank inference failed: {exc}") from exc
        return scores
