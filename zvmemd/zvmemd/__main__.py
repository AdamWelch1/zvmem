"""Entry point: python -m zvmemd (or ./run.sh)."""

import sys

import uvicorn

from .app import create_app
from .config import Config
from .engine import EngineError, FakeEngine, RealEngine


def main() -> None:
    cfg = Config()
    engine = FakeEngine(cfg) if cfg.fake else RealEngine(cfg)

    # Eagerly load both models (and warm up inference) so GPU memory is
    # reserved at startup — fail fast rather than 503 on every request.
    print(
        f"zvmemd: loading models onto {engine.device_label} "
        f"(embed={engine.embed_model_name}, rerank={engine.rerank_model_name})...",
        flush=True,
    )
    try:
        engine.preload()
    except EngineError as exc:
        print(f"zvmemd: {exc}", file=sys.stderr, flush=True)
        sys.exit(1)

    app = create_app(engine)
    print(
        f"zvmemd: serving on http://{cfg.host}:{cfg.port} "
        f"(device={engine.device_label}, embed={engine.embed_model_name}, "
        f"rerank={engine.rerank_model_name})",
        flush=True,
    )
    uvicorn.run(app, host=cfg.host, port=cfg.port)


if __name__ == "__main__":
    main()
