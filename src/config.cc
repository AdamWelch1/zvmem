#include "config.h"

#include <cstdlib>

namespace zvmem {

Config Config::from_env() {
  Config cfg;
  if (const char* p = std::getenv("ZVMEM_PATH")) cfg.path = p;
  if (const char* u = std::getenv("ZVMEM_EMBED_URL")) cfg.embed_url = u;
  if (const char* t = std::getenv("ZVMEM_EMBED_TOKEN")) cfg.embed_token = t;
  return cfg;
}

std::string Config::resolved_path() const {
  if (!path.empty() && path[0] == '~') {
    const char* home = std::getenv("HOME");
    std::string base = (home && *home) ? home : ".";
    return base + path.substr(1);
  }
  return path;
}

}  // namespace zvmem
