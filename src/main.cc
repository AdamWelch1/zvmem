// zvmem — a memory interface for coding agents over a local zvec collection.
// See PLAN.md for the design; this file wires CLI args → embed client + store.

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "embed_client.h"
#include "exit_codes.h"
#include "store.h"
#include "ulid.h"

using njson = nlohmann::json;
using namespace zvmem;

namespace {

[[noreturn]] void die(ExitCode code, const std::string& msg) {
  std::fprintf(stderr, "error: %s\n", msg.c_str());
  std::exit(static_cast<int>(code));
}

void usage() {
  std::fputs(
R"(zvmem — memory interface for coding agents (local zvec store + remote embeddings)

Usage: zvmem [global flags] <command> [args]

Global flags:
  --path DIR            collection directory (default: $ZVMEM_PATH or ~/.zvmem/default)
  --embed-url URL       embedding service base URL ($ZVMEM_EMBED_URL)
  --embed-token TOKEN   bearer token for the embedding service ($ZVMEM_EMBED_TOKEN)
  --lock-timeout MS     how long to wait for the write lock (default 10000)
  --pretty              pretty-print JSON output

Commands:
  init      [--model bge-m3] [--index flat|hnsw] [--dim N] [--dense-metric cosine|ip]
            Create a new collection. Model profile fixes dim + metrics.
  add       --summary TEXT --content TEXT [--id SLUG] [--tags a,b,c]
            Embed the summary, store the memory, print its id (ULID unless --id).
  search    --query TEXT [-k N] [--mode hybrid|dense|sparse|fts] [--filter EXPR]
            [--content] [--rerank] [--candidates N]
            Ranked memories. Default mode: hybrid (dense+sparse+FTS, RRF fusion).
            --rerank re-scores candidates with the cross-encoder reranker.
  get       ID [ID...]                Fetch full docs by id.
  update    ID --summary TEXT [--content TEXT] [--tags a,b,c]
            Re-embeds the summary and replaces the doc's vectors + fields.
  delete    ID [ID...]                Delete memories by id.
  list      [--limit N]               Full scan (no embedding service needed).
  stats                             Collection stats + schema summary.

Output: one JSON document on stdout; terse errors on stderr.
Exit codes: 0 ok, 1 internal, 2 usage, 3 lock busy, 4 embed error, 5 not found,
            6 collection/schema mismatch.
)",
      stderr);
}

struct Args {
  std::string command;
  std::vector<std::string> positional;
  std::map<std::string, std::string> flags;   // --name value (also --name=value)
  std::set<std::string> switches;             // boolean flags
};

// Flags that take no value. Note: --content is a switch for search/list but
// takes a text value for add/update — hence the per-command set.
std::set<std::string> switch_names(const std::string& command) {
  std::set<std::string> s = {"pretty", "rerank"};
  if (command == "search" || command == "list") s.insert("content");
  return s;
}

// True for tokens that are themselves flags (so a value flag can detect a
// missing value). Values like "- done: ..." or negative numbers are NOT flags.
bool looks_like_flag(const std::string& arg) {
  if (arg == "-k" || arg == "-h") return true;
  return arg.size() > 2 && arg.rfind("--", 0) == 0;
}

// Flags that may legally appear before the command.
const std::set<std::string> kGlobalValueFlags = {
    "path", "embed-url", "embed-token", "lock-timeout"};

Args parse_args(int argc, char** argv) {
  if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
    usage();
    std::exit(argc < 2 ? static_cast<int>(ExitCode::Usage) : 0);
  }

  // Pass 1: locate the command — first token that is not a global flag (or its value).
  int cmd_idx = -1;
  bool expect_value = false;
  for (int i = 1; i < argc && cmd_idx == -1; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      usage();
      std::exit(0);
    }
    if (expect_value) {
      expect_value = false;
      continue;
    }
    if (arg.rfind("--", 0) == 0 && arg.size() > 2) {
      const std::string name = arg.substr(2);
      const auto eq = name.find('=');
      if (eq != std::string::npos) continue;  // --flag=value form
      if (name == "pretty") continue;         // global switch
      if (kGlobalValueFlags.count(name)) {
        expect_value = true;
        continue;
      }
      die(ExitCode::Usage, "unknown option before command: " + arg);
    }
    if (!arg.empty() && arg[0] == '-') {
      die(ExitCode::Usage, "unknown option before command: " + arg);
    }
    cmd_idx = i;
  }
  if (cmd_idx == -1) die(ExitCode::Usage, "missing command — run 'zvmem --help'");

  Args a;
  a.command = argv[cmd_idx];
  const auto switches = switch_names(a.command);

  // Pass 2: parse flags and positionals; global and command flags share one namespace.
  for (int i = 1; i < argc; ++i) {
    if (i == cmd_idx) continue;
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      usage();
      std::exit(0);
    }
    if (arg.rfind("--", 0) == 0 && arg.size() > 2) {
      const std::string name = arg.substr(2);
      const auto eq = name.find('=');
      if (eq != std::string::npos) {
        a.flags[name.substr(0, eq)] = name.substr(eq + 1);
      } else if (switches.count(name)) {
        a.switches.insert(name);
      } else if (i + 1 >= argc || looks_like_flag(argv[i + 1])) {
        die(ExitCode::Usage, "flag --" + name + " requires a value");
      } else {
        a.flags[name] = argv[++i];
      }
    } else if (arg == "-k") {
      if (i + 1 >= argc) die(ExitCode::Usage, "-k requires a value");
      a.flags["topk"] = argv[++i];
    } else if (!arg.empty() && arg[0] == '-') {
      die(ExitCode::Usage, "unknown option: " + arg);
    } else {
      a.positional.push_back(arg);
    }
  }
  return a;
}

