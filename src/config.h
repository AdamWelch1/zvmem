#pragma once

#include <string>

namespace zvmem {

struct Config {
  std::string path;             // collection directory (may start with "~")
  std::string embed_url;        // base URL of the embedding service
  std::string embed_token;      // bearer token (optional)
  int lock_timeout_ms = 10000;  // advisory write-lock wait before exit 3
  int connect_timeout_ms = 2000;
  int read_timeout_ms = 30000;  // embedding inference can be slow
  bool pretty = false;          // pretty-print JSON output

  // Defaults from environment: ZVMEM_PATH, ZVMEM_EMBED_URL, ZVMEM_EMBED_TOKEN.
  static Config from_env();

  // Expand a leading "~" to $HOME.
  std::string resolved_path() const;
};

}  // namespace zvmem
