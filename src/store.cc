#include "store.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace zvmem {

using njson = nlohmann::json;

namespace {

zvec::MetricType dense_metric(bool cosine) {
  return cosine ? zvec::MetricType::COSINE : zvec::MetricType::IP;
}

std::string metric_name(zvec::MetricType m) {
  switch (m) {
    case zvec::MetricType::L2: return "l2";
    case zvec::MetricType::IP: return "ip";
    case zvec::MetricType::COSINE: return "cosine";
    default: return "undefined";
  }
}

zvec::Doc to_zvec_doc(const MemoryDoc& doc, const std::vector<float>& dense,
                      const SparseVector& sparse) {
  zvec::Doc d;
  d.set_pk(doc.id);
  d.set<std::string>(field::kSummary, doc.summary);
  d.set<std::string>(field::kContent, doc.content);
  d.set<std::vector<float>>(field::kDense, dense);
  d.set<std::pair<std::vector<uint32_t>, std::vector<float>>>(
      field::kSparse, {sparse.indices, sparse.values});
  d.set<int64_t>(field::kCreatedAt, doc.created_at);
  d.set<int64_t>(field::kUpdatedAt, doc.updated_at);
  d.set<std::string>(field::kModel, doc.model);
  d.set<std::vector<std::string>>(field::kTags, doc.tags);  // may be empty
  return d;
}

MemoryDoc from_zvec_doc(const zvec::Doc& d) {
  MemoryDoc m;
  m.id = d.pk();
  if (auto v = d.get<std::string>(field::kSummary)) m.summary = *v;
  if (auto v = d.get<std::string>(field::kContent)) m.content = *v;
  if (auto v = d.get<int64_t>(field::kCreatedAt)) m.created_at = *v;
  if (auto v = d.get<int64_t>(field::kUpdatedAt)) m.updated_at = *v;
  if (auto v = d.get<std::string>(field::kModel)) m.model = *v;
  if (auto v = d.get<std::vector<std::string>>(field::kTags)) m.tags = *v;
  return m;
}

void append_hits(const zvec::DocPtrList& results, std::vector<MemoryDoc>& out) {
  for (const auto& doc : results) {
    if (!doc) continue;  // defensive: skip null entries
    MemoryDoc m = from_zvec_doc(*doc);
    m.score = doc->score();
    out.push_back(std::move(m));
  }
}

std::string pack_dense(const std::vector<float>& v) {
  return std::string(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float));
}

std::string pack_u32(const std::vector<uint32_t>& v) {
  return std::string(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(uint32_t));
}

// zvec expects sparse indices sorted ascending; sort (index, value) pairs together.
void sort_sparse(SparseVector& sv) {
  std::vector<std::pair<uint32_t, float>> items;
  items.reserve(sv.indices.size());
  for (size_t i = 0; i < sv.indices.size(); ++i) {
    items.emplace_back(sv.indices[i], sv.values[i]);
  }
  std::sort(items.begin(), items.end());
  sv.indices.clear();
  sv.values.clear();
  for (auto& [k, v] : items) {
    sv.indices.push_back(k);
    sv.values.push_back(v);
  }
}

