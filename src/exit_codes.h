#pragma once

namespace zvmem {

// Stable exit codes (PLAN.md §6). Never change these without a major version bump.
enum class ExitCode : int {
  Ok = 0,             // success (including partial delete with not_found reported)
  Internal = 1,       // unexpected/internal error
  Usage = 2,          // usage/argument error; id collision on `add --id`
  LockBusy = 3,       // write lock busy past timeout
  EmbedError = 4,     // embedding service unreachable or returned an error
  NotFound = 5,       // requested id(s) not found (get/update/delete-all-missing)
  SchemaMismatch = 6, // collection missing, or dim/metric/index mismatch
};

}  // namespace zvmem
