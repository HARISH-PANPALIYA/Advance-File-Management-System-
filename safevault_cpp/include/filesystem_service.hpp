#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <cstdint>
#include <chrono>

namespace fileguard {

struct FileItem {
    std::string path;
    std::string name;
    std::string extension;
    bool is_directory{false};
    uint64_t size_bytes{0};
    int64_t modified_at{0}; // Unix timestamp in ms
    bool is_readonly{false};
    std::string hash;
};

struct DirectoryListing {
    std::string current_path;
    std::string parent_path;
    std::vector<FileItem> items;
    uint64_t total_files{0};
    uint64_t total_directories{0};
    uint64_t total_size_bytes{0};
};

struct SearchOptions {
    std::string root_path;
    std::string query;
    std::vector<std::string> extensions;
    std::optional<uint64_t> min_size;
    std::optional<uint64_t> max_size;
    std::optional<int64_t> modified_after;
    std::optional<int64_t> modified_before;
    std::string content_query;
    size_t limit{200};
};

struct DriveInfo {
    std::string letter;
    std::string label;
    std::string path;
    uint64_t total_bytes{0};
    uint64_t free_bytes{0};
};

struct FileTypeStat {
    std::string extension;
    int64_t count{0};
    uint64_t total_bytes{0};
};

struct DuplicateGroup {
    std::string sha256_hash;
    uint64_t size_bytes{0};
    std::vector<std::string> paths;
};

struct LargestFileInfo {
    std::string path;
    std::string name;
    uint64_t size_bytes{0};
};

struct FolderInsights {
    std::string folder_path;
    int64_t total_files{0};
    int64_t total_directories{0};
    uint64_t total_bytes{0};
    uint64_t reclaimable_bytes{0};
    std::vector<FileTypeStat> type_distribution;
    std::vector<DuplicateGroup> duplicates;
    std::vector<LargestFileInfo> largest_files;
    std::vector<std::string> empty_directories;
};

struct DocumentSaveResult {
    bool success{false};
    std::string error;
    bool conflict{false};
    std::string current_hash;
    std::string new_hash;
    int64_t revision_number{0};
    std::string revision_id;
};

class FilesystemService {
public:
    FilesystemService();
    explicit FilesystemService(std::vector<std::filesystem::path> approved_roots);

    // Root management
    void add_approved_root(const std::filesystem::path& root);
    void set_approved_roots(const std::vector<std::filesystem::path>& roots);
    const std::vector<std::filesystem::path>& get_approved_roots() const;
    bool is_path_approved(const std::filesystem::path& p) const;
    std::filesystem::path canonicalize_and_validate(const std::filesystem::path& p) const;

    // Directory operations
    DirectoryListing list_directory(const std::filesystem::path& dir_path, bool compute_hashes = false) const;
    FileItem get_metadata(const std::filesystem::path& path, bool compute_hash = true) const;

    // Mutating filesystem primitives
    bool create_file(const std::filesystem::path& path, const std::string& content = "");
    bool create_directory(const std::filesystem::path& path);
    bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& data);
    bool read_file(const std::filesystem::path& path, std::vector<uint8_t>& out_data) const;
    bool rename_item(const std::filesystem::path& source, const std::filesystem::path& destination);
    bool move_item(const std::filesystem::path& source, const std::filesystem::path& destination);
    bool copy_item(const std::filesystem::path& source, const std::filesystem::path& destination, bool overwrite = false);
    bool remove_item(const std::filesystem::path& path); // Raw remove (for internal / rollback use)

    // Search
    std::vector<FileItem> search(const SearchOptions& options) const;

    // System Drives & Folder Insights
    std::vector<DriveInfo> get_system_drives() const;
    FolderInsights get_folder_insights(const std::filesystem::path& folder, size_t max_files_scan = 5000) const;
    void populate_demo_workspace(const std::filesystem::path& target_dir) const;

    void set_allow_any_path(bool allow) { allow_any_path_ = allow; }
    bool is_allow_any_path() const { return allow_any_path_; }

    // Helpers
    static int64_t file_time_to_unix_ms(std::filesystem::file_time_type ftime);

private:
    std::vector<std::filesystem::path> approved_roots_;
    bool allow_any_path_{false};
};

} // namespace fileguard