zvec::CollectionSchema make_schema(const InitParams& p) {
  zvec::CollectionSchema schema("memories");

  // summary: plain stored text (the embedded search text).
  schema.add_field(std::make_shared<zvec::FieldSchema>(field::kSummary,
                                                       zvec::DataType::STRING));

  // content: raw payload with an FTS index for BM25 keyword search. Indexed
  // scalar fields are also forward (stored) fields in zvec — the text is
  // returned by fetch/query, so no duplicate field is needed.
  auto content = std::make_shared<zvec::FieldSchema>(field::kContent,
                                                     zvec::DataType::STRING);
  content->set_index_params(std::make_shared<zvec::FtsIndexParams>());
  schema.add_field(content);

  // dense: FLAT (exact) or HNSW per init config; metric from the model profile.
  zvec::IndexParams::Ptr dense_idx;
  if (p.index_type == "hnsw") {
    dense_idx = std::make_shared<zvec::HnswIndexParams>(dense_metric(p.dense_cosine));
  } else {
    dense_idx = std::make_shared<zvec::FlatIndexParams>(dense_metric(p.dense_cosine));
  }
  schema.add_field(std::make_shared<zvec::FieldSchema>(field::kDense,
                                                       zvec::DataType::VECTOR_FP32,
                                                       p.dim, false, dense_idx));

  // sparse: always IP (zvec enforces this on all sparse fields); FLAT for exactness.
  schema.add_field(std::make_shared<zvec::FieldSchema>(
      field::kSparse, zvec::DataType::SPARSE_VECTOR_FP32, 0, false,
      std::make_shared<zvec::FlatIndexParams>(zvec::MetricType::IP)));

  // timestamps: inverted-indexed for range filters.
  auto created = std::make_shared<zvec::FieldSchema>(field::kCreatedAt,
                                                     zvec::DataType::INT64);
  created->set_index_params(std::make_shared<zvec::InvertIndexParams>());
  schema.add_field(created);
  auto updated = std::make_shared<zvec::FieldSchema>(field::kUpdatedAt,
                                                     zvec::DataType::INT64);
  updated->set_index_params(std::make_shared<zvec::InvertIndexParams>());
  schema.add_field(updated);

  // tags: inverted-indexed array for filtered search.
  auto tags = std::make_shared<zvec::FieldSchema>(field::kTags,
                                                  zvec::DataType::ARRAY_STRING, true);
  tags->set_index_params(std::make_shared<zvec::InvertIndexParams>());
  schema.add_field(tags);

  // model: provenance (which embedding model produced the vectors).
  schema.add_field(std::make_shared<zvec::FieldSchema>(field::kModel,
                                                       zvec::DataType::STRING));
  return schema;
}

bool check_write_results(const zvec::WriteResults& results, std::string& err) {
  for (const auto& s : results) {
    if (!s.ok()) {
      err = s.message();
      return false;
    }
  }
  return true;
}

}  // namespace

std::unique_ptr<Store> Store::create(const std::string& path, const InitParams& p,
                                     std::string& err) {
  if (p.dim <= 0) {
    err = "dim must be > 0";
    return nullptr;
  }
  auto schema = make_schema(p);
  zvec::CollectionOptions options;  // read-write, mmap enabled
  auto result = zvec::Collection::CreateAndOpen(path, schema, options);
  if (!result.has_value()) {
    err = result.error().message();
    return nullptr;
  }
  auto store = std::unique_ptr<Store>(new Store());
  store->coll_ = std::move(result).value();
  store->dim_ = p.dim;
  store->dense_cosine_ = p.dense_cosine;
  return store;
}

std::unique_ptr<Store> Store::open(const std::string& path, bool read_only,
                                   std::string& err) {
  zvec::CollectionOptions options;
  options.read_only_ = read_only;
  auto result = zvec::Collection::Open(path, options);
  if (!result.has_value()) {
    err = result.error().message();
    return nullptr;
  }
  auto store = std::unique_ptr<Store>(new Store());
  store->coll_ = std::move(result).value();

  // Read dim + dense metric back from the persisted schema (single source of truth).
  if (auto schema_res = store->coll_->schema(); schema_res.has_value()) {
    const auto schema = std::move(schema_res).value();
    if (const zvec::FieldSchema* f = schema.get_field(field::kDense)) {
      store->dim_ = static_cast<int>(f->dimension());
      if (auto ip = std::dynamic_pointer_cast<zvec::VectorIndexParams>(
              f->index_params())) {
        store->dense_cosine_ = ip->metric_type() == zvec::MetricType::COSINE;
      }
    }
  }
  return store;
}

