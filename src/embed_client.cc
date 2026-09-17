#include "embed_client.h"

#include <curl/curl.h>

#include <nlohmann/json.hpp>

namespace zvmem {

namespace {

size_t write_to_string(char* ptr, size_t size, size_t nmemb, std::string* out) {
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string truncate_for_log(const std::string& s, size_t n = 300) {
  if (s.size() <= n) return s;
  return s.substr(0, n) + "...";
}

}  // namespace

bool EmbedClient::post(const std::string& path, const std::string& body_json,
                       std::string& resp_body, std::string& err) const {
  if (cfg_.embed_url.empty()) {
    err = "no embedding service URL configured (--embed-url or $ZVMEM_EMBED_URL)";
    return false;
  }
  const std::string url = cfg_.embed_url + path;

  CURL* curl = curl_easy_init();
  if (!curl) {
    err = "curl initialization failed";
    return false;
  }

  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (!cfg_.embed_token.empty()) {
    headers = curl_slist_append(
        headers, ("Authorization: Bearer " + cfg_.embed_token).c_str());
  }

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body_json.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, cfg_.connect_timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg_.read_timeout_ms);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    err = "request to " + url + " failed: " + curl_easy_strerror(rc);
    return false;
  }
  if (http_code < 200 || http_code >= 300) {
    err = "embedding service returned HTTP " + std::to_string(http_code) + ": " +
          truncate_for_log(response);
    return false;
  }
  resp_body = std::move(response);
  return true;
}

bool EmbedClient::embed(const std::vector<std::string>& texts, EmbedResponse& out,
                        std::string& err) {
  nlohmann::json req = {{"texts", texts}};
  std::string body;
  if (!post("/v1/embeddings", req.dump(), body, err)) return false;

  try {
    auto j = nlohmann::json::parse(body);
    out.model = j.value("model", std::string());
    out.metric = j.value("metric", std::string());
    if (j.contains("dense") && j["dense"].is_array()) {
      for (const auto& v : j["dense"]) {
        std::vector<float> vec;
        vec.reserve(v.size());
        for (const auto& x : v) vec.push_back(x.get<float>());
        out.dense.push_back(std::move(vec));
      }
    }
    if (j.contains("sparse") && j["sparse"].is_array()) {
      for (const auto& item : j["sparse"]) {
        SparseVector sv;
        const auto indices = item.value("indices", nlohmann::json::array());
        const auto values = item.value("values", nlohmann::json::array());
        for (const auto& x : indices) sv.indices.push_back(x.get<uint32_t>());
        for (const auto& x : values) sv.values.push_back(x.get<float>());
        if (sv.indices.size() != sv.values.size()) {
          err = "sparse vector has mismatched indices/values lengths";
          return false;
        }
        out.sparse.push_back(std::move(sv));
      }
    }
  } catch (const std::exception& e) {
    err = std::string("failed to parse embedding response: ") + e.what();
    return false;
  }

  if (out.dense.size() != texts.size() || out.sparse.size() != texts.size()) {
    err = "embedding service returned a different number of vectors than texts";
    return false;
  }
  return true;
}

bool EmbedClient::rerank(const std::string& query,
                         const std::vector<std::string>& documents,
                         std::vector<float>& scores, std::string& err) {
  nlohmann::json req = {{"query", query}, {"documents", documents}};
  std::string body;
  if (!post("/v1/rerank", req.dump(), body, err)) return false;

  try {
    auto j = nlohmann::json::parse(body);
    if (!j.contains("scores") || !j["scores"].is_array()) {
      err = "rerank response is missing the 'scores' array";
      return false;
    }
    scores.clear();
    for (const auto& x : j["scores"]) scores.push_back(x.get<float>());
  } catch (const std::exception& e) {
    err = std::string("failed to parse rerank response: ") + e.what();
    return false;
  }

  if (scores.size() != documents.size()) {
    err = "rerank service returned a different number of scores than documents";
    return false;
  }
  return true;
}

}  // namespace zvmem
