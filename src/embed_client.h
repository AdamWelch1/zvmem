#pragma once

#include <string>
#include <vector>

#include "config.h"
#include "types.h"

namespace zvmem {

struct EmbedResponse {
  std::string model;   // resolved model id (stored per doc for provenance)
  std::string metric;  // "cosine" | "ip" — the model's training metric
  std::vector<std::vector<float>> dense;  // parallel to input texts
  std::vector<SparseVector> sparse;       // parallel to input texts
};

// Thin HTTP+JSON client for the remote embedding service (PLAN.md §5).
class EmbedClient {
 public:
  explicit EmbedClient(const Config& cfg) : cfg_(cfg) {}

  // POST /v1/embeddings. Returns false and sets err on transport/HTTP/parse errors.
  bool embed(const std::vector<std::string>& texts, EmbedResponse& out,
             std::string& err);

  // POST /v1/rerank. On success scores is parallel to documents.
  bool rerank(const std::string& query, const std::vector<std::string>& documents,
              std::vector<float>& scores, std::string& err);

 private:
  bool post(const std::string& path, const std::string& body_json,
            std::string& resp_body, std::string& err) const;

  const Config& cfg_;
};

}  // namespace zvmem