// Note: zvec's C++ fetch returns an entry with a NULL doc pointer for missing
// ids (the Python binding omits them instead), so "present" must mean non-null.
std::optional<bool> Store::exists(const std::string& id, std::string& err) const {
  auto res = coll_->fetch({id}, std::nullopt, false);
  if (!res.has_value()) {
    err = res.error().message();
    return std::nullopt;
  }
  auto it = res.value().find(id);
  return it != res.value().end() && it->second != nullptr;
}

bool Store::add(const MemoryDoc& doc, const std::vector<float>& dense,
                const SparseVector& sparse, std::string& err) {
  if (static_cast<int>(dense.size()) != dim_) {
    err = "dense vector has dimension " + std::to_string(dense.size()) +
          " but the collection expects " + std::to_string(dim_);
    return false;
  }
  zvec::Doc d = to_zvec_doc(doc, dense, sparse);
  std::vector<zvec::Doc> docs{d};
  auto res = coll_->insert(docs);
  if (!res.has_value()) {
    err = res.error().message();
    return false;
  }
  return check_write_results(res.value(), err);
}

bool Store::get(const std::vector<std::string>& ids, std::vector<MemoryDoc>& found,
                std::vector<std::string>& not_found, std::string& err) const {
  auto res = coll_->fetch(ids, std::nullopt, false);
  if (!res.has_value()) {
    err = res.error().message();
    return false;
  }
  auto map = std::move(res).value();
  for (const auto& id : ids) {
    auto it = map.find(id);
    if (it == map.end() || it->second == nullptr) not_found.push_back(id);
    else found.push_back(from_zvec_doc(*it->second));
  }
  return true;
}

bool Store::update(const MemoryDoc& doc, const std::vector<float>& dense,
                   const SparseVector& sparse, std::string& err) {
  if (static_cast<int>(dense.size()) != dim_) {
    err = "dense vector has dimension " + std::to_string(dense.size()) +
          " but the collection expects " + std::to_string(dim_);
    return false;
  }
  zvec::Doc d = to_zvec_doc(doc, dense, sparse);
  std::vector<zvec::Doc> docs{d};
  auto res = coll_->upsert(docs);
  if (!res.has_value()) {
    err = res.error().message();
    return false;
  }
  return check_write_results(res.value(), err);
}

bool Store::remove(const std::vector<std::string>& ids,
                   std::vector<std::string>& deleted,
                   std::vector<std::string>& not_found, std::string& err) {
  // Determine which ids exist first so the report is accurate.
  std::vector<MemoryDoc> found;
  if (!get(ids, found, not_found, err)) return false;
  if (found.empty()) return true;  // nothing to delete; not_found already populated

  std::vector<std::string> pks;
  pks.reserve(found.size());
  for (const auto& m : found) pks.push_back(m.id);
  auto res = coll_->delete_(pks);
  if (!res.has_value()) {
    err = res.error().message();
    return false;
  }
  if (!check_write_results(res.value(), err)) return false;
  deleted = std::move(pks);
  return true;
}

bool Store::list(int limit, std::vector<MemoryDoc>& out, std::string& err) const {
  zvec::IteratorOptions io;
  io.include_vector_ = false;  // all fields, no vectors — keeps payloads small
  auto it_res = coll_->create_iterator(io);
  if (!it_res.has_value()) {
    err = it_res.error().message();
    return false;
  }
  auto iter = std::move(it_res).value();
  while (static_cast<int>(out.size()) < limit) {
    auto next = iter->next();
    if (!next.has_value()) {
      err = next.error().message();
      break;
    }
    if (!next.value()) break;  // EOF
    out.push_back(from_zvec_doc(*next.value()));
  }
  iter->close();
  return true;
}