const std::string& require_flag(const Args& a, const std::string& name) {
  auto it = a.flags.find(name);
  if (it == a.flags.end()) die(ExitCode::Usage, "missing required --" + name);
  return it->second;
}

std::string opt_flag(const Args& a, const std::string& name, const std::string& def) {
  auto it = a.flags.find(name);
  return it == a.flags.end() ? def : it->second;
}

int int_flag(const Args& a, const std::string& name, int def) {
  auto it = a.flags.find(name);
  if (it == a.flags.end()) return def;
  try {
    return std::stoi(it->second);
  } catch (...) {
    die(ExitCode::Usage,
        "flag --" + name + " expects an integer, got '" + it->second + "'");
  }
}

bool has_switch(const Args& a, const char* name) { return a.switches.count(name) > 0; }

std::vector<std::string> csv_split(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string truncate(const std::string& s, size_t max_chars) {
  return s.size() <= max_chars ? s : s.substr(0, max_chars);
}

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void print_json(const njson& j, bool pretty) {
  const std::string s = pretty ? j.dump(2) : j.dump();
  std::fputs(s.c_str(), stdout);
  std::fputc('\n', stdout);
}

njson doc_to_json(const MemoryDoc& m, bool include_content, bool with_scores) {
  njson j;
  j["id"] = m.id;
  if (with_scores) {
    j["score"] = m.score;
    if (m.has_rerank_score) j["rerank_score"] = m.rerank_score;
  }
  j["summary"] = m.summary;
  if (include_content) j["content"] = m.content;
  j["tags"] = m.tags;
  j["model"] = m.model;
  j["created_at"] = m.created_at;
  j["updated_at"] = m.updated_at;
  return j;
}

// Advisory exclusive lock for write commands: flock on <path>/.zvmem.lock with a
// non-blocking retry loop. Holding it before opening zvec in read-write mode
// guarantees zvec's internal write lock cannot block us (PLAN.md §9).
struct WriteLock {
  int fd = -1;
  std::string path_;

  explicit WriteLock(const std::string& dir, int timeout_ms) : path_(dir + "/.zvmem.lock") {
    fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) die(ExitCode::Internal, "cannot open lock file: " + path_);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
      if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return;
      if (std::chrono::steady_clock::now() >= deadline) {
        ::close(fd);
        fd = -1;
        die(ExitCode::LockBusy, "another process is writing to this collection — retry shortly");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  ~WriteLock() {
    if (fd >= 0) {
      ::flock(fd, LOCK_UN);
      ::close(fd);
    }
  }
};

std::string require_collection(const Config& cfg) {
  const std::string path = cfg.resolved_path();
  if (!std::filesystem::exists(path)) {
    die(ExitCode::SchemaMismatch, "no collection at " + path + " — run 'zvmem init' first");
  }
  return path;
}

void validate_embedding(const EmbedResponse& r, const Store& store) {
  if (r.dense.empty()) {
    die(ExitCode::EmbedError, "embedding service returned no dense vectors");
  }
  if (static_cast<int>(r.dense[0].size()) != store.dim()) {
    die(ExitCode::SchemaMismatch,
        "dense vector has dimension " + std::to_string(r.dense[0].size()) +
        " but the collection expects " + std::to_string(store.dim()));
  }
  if (!r.metric.empty() && r.metric != "cosine" && r.metric != "ip") {
    die(ExitCode::SchemaMismatch, "unsupported metric from embedding service: '" + r.metric + "'");
  }
  const bool want_cosine = store.dense_cosine();
  if (r.metric == "cosine" || r.metric == "ip") {
    if ((r.metric == "cosine") != want_cosine) {
      die(ExitCode::SchemaMismatch,
          "embedding service reports metric '" + r.metric + "' but the collection was initialized with '" +
          (want_cosine ? "cosine" : "ip") + "'");
    }
  }
}

int cmd_init(const Args& a, Config& cfg) {
  const std::string model = opt_flag(a, "model", "bge-m3");
  const ModelProfile& profile = model_profile(model);
  const int dim = int_flag(a, "dim", profile.dim);
  if (dim <= 0) die(ExitCode::Usage, "--dim is required for unknown model '" + model + "'");

  const std::string metric_s = opt_flag(a, "dense-metric", profile.dense_cosine ? "cosine" : "ip");
  bool cosine;
  if (metric_s == "cosine") cosine = true;
  else if (metric_s == "ip") cosine = false;
  else die(ExitCode::Usage, "--dense-metric must be 'cosine' or 'ip'");

  const std::string index = opt_flag(a, "index", "flat");
  if (index != "flat" && index != "hnsw") die(ExitCode::Usage, "--index must be 'flat' or 'hnsw'");

  const std::string path = cfg.resolved_path();
  {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
      for (auto it = std::filesystem::directory_iterator(path);
           it != std::filesystem::directory_iterator(); ++it) {
        die(ExitCode::SchemaMismatch,
            "collection already initialized at " + path +
            " — use a different --path or remove the directory");
      }
    }
  }

  InitParams p;
  p.dim = dim;
  p.dense_cosine = cosine;
  p.index_type = index;
  std::string err;
  auto store = Store::create(path, p, err);
  if (!store) die(ExitCode::Internal, "failed to create collection: " + err);

  njson out;
  out["initialized"] = path;
  out["model_profile"] = model;
  out["dim"] = dim;
  out["dense_metric"] = cosine ? "cosine" : "ip";
  out["index"] = index;
  print_json(out, cfg.pretty);
  return static_cast<int>(ExitCode::Ok);
}

int cmd_add(const Args& a, Config& cfg) {
  const std::string summary_raw = require_flag(a, "summary");
  const std::string content = require_flag(a, "content");
  const std::vector<std::string> tags = csv_split(opt_flag(a, "tags", ""));
  const bool has_id = a.flags.count("id") > 0;
  const std::string id = has_id ? a.flags.at("id") : new_ulid();

  const std::string path = require_collection(cfg);
  WriteLock lock(path, cfg.lock_timeout_ms);
  std::string err;
  auto store = Store::open(path, false /*read-write*/, err);
  if (!store) die(ExitCode::Internal, "failed to open collection: " + err);

  EmbedClient client(cfg);
  EmbedResponse emb;
  if (!client.embed({truncate(summary_raw, kMaxEmbedInputChars)}, emb, err)) {
    die(ExitCode::EmbedError, err);
  }
  validate_embedding(emb, *store);

  if (has_id) {
    auto exists = store->exists(id, err);
    if (!exists.has_value()) die(ExitCode::Internal, "existence check failed: " + err);
    if (*exists) {
      die(ExitCode::Usage,
          "memory with id '" + id + "' already exists — use 'zvmem update'");
    }
  }

  const int64_t now = now_ms();
  MemoryDoc doc;
  doc.id = id;
  doc.summary = summary_raw;   // store full text; truncation happens at embed time
  doc.content = content;
  doc.tags = tags;
  doc.created_at = now;
  doc.updated_at = now;
  doc.model = emb.model;

  if (!store->add(doc, emb.dense[0], emb.sparse[0], err)) {
    die(ExitCode::Internal, "insert failed: " + err);
  }

  njson out;
  out["id"] = id;
  out["created_at"] = now;
  print_json(out, cfg.pretty);
  return static_cast<int>(ExitCode::Ok);
}

int cmd_search(const Args& a, Config& cfg) {
  const std::string query_raw = require_flag(a, "query");
  int k = int_flag(a, "topk", 10);
  if (k <= 0) die(ExitCode::Usage, "-k must be > 0");
  const std::string mode = opt_flag(a, "mode", "hybrid");
  if (mode != "hybrid" && mode != "dense" && mode != "sparse" && mode != "fts") {
    die(ExitCode::Usage, "--mode must be one of: hybrid, dense, sparse, fts");
  }
  const std::string filter = opt_flag(a, "filter", "");
  const bool include_content = has_switch(a, "content");
  const bool rerank = has_switch(a, "rerank");
  const int candidates = int_flag(a, "candidates", std::max(3 * k, 50));

  const std::string path = require_collection(cfg);
  std::string err;
  auto store = Store::open(path, true /*read-only*/, err);
  if (!store) die(ExitCode::Internal, "failed to open collection: " + err);

  const std::string query = truncate(query_raw, kMaxEmbedInputChars);
  EmbedClient client(cfg);
  EmbedResponse emb;
  if (mode != "fts") {
    if (!client.embed({query}, emb, err)) die(ExitCode::EmbedError, err);
    validate_embedding(emb, *store);
  }

  std::vector<MemoryDoc> results;
  const int topk_for_db = rerank ? candidates : k;
  bool ok = false;
  if (mode == "dense") {
    ok = store->search_dense(emb.dense[0], topk_for_db, filter, results, err);
  } else if (mode == "sparse") {
    ok = store->search_sparse(emb.sparse[0], topk_for_db, filter, results, err);
  } else if (mode == "fts") {
    ok = store->search_fts(query, topk_for_db, filter, results, err);
  } else {
    ok = store->search_hybrid(emb.dense[0], emb.sparse[0], query, topk_for_db, filter,
                              results, err);
  }
  if (!ok) die(ExitCode::Internal, "search failed: " + err);

  if (rerank && !results.empty()) {
    std::vector<std::string> texts;
    texts.reserve(results.size());
    for (const auto& m : results) {
      std::string t = m.summary;
      if (!m.content.empty()) {
        if (!t.empty()) t += "\n\n";
        t += m.content;
      }
      texts.push_back(truncate(t, kMaxRerankDocChars));
    }
    std::vector<float> scores;
    if (!client.rerank(query_raw, texts, scores, err)) {
      die(ExitCode::EmbedError, "rerank failed: " + err);
    }
    std::vector<size_t> order(results.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t x, size_t y) { return scores[x] > scores[y]; });
    std::vector<MemoryDoc> reranked;
    for (size_t i = 0; i < order.size() && static_cast<int>(i) < k; ++i) {
      MemoryDoc m = std::move(results[order[i]]);
      m.has_rerank_score = true;
      m.rerank_score = scores[order[i]];
      reranked.push_back(std::move(m));
    }
    results = std::move(reranked);
  }

  njson arr = njson::array();
  for (const auto& m : results) arr.push_back(doc_to_json(m, include_content, true));
  njson out;
  out["results"] = arr;
  print_json(out, cfg.pretty);
  return static_cast<int>(ExitCode::Ok);
}

