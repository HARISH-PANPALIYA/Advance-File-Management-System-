#pragma once
#include "journal.hpp"
#include <filesystem>

// ─────────────────────────────────────────────
// watch()  (mirrors Rust watch.rs)
//
// Starts a background journal thread and an
// interactive terminal menu on the calling thread.
// Returns only when the user selects "Stop".
// ─────────────────────────────────────────────
void watch(const std::filesystem::path& root,
           const std::filesystem::path& db_path);