bool Store::search_dense(const std::vector<float>& qv, int topk,
                         const std::string& filter, std::vector<MemoryDoc>& out,
                         std::string& err) const {
  zvec::SearchQuery q;
  q.target_.field_name_ = field::kDense;
  q.topk_ = topk;
  q.filter_ = filter;
  q.include_vector_ = false;
  q.target_.set_vector(pack_dense(qv));
  auto res = coll_->query(q);
  if (!res.has_value()) {
    err = res.error().message();
    return false;
  }
  append_hits(res.value(), out);
  return true;
}

bool Store::search_sparse(const SparseVector& qs_in, int topk, const std::string& filter,
                          std::vector<MemoryDoc>& out, std::string& err) const {
  SparseVector qs = qs_in;
  sort_sparse(qs);
  zvec::SearchQuery q;
  q.target_.field_name_ = field::kSparse;
  q.topk_ = topk;
  q.filter_ = filter;
  q.include_vector_ = false;
  q.target_.set_sparse_vector(pack_u32(qs.indices), pack_dense(qs.values));
  auto res = coll_->query(q);
  if (!res.has_value()) {
    err = res.error().message();
    return false;
  }
  append_hits(res.value(), out);
  return true;
}

bool Store::search_fts(const std::string& text, int topk, const std::string& filter,
                       std::vector<MemoryDoc>& out, std::string& err) const {
  zvec::SearchQuery q;
  q.target_.field_name_ = field::kContent;
  q.topk_ = topk;
  q.filter_ = filter;
  q.include_vector_ = false;
  zvec::FtsClause fts;
  fts.query_string_ = text;
  q.target_.clause_ = fts;
  auto res = coll_->query(q);
  if (!res.has_value()) {
    err = res.error().message();
    return false;
  }
  append_hits(res.value(), out);
  return true;
}

bool Store::search_hybrid(const std::vector<float>& qv, const SparseVector& qs_in,
                          const std::string& fts_text, int topk,
                          const std::string& filter, std::vector<MemoryDoc>& out,
                          std::string& err) const {
  zvec::MultiQuery mq;
  mq.topk = topk;
  mq.filter = filter;
  mq.include_vector = false;
  // RRF fusion (default k=60) merges the three sub-query rankings.
  mq.rerank = zvec::reranker::RrfParams{60};

  {
    zvec::SubQuery sq;
    sq.num_candidates_ = topk;
    sq.target_.field_name_ = field::kDense;
    sq.target_.set_vector(pack_dense(qv));
    mq.queries.push_back(sq);
  }
  {
    SparseVector qs = qs_in;
    sort_sparse(qs);
    zvec::SubQuery sq;
    sq.num_candidates_ = topk;
    sq.target_.field_name_ = field::kSparse;
    sq.target_.set_sparse_vector(pack_u32(qs.indices), pack_dense(qs.values));
    mq.queries.push_back(sq);
  }
  {
    zvec::SubQuery sq;
    sq.num_candidates_ = topk;
    sq.target_.field_name_ = field::kContent;
    zvec::FtsClause fts;
    fts.query_string_ = fts_text;
    sq.target_.clause_ = fts;
    mq.queries.push_back(sq);
  }

  auto res = coll_->query(mq);
  if (!res.has_value()) {
    err = res.error().message();
    return false;
  }
  append_hits(res.value(), out);
  return true;
}

bool Store::stats_json(std::string& json_out, std::string& err) const {
  auto res = coll_->stats();
  if (!res.has_value()) {
    err = res.error().message();
    return false;
  }
  const auto s = std::move(res).value();
  njson j;
  j["doc_count"] = s.doc_count;
  j["dim"] = dim_;
  j["dense_metric"] = metric_name(dense_cosine_ ? zvec::MetricType::COSINE
                                                : zvec::MetricType::IP);
  njson completeness = njson::object();
  for (const auto& [k, v] : s.index_completeness) completeness[k] = v;
  j["index_completeness"] = completeness;
  json_out = j.dump();
  return true;
}

}  // namespace zvmem
