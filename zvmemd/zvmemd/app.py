"""FastAPI app implementing the zvmem embedding-service contract (PLAN.md §5)."""

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

from .engine import EngineError

# Keep request payloads bounded; zvmem sends one text per add/update and one
# query + N candidates per search.
MAX_TEXTS_PER_REQUEST = 64


class EmbedRequest(BaseModel):
    texts: list[str] = Field(min_length=1, max_length=MAX_TEXTS_PER_REQUEST)


class RerankRequest(BaseModel):
    query: str = Field(min_length=1)
    documents: list[str] = Field(min_length=1, max_length=MAX_TEXTS_PER_REQUEST)


def create_app(engine) -> FastAPI:
    app = FastAPI(title="zvmemd", version="0.1.0")

    @app.get("/health")
    def health():
        return {
            "status": "ok",
            "embed_model": engine.embed_model_name,
            "rerank_model": engine.rerank_model_name,
            "device": engine.device_label,
            "models_loaded": engine.loaded,
        }

    @app.post("/v1/embeddings")
    def embeddings(req: EmbedRequest):
        try:
            dense, sparse = engine.embed(req.texts)
        except EngineError as exc:
            raise HTTPException(status_code=503, detail=str(exc)) from exc
        return {
            "model": engine.embed_model_name,
            "metric": engine.metric,
            "dense": dense,
            "sparse": sparse,
        }

    @app.post("/v1/rerank")
    def rerank(req: RerankRequest):
        try:
            scores = engine.rerank(req.query, req.documents)
        except EngineError as exc:
            raise HTTPException(status_code=503, detail=str(exc)) from exc
        return {
            "model": engine.rerank_model_name,
            "scores": scores,
        }

    return app
