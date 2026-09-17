#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "types.h"
#include "zvec/db/collection.h"

namespace zvmem {

struct InitParams {
  int dim = 0;
  bool dense_cosine = true;        // false → IP metric on the dense field
  std::string index_type = "flat"; // "flat" | "hnsw"
};

// Thin wrapper over one zvec collection. One Store per CLI invocation:
// open, do the work, let it close (WAL + file locking handle durability).
class Store {
 public:
  static std::unique_ptr<Store> create(const std::string& path, const InitParams& p,
                                       std::string& err);
  // read_only=true for search/get/list/stats — readers never block on writers.
  static std::unique_ptr<Store> open(const std::string& path, bool read_only,
                                     std::string& err);

  int dim() const { return dim_; }
  bool dense_cosine() const { return dense_cosine_; }

  // nullopt → error (see err); true/false → answer.
  std::optional<bool> exists(const std::string& id, std::string& err) const;
  bool add(const MemoryDoc& doc, const std::vector<float>& dense,
           const SparseVector& sparse, std::string& err);
  bool get(const std::vector<std::string>& ids, std::vector<MemoryDoc>& found,
           std::vector<std::string>& not_found, std::string& err) const;
  // Upsert: replaces the whole doc (vectors included). Caller must have fetched
  // the existing doc first to preserve created_at.
  bool update(const MemoryDoc& doc, const std::vector<float>& dense,
              const SparseVector& sparse, std::string& err);
  bool remove(const std::vector<std::string>& ids, std::vector<std::string>& deleted,
              std::vector<std::string>& not_found, std::string& err);
  bool list(int limit, std::vector<MemoryDoc>& out, std::string& err) const;

  // topk = number of results to return. Callers pass a larger candidate count
  // when they plan to re-rank the results afterwards.
  bool search_dense(const std::vector<float>& qv, int topk, const std::string& filter,
                    std::vector<MemoryDoc>& out, std::string& err) const;
  bool search_sparse(const SparseVector& qs, int topk, const std::string& filter,
                     std::vector<MemoryDoc>& out, std::string& err) const;
  bool search_fts(const std::string& text, int topk, const std::string& filter,
                  std::vector<MemoryDoc>& out, std::string& err) const;
  // Hybrid: dense + sparse + FTS sub-queries fused with RRF (zvec MultiQuery).
  bool search_hybrid(const std::vector<float>& qv, const SparseVector& qs,
                     const std::string& fts_text, int topk, const std::string& filter,
                     std::vector<MemoryDoc>& out, std::string& err) const;

  // JSON object (serialized string) with doc count + schema summary.
  bool stats_json(std::string& json_out, std::string& err) const;

 private:
  Store() = default;
  zvec::Collection::Ptr coll_;
  int dim_ = 0;
  bool dense_cosine_ = true;
};

}  // namespace zvmem