int cmd_get(const Args& a, Config& cfg) {
  if (a.positional.empty()) die(ExitCode::Usage, "usage: zvmem get ID [ID...]");
  const std::string path = require_collection(cfg);
  std::string err;
  auto store = Store::open(path, true /*read-only*/, err);
  if (!store) die(ExitCode::Internal, "failed to open collection: " + err);

  std::vector<MemoryDoc> found;
  std::vector<std::string> not_found;
  if (!store->get(a.positional, found, not_found, err)) {
    die(ExitCode::Internal, "fetch failed: " + err);
  }

  njson arr = njson::array();
  for (const auto& m : found) arr.push_back(doc_to_json(m, true /*content*/, false));
  njson out;
  out["docs"] = arr;
  out["not_found"] = not_found;
  print_json(out, cfg.pretty);
  return found.empty() ? static_cast<int>(ExitCode::NotFound)
                       : static_cast<int>(ExitCode::Ok);
}

int cmd_update(const Args& a, Config& cfg) {
  if (a.positional.size() != 1) die(ExitCode::Usage, "usage: zvmem update ID --summary TEXT [--content TEXT] [--tags a,b,c]");
  const std::string id = a.positional[0];
  const std::string new_summary_raw = require_flag(a, "summary");
  const bool has_content = a.flags.count("content") > 0;
  const bool has_tags = a.flags.count("tags") > 0;

  const std::string path = require_collection(cfg);
  WriteLock lock(path, cfg.lock_timeout_ms);
  std::string err;
  auto store = Store::open(path, false /*read-write*/, err);
  if (!store) die(ExitCode::Internal, "failed to open collection: " + err);

  std::vector<MemoryDoc> found;
  std::vector<std::string> not_found;
  if (!store->get({id}, found, not_found, err)) {
    die(ExitCode::Internal, "fetch failed: " + err);
  }
  if (found.empty()) die(ExitCode::NotFound, "no memory with id '" + id + "'");

  EmbedClient client(cfg);
  EmbedResponse emb;
  if (!client.embed({truncate(new_summary_raw, kMaxEmbedInputChars)}, emb, err)) {
    die(ExitCode::EmbedError, err);
  }
  validate_embedding(emb, *store);

  MemoryDoc doc = found[0];   // preserve created_at and any untouched fields
  doc.summary = new_summary_raw;
  if (has_content) doc.content = a.flags.at("content");
  if (has_tags) doc.tags = csv_split(a.flags.at("tags"));
  doc.updated_at = now_ms();
  doc.model = emb.model;

  if (!store->update(doc, emb.dense[0], emb.sparse[0], err)) {
    die(ExitCode::Internal, "update failed: " + err);
  }

  njson out;
  out["id"] = id;
  out["updated_at"] = doc.updated_at;
  print_json(out, cfg.pretty);
  return static_cast<int>(ExitCode::Ok);
}

