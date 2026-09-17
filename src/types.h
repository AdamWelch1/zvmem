#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zvmem {

// Sparse vector: parallel index/value arrays (zvec stores uint32 indices + fp32 values).
struct SparseVector {
  std::vector<uint32_t> indices;
  std::vector<float> values;
};

// A memory as seen by the CLI — a storage-agnostic view of one zvec doc.
struct MemoryDoc {
  std::string id;          // primary key (ULID or agent-supplied slug)
  std::string summary;     // embedded search text (1-2 paragraphs written by the agent)
  std::string content;     // raw payload, FTS-indexed for keyword search
  std::vector<std::string> tags;
  int64_t created_at = 0;  // epoch millis
  int64_t updated_at = 0;  // epoch millis
  std::string model;       // embedding model id reported by the service

  // Search-result-only fields.
  float score = 0.0f;          // retrieval (fused) score
  bool has_rerank_score = false;
  float rerank_score = 0.0f;   // cross-encoder score, only when --rerank was used
};

// Field names in the zvec collection schema.
namespace field {
constexpr const char* kSummary = "summary";
constexpr const char* kContent = "content";
constexpr const char* kDense = "dense";
constexpr const char* kSparse = "sparse";
constexpr const char* kTags = "tags";
constexpr const char* kCreatedAt = "created_at";
constexpr const char* kUpdatedAt = "updated_at";
constexpr const char* kModel = "model";
}  // namespace field

// Known embedding models and the storage requirements they imply.
struct ModelProfile {
  std::string name;
  int dim = 0;            // 0 → unknown model, caller must pass --dim explicitly
  bool dense_cosine = true;  // dense metric (bge-m3 normalizes its vectors)
};

inline const ModelProfile& model_profile(const std::string& name) {
  static const ModelProfile kProfiles[] = {
      {"bge-m3", 1024, true},
  };
  for (const auto& p : kProfiles) {
    if (p.name == name) return p;
  }
  // Unknown model: zero profile — the caller must supply --dim/--dense-metric.
  static const ModelProfile kUnknown{"unknown", 0, true};
  return kUnknown;
}

// bge-m3 / bge-reranker-v2-m3 accept up to 8192 tokens; bound our payloads well
// under that (chars are a coarse proxy for tokens).
constexpr size_t kMaxEmbedInputChars = 32000;   // text sent to /v1/embeddings
constexpr size_t kMaxRerankDocChars = 8192;     // per-doc text sent to /v1/rerank

}  // namespace zvmem
