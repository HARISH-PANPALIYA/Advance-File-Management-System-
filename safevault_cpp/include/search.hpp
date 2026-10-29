#pragma once
#include <filesystem>
#include <string>
#include <vector>

// ─────────────────────────────────────────────
// SearchSchema  (mirrors Rust SearchSchema)
// ─────────────────────────────────────────────
struct SearchSchema {
    std::vector<std::string> keywords;
    std::vector<std::string> extensions;
    std::string              intent;
};

// ─────────────────────────────────────────────
// SearchResult  (one matching file)
// ─────────────────────────────────────────────
struct SearchResult {
    std::filesystem::path path;
    std::string           file_name;
    std::string           extension; // empty if none
    uint64_t              size_bytes{0};
};

// ─────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────

// Parse a plain-text query into a schema (local, no AI)
SearchSchema parse_query(const std::string& query);

// Call Groq API for schema extraction; throws on failure
SearchSchema parse_with_groq(const std::string& query,
                             const std::string& api_key);

// Walk root recursively and return files matching schema
std::vector<SearchResult> search_files(const std::filesystem::path& root,
                                       const SearchSchema& schema);
