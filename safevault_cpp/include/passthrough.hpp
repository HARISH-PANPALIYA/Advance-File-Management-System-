#pragma once
#include "journal.hpp"
#include <filesystem>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────
// BackingStore  (mirrors Rust passthrough.rs)
//
// Journals intent *before* every mutation, then
// applies the change to the real filesystem.
// ─────────────────────────────────────────────
class BackingStore {
public:
    BackingStore(std::filesystem::path root, Journal& journal);

    // Read a relative path inside the backing root
    std::vector<uint8_t> read(const std::filesystem::path& rel) const;

    // Write (create or modify) a relative path
    void write(const std::filesystem::path& rel,
               const std::vector<uint8_t>& contents);

    // Delete a relative path
    void remove(const std::filesystem::path& rel);

    // Rename old_rel → new_rel
    void rename(const std::filesystem::path& old_rel,
                const std::filesystem::path& new_rel);

private:
    std::filesystem::path resolve(const std::filesystem::path& rel) const;

    std::filesystem::path root_;
    Journal&              journal_;
};