int cmd_delete(const Args& a, Config& cfg) {
  if (a.positional.empty()) die(ExitCode::Usage, "usage: zvmem delete ID [ID...]");
  const std::string path = require_collection(cfg);
  WriteLock lock(path, cfg.lock_timeout_ms);
  std::string err;
  auto store = Store::open(path, false /*read-write*/, err);
  if (!store) die(ExitCode::Internal, "failed to open collection: " + err);

  std::vector<std::string> deleted;
  std::vector<std::string> not_found;
  if (!store->remove(a.positional, deleted, not_found, err)) {
    die(ExitCode::Internal, "delete failed: " + err);
  }

  njson out;
  out["deleted"] = deleted;
  out["not_found"] = not_found;
  print_json(out, cfg.pretty);
  return deleted.empty() ? static_cast<int>(ExitCode::NotFound)
                         : static_cast<int>(ExitCode::Ok);
}

int cmd_list(const Args& a, Config& cfg) {
  int limit = int_flag(a, "limit", 100);
  if (limit <= 0) die(ExitCode::Usage, "--limit must be > 0");
  const bool include_content = has_switch(a, "content");

  const std::string path = require_collection(cfg);
  std::string err;
  auto store = Store::open(path, true /*read-only*/, err);
  if (!store) die(ExitCode::Internal, "failed to open collection: " + err);

  std::vector<MemoryDoc> docs;
  if (!store->list(limit, docs, err)) die(ExitCode::Internal, "list failed: " + err);

  njson arr = njson::array();
  for (const auto& m : docs) arr.push_back(doc_to_json(m, include_content, false));
  njson out;
  out["docs"] = arr;
  out["count"] = static_cast<int>(docs.size());
  print_json(out, cfg.pretty);
  return static_cast<int>(ExitCode::Ok);
}

