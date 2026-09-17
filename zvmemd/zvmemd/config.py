"""Configuration for zvmemd, sourced from environment variables."""

import os


def _env_str(name: str, default: str) -> str:
    value = os.environ.get(name)
    return value if value not in (None, "") else default


def _env_int(name: str, default: int) -> int:
    raw = os.environ.get(name)
    if raw is None or raw == "":
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise SystemExit(f"zvmemd: {name} must be an integer, got '{raw}'") from exc


def _env_bool(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None or raw == "":
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


class Config:
    def __init__(self) -> None:
        self.host = _env_str("ZVMEMD_HOST", "0.0.0.0")
        self.port = _env_int("ZVMEMD_PORT", 8080)

        # "auto" (default): cuda if available, else cpu.
        # Force with "cuda", "cpu", or a specific GPU: "cuda:0", "cuda:1", ...
        self.device = _env_str("ZVMEMD_DEVICE", "auto").lower()

        # Model repos — may be HF repo ids or local directories (pre-downloaded).
        self.embed_model = _env_str("ZVMEMD_EMBED_MODEL", "BAAI/bge-m3")
        self.rerank_model = _env_str("ZVMEMD_RERANK_MODEL", "BAAI/bge-reranker-v2-m3")

        # bge-m3 / bge-reranker-v2-m3 both accept up to 8192 tokens.
        self.max_length = _env_int("ZVMEMD_MAX_LENGTH", 8192)
        self.batch_size = _env_int("ZVMEMD_BATCH_SIZE", 16)

        # fp16 inference (only used when running on cuda).
        self.fp16 = _env_bool("ZVMEMD_FP16", True)

        # Test hook: deterministic fake models, no GPU/torch needed.
        self.fake = _env_bool("ZVMEMD_FAKE", False)

    @property
    def resolved_device(self) -> str:
        if self.device != "auto":
            return self.device  # "cuda", "cpu", or "cuda:N"
        import torch  # deferred: only needed when resolving "auto" on a real engine

        return "cuda" if torch.cuda.is_available() else "cpu"
