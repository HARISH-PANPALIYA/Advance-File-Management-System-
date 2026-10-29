#include "search.hpp"
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <winhttp.h>
#  pragma comment(lib, "winhttp.lib")
#else
// On non-Windows use cpp-httplib for HTTPS (requires OpenSSL)
// #  define CPPHTTPLIB_OPENSSL_SUPPORT
#  include <httplib.h>
#endif

// ─────────────────────────────────────────────
// String utilities
// ─────────────────────────────────────────────

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool str_contains_ci(const std::string& haystack, const std::string& needle) {
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

// ─────────────────────────────────────────────
// parse_query  (local, no AI)
// ─────────────────────────────────────────────

SearchSchema parse_query(const std::string& query) {
    // Try to parse as JSON first (matches Rust behaviour)
    try {
        auto j = nlohmann::json::parse(query);
        SearchSchema s;
        if (j.contains("keywords"))
            s.keywords = j["keywords"].get<std::vector<std::string>>();
        if (j.contains("extensions"))
            s.extensions = j["extensions"].get<std::vector<std::string>>();
        if (j.contains("intent"))
            s.intent = j["intent"].get<std::string>();
        return s;
    } catch (...) { /* not JSON, fall through */ }

    // Split on whitespace, strip non-alphanumeric edges, lowercase
    SearchSchema s;
    std::istringstream iss(query);
    std::string word;
    while (iss >> word) {
        // Trim leading/trailing punctuation
        size_t start = word.find_first_of("abcdefghijklmnopqrstuvwxyz"
                                           "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
        size_t end   = word.find_last_of("abcdefghijklmnopqrstuvwxyz"
                                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
        if (start == std::string::npos) continue;
        word = word.substr(start, end - start + 1);
        if (!word.empty())
            s.keywords.push_back(to_lower(word));
    }
    return s;
}

// ─────────────────────────────────────────────
// Extension / keyword matching
// ─────────────────────────────────────────────

static bool matches_extension(const std::string& ext,
                               const std::vector<std::string>& requested) {
    if (requested.empty()) return true;
    std::string lo = to_lower(ext);
    for (const auto& r : requested) {
        std::string rlo = to_lower(r);
        if (!rlo.empty() && rlo.front() == '.') rlo = rlo.substr(1);
        if (rlo == lo) return true;
    }
    return false;
}

static bool matches_keywords(const std::string& file_name,
                              const SearchSchema& schema) {
    if (schema.keywords.empty()) return true;
    std::string lo = to_lower(file_name);
    for (const auto& kw : schema.keywords)
        if (lo.find(to_lower(kw)) != std::string::npos) return true;
    return false;
}

// ─────────────────────────────────────────────
// search_files  (parallel via std::async)
// ─────────────────────────────────────────────

std::vector<SearchResult> search_files(const std::filesystem::path& root,
                                       const SearchSchema& schema)
{
    // Collect all regular files first
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto& entry :
         std::filesystem::recursive_directory_iterator(root,
             std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (entry.is_regular_file(ec))
            files.push_back(entry.path());
    }

    // Filter in parallel using std::async chunks
    const size_t nthreads = std::max(1u, std::thread::hardware_concurrency());
    const size_t chunk    = (files.size() + nthreads - 1) / nthreads;

    std::mutex           mtx;
    std::vector<SearchResult> results;
    std::vector<std::future<void>> futures;

    for (size_t t = 0; t < nthreads; ++t) {
        size_t begin = t * chunk;
        size_t end   = std::min(begin + chunk, files.size());
        if (begin >= end) break;

        futures.push_back(std::async(std::launch::async,
            [&, begin, end]() {
                std::vector<SearchResult> local;
                for (size_t i = begin; i < end; ++i) {
                    const auto& p = files[i];
                    std::error_code e;

                    std::string fname = p.filename().string();
                    std::string ext;
                    if (p.has_extension())
                        ext = p.extension().string().substr(1); // drop leading '.'

                    if (!matches_extension(ext, schema.extensions)) continue;
                    if (!matches_keywords(fname, schema))           continue;

                    auto sz = std::filesystem::file_size(p, e);
                    if (e) sz = 0;

                    local.push_back({p, fname, ext, sz});
                }
                std::lock_guard<std::mutex> lk(mtx);
                for (auto& r : local) results.push_back(std::move(r));
            }));
    }

    for (auto& f : futures) f.get();
    return results;
}

// ─────────────────────────────────────────────
// parse_with_groq  (HTTPS call to api.groq.com)
// Uses WinHTTP on Windows (no extra deps).
// ─────────────────────────────────────────────

#ifdef _WIN32

// Helper: WinHTTP-based HTTPS POST returning response body as string
static std::string winhttp_post_json(const std::wstring& server,
                                     const std::wstring& path,
                                     const std::string& bearer_token,
                                     const std::string& body_json)
{
    HINTERNET session = WinHttpOpen(L"SafeVaultFS/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) throw std::runtime_error("WinHttpOpen failed");

    struct Guard {
        HINTERNET h;
        ~Guard() { if (h) WinHttpCloseHandle(h); }
    };
    Guard sg{session};

    HINTERNET conn = WinHttpConnect(session, server.c_str(),
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) throw std::runtime_error("WinHttpConnect failed");
    Guard cg{conn};

    HINTERNET req = WinHttpOpenRequest(conn, L"POST", path.c_str(),
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (!req) throw std::runtime_error("WinHttpOpenRequest failed");
    Guard rg{req};

    // Build headers
    std::wstring auth_hdr  = L"Authorization: Bearer " +
                              std::wstring(bearer_token.begin(), bearer_token.end());
    std::wstring ct_hdr    = L"Content-Type: application/json\r\n";
    ct_hdr += auth_hdr;

    WinHttpSendRequest(req,
                       ct_hdr.c_str(), static_cast<DWORD>(-1L),
                       const_cast<char*>(body_json.c_str()),
                       static_cast<DWORD>(body_json.size()),
                       static_cast<DWORD>(body_json.size()),
                       0);

    if (!WinHttpReceiveResponse(req, nullptr))
        throw std::runtime_error("WinHttpReceiveResponse failed");

    std::string response;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
        std::vector<char> buf(avail + 1, '\0');
        DWORD read = 0;
        WinHttpReadData(req, buf.data(), avail, &read);
        response.append(buf.data(), read);
    }
    return response;
}

SearchSchema parse_with_groq(const std::string& query, const std::string& api_key) {
    nlohmann::json payload = {
        {"model", "llama-3.1-8b-instant"},
        {"temperature", 0},
        {"response_format", {{"type", "json_object"}}},
        {"messages", nlohmann::json::array({
            {{"role", "system"},
             {"content", "Return JSON only with keywords (array of strings), "
                         "extensions (array of strings), and intent (string)."}},
            {{"role", "user"}, {"content", query}}
        })}
    };
    std::string body = payload.dump();

    std::string raw = winhttp_post_json(
        L"api.groq.com",
        L"/openai/v1/chat/completions",
        api_key, body);

    auto resp = nlohmann::json::parse(raw);
    std::string content = resp["choices"][0]["message"]["content"].get<std::string>();
    auto schema_json = nlohmann::json::parse(content);

    SearchSchema s;
    if (schema_json.contains("keywords"))
        s.keywords = schema_json["keywords"].get<std::vector<std::string>>();
    if (schema_json.contains("extensions"))
        s.extensions = schema_json["extensions"].get<std::vector<std::string>>();
    if (schema_json.contains("intent"))
        s.intent = schema_json["intent"].get<std::string>();
    return s;
}

#else  // Linux / macOS

SearchSchema parse_with_groq(const std::string& query, const std::string& /*api_key*/) {
    SearchSchema s;
    s.intent = query;
    s.keywords.push_back(query);
    return s;
}

#endif // _WIN32