int cmd_stats(const Args& a, Config& cfg) {
  const std::string path = require_collection(cfg);
  std::string err;
  auto store = Store::open(path, true /*read-only*/, err);
  if (!store) die(ExitCode::Internal, "failed to open collection: " + err);

  std::string json;
  if (!store->stats_json(json, err)) die(ExitCode::Internal, "stats failed: " + err);
  njson out = njson::parse(json);
  out["path"] = path;
  print_json(out, cfg.pretty);
  return static_cast<int>(ExitCode::Ok);
}

}  // namespace

int main(int argc, char** argv) {
  Args a = parse_args(argc, argv);

  Config cfg = Config::from_env();
  if (a.flags.count("path")) cfg.path = a.flags.at("path");
  if (a.flags.count("embed-url")) cfg.embed_url = a.flags.at("embed-url");
  if (a.flags.count("embed-token")) cfg.embed_token = a.flags.at("embed-token");
  if (a.flags.count("lock-timeout")) {
    cfg.lock_timeout_ms = int_flag(a, "lock-timeout", cfg.lock_timeout_ms);
  }
  if (has_switch(a, "pretty")) cfg.pretty = true;
  if (cfg.path.empty()) cfg.path = "~/.zvmem/default";

  const std::string& cmd = a.command;
  if (cmd == "init") return cmd_init(a, cfg);
  if (cmd == "add") return cmd_add(a, cfg);
  if (cmd == "search") return cmd_search(a, cfg);
  if (cmd == "get") return cmd_get(a, cfg);
  if (cmd == "update") return cmd_update(a, cfg);
  if (cmd == "delete") return cmd_delete(a, cfg);
  if (cmd == "list") return cmd_list(a, cfg);
  if (cmd == "stats") return cmd_stats(a, cfg);

  die(ExitCode::Usage, "unknown command '" + cmd + "' — run 'zvmem --help'");
}
