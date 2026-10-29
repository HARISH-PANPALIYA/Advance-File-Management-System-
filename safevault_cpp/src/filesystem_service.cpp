#include "filesystem_service.hpp"
#include "sha256.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <cctype>
#include <unordered_map>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  ifdef CreateFile
#    undef CreateFile
#  endif
#  ifdef CreateDirectory
#    undef CreateDirectory
#  endif
#  ifdef DeleteFile
#    undef DeleteFile
#  endif
#  ifdef CopyFile
#    undef CopyFile
#  endif
#  ifdef MoveFile
#    undef MoveFile
#  endif
#endif

namespace fileguard {

namespace {

std::string normalize_path_str(const std::filesystem::path& p) {
    std::string s = p.lexically_normal().string();
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return s;
}

bool is_subpath(const std::filesystem::path& base, const std::filesystem::path& sub) {
    std::string base_norm = normalize_path_str(base);
    std::string sub_norm = normalize_path_str(sub);
    if (base_norm == sub_norm) return true;
    if (sub_norm.size() > base_norm.size()) {
        if (sub_norm.compare(0, base_norm.size(), base_norm) == 0) {
            return sub_norm[base_norm.size()] == '/';
        }
    }
    return false;
}

std::string to_lower_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

} // namespace

int64_t FilesystemService::file_time_to_unix_ms(std::filesystem::file_time_type ftime) {
    try {
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        return std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();
    } catch (...) {
        return 0;
    }
}

FilesystemService::FilesystemService() {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        approved_roots_.push_back(cwd);
    }
}

FilesystemService::FilesystemService(std::vector<std::filesystem::path> approved_roots)
    : approved_roots_(std::move(approved_roots)) {
    if (approved_roots_.empty()) {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (!ec) approved_roots_.push_back(cwd);
    }
}

void FilesystemService::add_approved_root(const std::filesystem::path& root) {
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(root, ec);
    if (!ec && !canon.empty()) {
        for (const auto& existing : approved_roots_) {
            if (is_subpath(existing, canon)) return; // already covered
        }
        approved_roots_.push_back(canon);
    }
}

void FilesystemService::set_approved_roots(const std::vector<std::filesystem::path>& roots) {
    approved_roots_.clear();
    for (const auto& r : roots) {
        add_approved_root(r);
    }
    if (approved_roots_.empty()) {
        std::error_code ec;
        approved_roots_.push_back(std::filesystem::current_path(ec));
    }
}

const std::vector<std::filesystem::path>& FilesystemService::get_approved_roots() const {
    return approved_roots_;
}

bool FilesystemService::is_path_approved(const std::filesystem::path& p) const {
    if (allow_any_path_) return true;
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(p, ec);
    if (ec) return false;
    for (const auto& root : approved_roots_) {
        auto root_canon = std::filesystem::weakly_canonical(root, ec);
        if (!ec && is_subpath(root_canon, canon)) {
            return true;
        }
    }
    return false;
}

std::filesystem::path FilesystemService::canonicalize_and_validate(const std::filesystem::path& p) const {
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(p, ec);
    if (ec) {
        throw std::runtime_error("Invalid filesystem path: " + p.string() + " (" + ec.message() + ")");
    }
    if (!is_path_approved(canon)) {
        throw std::runtime_error("Access denied: path is outside approved roots: " + canon.string());
    }
    return canon;
}

FileItem FilesystemService::get_metadata(const std::filesystem::path& path, bool compute_hash) const {
    auto valid_path = canonicalize_and_validate(path);
    std::error_code ec;
    auto status = std::filesystem::status(valid_path, ec);
    if (ec || !std::filesystem::exists(status)) {
        throw std::runtime_error("File or directory not found: " + valid_path.string());
    }

    FileItem item;
    item.path = valid_path.string();
    std::replace(item.path.begin(), item.path.end(), '\\', '/');
    item.name = valid_path.filename().string();
    item.is_directory = std::filesystem::is_directory(status);

    if (!item.is_directory) {
        if (valid_path.has_extension()) {
            std::string ext = valid_path.extension().string();
            if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
            item.extension = to_lower_str(ext);
        }
        item.size_bytes = std::filesystem::file_size(valid_path, ec);
        if (ec) item.size_bytes = 0;
        if (compute_hash) {
            item.hash = sha256_file(valid_path);
        }
    }

    auto ftime = std::filesystem::last_write_time(valid_path, ec);
    if (!ec) {
        item.modified_at = file_time_to_unix_ms(ftime);
    }

    auto perms = status.permissions();
    item.is_readonly = ((perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none);

    return item;
}

DirectoryListing FilesystemService::list_directory(const std::filesystem::path& dir_path, bool compute_hashes) const {
    auto valid_path = canonicalize_and_validate(dir_path);
    std::error_code ec;
    if (!std::filesystem::is_directory(valid_path, ec)) {
        throw std::runtime_error("Not a directory: " + valid_path.string());
    }

    DirectoryListing listing;
    listing.current_path = valid_path.string();
    std::replace(listing.current_path.begin(), listing.current_path.end(), '\\', '/');

    auto parent = valid_path.parent_path();
    if (is_path_approved(parent) && parent != valid_path) {
        listing.parent_path = parent.string();
        std::replace(listing.parent_path.begin(), listing.parent_path.end(), '\\', '/');
    } else {
        listing.parent_path = "";
    }

    for (const auto& entry : std::filesystem::directory_iterator(valid_path,
             std::filesystem::directory_options::skip_permission_denied, ec)) {
        FileItem item;
        item.path = entry.path().string();
        std::replace(item.path.begin(), item.path.end(), '\\', '/');
        item.name = entry.path().filename().string();
        auto status = entry.status(ec);
        if (ec) continue;

        item.is_directory = std::filesystem::is_directory(status);
        if (item.is_directory) {
            listing.total_directories++;
        } else {
            listing.total_files++;
            if (entry.path().has_extension()) {
                std::string ext = entry.path().extension().string();
                if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
                item.extension = to_lower_str(ext);
            }
            item.size_bytes = entry.file_size(ec);
            if (ec) item.size_bytes = 0;
            listing.total_size_bytes += item.size_bytes;

            if (compute_hashes) {
                item.hash = sha256_file(entry.path());
            }
        }

        auto ftime = entry.last_write_time(ec);
        if (!ec) {
            item.modified_at = file_time_to_unix_ms(ftime);
        }
        item.is_readonly = ((status.permissions() & std::filesystem::perms::owner_write) == std::filesystem::perms::none);

        listing.items.push_back(std::move(item));
    }

    // Sort: directories first, then alphabetically
    std::sort(listing.items.begin(), listing.items.end(), [](const FileItem& a, const FileItem& b) {
        if (a.is_directory != b.is_directory) return a.is_directory > b.is_directory;
        return to_lower_str(a.name) < to_lower_str(b.name);
    });

    return listing;
}

bool FilesystemService::create_file(const std::filesystem::path& path, const std::string& content) {
    auto valid_path = canonicalize_and_validate(path);
    std::error_code ec;
    if (std::filesystem::exists(valid_path, ec)) {
        throw std::runtime_error("File already exists: " + valid_path.string());
    }
    if (valid_path.has_parent_path()) {
        std::filesystem::create_directories(valid_path.parent_path(), ec);
    }
    std::ofstream out(valid_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Failed to create file: " + valid_path.string());
    }
    if (!content.empty()) {
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    return true;
}

bool FilesystemService::create_directory(const std::filesystem::path& path) {
    auto valid_path = canonicalize_and_validate(path);
    std::error_code ec;
    if (std::filesystem::exists(valid_path, ec)) {
        throw std::runtime_error("Directory or file already exists: " + valid_path.string());
    }
    if (!std::filesystem::create_directories(valid_path, ec)) {
        if (ec) {
            throw std::runtime_error("Failed to create directory: " + ec.message());
        }
    }
    return true;
}

bool FilesystemService::write_file(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    auto valid_path = canonicalize_and_validate(path);
    std::error_code ec;
    if (valid_path.has_parent_path()) {
        std::filesystem::create_directories(valid_path.parent_path(), ec);
    }
    std::ofstream out(valid_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open file for writing: " + valid_path.string());
    }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return true;
}

bool FilesystemService::read_file(const std::filesystem::path& path, std::vector<uint8_t>& out_data) const {
    auto valid_path = canonicalize_and_validate(path);
    std::ifstream in(valid_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open file for reading: " + valid_path.string());
    }
    in.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    out_data.resize(size);
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out_data.data()), static_cast<std::streamsize>(size));
    }
    return true;
}

bool FilesystemService::rename_item(const std::filesystem::path& source, const std::filesystem::path& destination) {
    auto valid_src = canonicalize_and_validate(source);
    auto valid_dst = canonicalize_and_validate(destination);

    std::error_code ec;
    if (!std::filesystem::exists(valid_src, ec)) {
        throw std::runtime_error("Source does not exist: " + valid_src.string());
    }

    bool is_case_only_rename = false;
    if (std::filesystem::exists(valid_dst, ec)) {
        if (std::filesystem::equivalent(valid_src, valid_dst, ec)) {
            is_case_only_rename = true;
        } else {
            throw std::runtime_error("Destination already exists: " + valid_dst.string());
        }
    }

    if (valid_dst.has_parent_path()) {
        std::filesystem::create_directories(valid_dst.parent_path(), ec);
    }

    if (is_case_only_rename) {
        auto temp_dst = valid_src.parent_path() / (valid_src.filename().string() + ".fg_tmp_case_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        std::filesystem::rename(valid_src, temp_dst, ec);
        if (ec) {
            throw std::runtime_error("Case-only rename intermediate step failed: " + ec.message());
        }
        std::filesystem::rename(temp_dst, valid_dst, ec);
        if (ec) {
            std::error_code rec;
            std::filesystem::rename(temp_dst, valid_src, rec);
            throw std::runtime_error("Case-only rename final step failed: " + ec.message());
        }
    } else {
        std::filesystem::rename(valid_src, valid_dst, ec);
        if (ec) {
            throw std::runtime_error("Rename failed: " + ec.message());
        }
    }
    return true;
}

bool FilesystemService::move_item(const std::filesystem::path& source, const std::filesystem::path& destination) {
    auto valid_src = canonicalize_and_validate(source);
    auto valid_dst = canonicalize_and_validate(destination);

    std::error_code ec;
    if (!std::filesystem::exists(valid_src, ec)) {
        throw std::runtime_error("Source does not exist: " + valid_src.string());
    }
    if (std::filesystem::exists(valid_dst, ec)) {
        throw std::runtime_error("Destination already exists: " + valid_dst.string());
    }

    if (valid_dst.has_parent_path()) {
        std::filesystem::create_directories(valid_dst.parent_path(), ec);
    }

    // Try fast rename
    std::filesystem::rename(valid_src, valid_dst, ec);
    if (!ec) return true;

    // Cross-volume move fallback
    ec.clear();
    std::filesystem::copy(valid_src, valid_dst,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks, ec);
    if (ec) {
        throw std::runtime_error("Move (copy fallback) failed: " + ec.message());
    }
    std::filesystem::remove_all(valid_src, ec);
    return true;
}

bool FilesystemService::copy_item(const std::filesystem::path& source, const std::filesystem::path& destination, bool overwrite) {
    auto valid_src = canonicalize_and_validate(source);
    auto valid_dst = canonicalize_and_validate(destination);

    std::error_code ec;
    if (!std::filesystem::exists(valid_src, ec)) {
        throw std::runtime_error("Source does not exist: " + valid_src.string());
    }
    if (std::filesystem::exists(valid_dst, ec) && !overwrite) {
        throw std::runtime_error("Destination already exists: " + valid_dst.string());
    }

    if (valid_dst.has_parent_path()) {
        std::filesystem::create_directories(valid_dst.parent_path(), ec);
    }

    auto options = std::filesystem::copy_options::recursive;
    if (overwrite) {
        options |= std::filesystem::copy_options::overwrite_existing;
    }

    std::filesystem::copy(valid_src, valid_dst, options, ec);
    if (ec) {
        throw std::runtime_error("Copy failed: " + ec.message());
    }
    return true;
}

bool FilesystemService::remove_item(const std::filesystem::path& path) {
    auto valid_path = canonicalize_and_validate(path);
    std::error_code ec;
    if (!std::filesystem::exists(valid_path, ec)) {
        return false;
    }
    std::filesystem::remove_all(valid_path, ec);
    if (ec) {
        throw std::runtime_error("Failed to remove: " + valid_path.string() + " (" + ec.message() + ")");
    }
    return true;
}

std::vector<FileItem> FilesystemService::search(const SearchOptions& options) const {
    std::vector<FileItem> results;
    std::filesystem::path root = options.root_path.empty() ? approved_roots_.front() : std::filesystem::path(options.root_path);
    auto valid_root = canonicalize_and_validate(root);

    std::string query_lower = to_lower_str(options.query);
    std::vector<std::string> ext_lowers;
    for (const auto& ext : options.extensions) {
        std::string e = to_lower_str(ext);
        if (!e.empty() && e[0] == '.') e = e.substr(1);
        ext_lowers.push_back(e);
    }

    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(valid_root,
             std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {

        if (results.size() >= options.limit) break;

        const auto& entry = *it;
        auto status = entry.status(ec);
        if (ec) continue;

        bool is_dir = std::filesystem::is_directory(status);
        std::string filename = entry.path().filename().string();
        std::string filename_lower = to_lower_str(filename);

        // Filename query match
        if (!query_lower.empty() && filename_lower.find(query_lower) == std::string::npos) {
            continue;
        }

        // Extension match
        std::string ext;
        if (entry.path().has_extension()) {
            ext = entry.path().extension().string();
            if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
            ext = to_lower_str(ext);
        }
        if (!ext_lowers.empty()) {
            if (is_dir || std::find(ext_lowers.begin(), ext_lowers.end(), ext) == ext_lowers.end()) {
                continue;
            }
        }

        // Size match
        uint64_t size = 0;
        if (!is_dir) {
            size = entry.file_size(ec);
            if (ec) size = 0;
            if (options.min_size.has_value() && size < *options.min_size) continue;
            if (options.max_size.has_value() && size > *options.max_size) continue;
        }

        // Modified date match
        int64_t mod_ms = 0;
        auto ftime = entry.last_write_time(ec);
        if (!ec) {
            mod_ms = file_time_to_unix_ms(ftime);
            if (options.modified_after.has_value() && mod_ms < *options.modified_after) continue;
            if (options.modified_before.has_value() && mod_ms > *options.modified_before) continue;
        }

        // Content query match (optional)
        if (!options.content_query.empty() && !is_dir) {
            std::ifstream file(entry.path(), std::ios::binary);
            if (!file) continue;
            char buffer[65536];
            file.read(buffer, sizeof(buffer) - 1);
            std::streamsize bytes_read = file.gcount();
            buffer[bytes_read] = '\0';
            std::string content(buffer, static_cast<size_t>(bytes_read));
            if (to_lower_str(content).find(to_lower_str(options.content_query)) == std::string::npos) {
                continue;
            }
        }

        FileItem item;
        item.path = entry.path().string();
        std::replace(item.path.begin(), item.path.end(), '\\', '/');
        item.name = filename;
        item.extension = ext;
        item.is_directory = is_dir;
        item.size_bytes = size;
        item.modified_at = mod_ms;
        item.is_readonly = ((status.permissions() & std::filesystem::perms::owner_write) == std::filesystem::perms::none);
        results.push_back(std::move(item));
    }

    return results;
}

std::vector<DriveInfo> FilesystemService::get_system_drives() const {
    std::vector<DriveInfo> drives;
#ifdef _WIN32
    wchar_t drive_strings[512];
    DWORD len = GetLogicalDriveStringsW(511, drive_strings);
    if (len > 0 && len <= 511) {
        wchar_t* p = drive_strings;
        while (*p) {
            std::wstring root_w = p;
            p += root_w.length() + 1;

            DriveInfo info;
            std::string root_str(root_w.begin(), root_w.end());
            info.letter = root_str.substr(0, 2);
            info.path = root_str;
            std::replace(info.path.begin(), info.path.end(), '\\', '/');

            wchar_t vol_name[MAX_PATH + 1] = {0};
            if (GetVolumeInformationW(root_w.c_str(), vol_name, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) {
                std::wstring vn = vol_name;
                info.label = std::string(vn.begin(), vn.end());
            }
            if (info.label.empty()) {
                info.label = "Local Disk (" + info.letter + ")";
            } else {
                info.label += " (" + info.letter + ")";
            }

            ULARGE_INTEGER free_bytes_avail, total_bytes, total_free_bytes;
            if (GetDiskFreeSpaceExW(root_w.c_str(), &free_bytes_avail, &total_bytes, &total_free_bytes)) {
                info.total_bytes = total_bytes.QuadPart;
                info.free_bytes = free_bytes_avail.QuadPart;
            }

            drives.push_back(std::move(info));
        }
    }
#else
    DriveInfo root_info;
    root_info.letter = "/";
    root_info.label = "Root Filesystem";
    root_info.path = "/";
    std::error_code ec;
    auto space = std::filesystem::space("/", ec);
    if (!ec) {
        root_info.total_bytes = space.capacity;
        root_info.free_bytes = space.available;
    }
    drives.push_back(std::move(root_info));
#endif
    return drives;
}

FolderInsights FilesystemService::get_folder_insights(const std::filesystem::path& folder, size_t max_files_scan) const {
    auto valid_folder = canonicalize_and_validate(folder);
    FolderInsights insights;
    insights.folder_path = valid_folder.string();
    std::replace(insights.folder_path.begin(), insights.folder_path.end(), '\\', '/');

    std::unordered_map<std::string, FileTypeStat> ext_map;
    std::unordered_map<uint64_t, std::vector<std::string>> size_map;
    std::vector<LargestFileInfo> all_files;

    std::error_code ec;
    size_t scanned = 0;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             valid_folder,
             std::filesystem::directory_options::skip_permission_denied,
             ec)) {
        if (ec) continue;
        if (++scanned > max_files_scan) break;

        auto status = entry.status(ec);
        if (ec) continue;

        if (std::filesystem::is_directory(status)) {
            insights.total_directories++;
            bool is_empty = true;
            for (const auto& child : std::filesystem::directory_iterator(entry.path(), std::filesystem::directory_options::skip_permission_denied, ec)) {
                (void)child;
                is_empty = false;
                break;
            }
            if (is_empty) {
                std::string empty_p = entry.path().string();
                std::replace(empty_p.begin(), empty_p.end(), '\\', '/');
                insights.empty_directories.push_back(std::move(empty_p));
            }
        } else if (std::filesystem::is_regular_file(status)) {
            insights.total_files++;
            uint64_t sz = entry.file_size(ec);
            if (ec) sz = 0;
            insights.total_bytes += sz;

            std::string ext = "";
            if (entry.path().has_extension()) {
                ext = entry.path().extension().string();
                if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
                ext = to_lower_str(ext);
            }
            if (ext.empty()) ext = "other";

            auto& stat = ext_map[ext];
            stat.extension = ext;
            stat.count++;
            stat.total_bytes += sz;

            std::string fpath = entry.path().string();
            std::replace(fpath.begin(), fpath.end(), '\\', '/');

            LargestFileInfo lf;
            lf.path = fpath;
            lf.name = entry.path().filename().string();
            lf.size_bytes = sz;
            all_files.push_back(std::move(lf));

            if (sz > 0) {
                size_map[sz].push_back(fpath);
            }
        }
    }

    for (auto& pair : ext_map) {
        insights.type_distribution.push_back(std::move(pair.second));
    }
    std::sort(insights.type_distribution.begin(), insights.type_distribution.end(), [](const FileTypeStat& a, const FileTypeStat& b) {
        return a.total_bytes > b.total_bytes;
    });

    std::sort(all_files.begin(), all_files.end(), [](const LargestFileInfo& a, const LargestFileInfo& b) {
        return a.size_bytes > b.size_bytes;
    });
    if (all_files.size() > 10) all_files.resize(10);
    insights.largest_files = std::move(all_files);

    for (auto& pair : size_map) {
        if (pair.second.size() < 2) continue;
        uint64_t file_sz = pair.first;

        std::unordered_map<std::string, std::vector<std::string>> hash_map;
        for (const auto& p : pair.second) {
            std::string h = sha256_file(std::filesystem::path(p));
            if (!h.empty()) {
                hash_map[h].push_back(p);
            }
        }

        for (auto& h_entry : hash_map) {
            if (h_entry.second.size() > 1) {
                DuplicateGroup dg;
                dg.sha256_hash = h_entry.first;
                dg.size_bytes = file_sz;
                dg.paths = std::move(h_entry.second);
                insights.reclaimable_bytes += (dg.paths.size() - 1) * file_sz;
                insights.duplicates.push_back(std::move(dg));
            }
        }
    }

    std::sort(insights.duplicates.begin(), insights.duplicates.end(), [](const DuplicateGroup& a, const DuplicateGroup& b) {
        uint64_t rec_a = (a.paths.size() - 1) * a.size_bytes;
        uint64_t rec_b = (b.paths.size() - 1) * b.size_bytes;
        return rec_a > rec_b;
    });

    return insights;
}

void FilesystemService::populate_demo_workspace(const std::filesystem::path& target_dir) const {
    std::filesystem::create_directories(target_dir);
    std::filesystem::create_directories(target_dir / "duplicates");
    std::filesystem::create_directories(target_dir / "subfolder");
    std::filesystem::create_directories(target_dir / "empty_folder");

    std::string paper_content =
        "Operating System Event Notification Architectures\n"
        "=================================================\n\n"
        "Modern operating systems provide native event multiplexing APIs:\n"
        "1. Windows: ReadDirectoryChangesW and I/O Completion Ports (IOCP)\n"
        "2. Linux: inotify and epoll\n"
        "3. macOS / BSD: kqueue and FSEvents\n\n"
        "Event-driven monitoring reduces CPU wakeups and latency from polling intervals (O(N) filesystem walks) "
        "to sub-millisecond kernel push notifications (O(1)). FileGuard leverages these primitives for instant "
        "conflict awareness and transaction journaling.\n";

    const_cast<FilesystemService*>(this)->create_file(target_dir / "research_paper.txt", paper_content);
    const_cast<FilesystemService*>(this)->create_file(target_dir / "duplicates" / "research_paper_copy.txt", paper_content);

    std::string cpp_content =
        "#include <iostream>\n"
        "#include <unordered_map>\n"
        "#include <list>\n"
        "#include <mutex>\n\n"
        "// Concurrent LRU Cache implementation\n"
        "template<typename K, typename V>\n"
        "class ConcurrentLRUCache {\n"
        "    size_t capacity_;\n"
        "    std::list<std::pair<K, V>> items_;\n"
        "    std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> map_;\n"
        "    mutable std::mutex mutex_;\n"
        "public:\n"
        "    explicit ConcurrentLRUCache(size_t cap) : capacity_(cap) {}\n"
        "    void put(const K& key, const V& val) {\n"
        "        std::lock_guard<std::mutex> lock(mutex_);\n"
        "        auto it = map_.find(key);\n"
        "        if (it != map_.end()) {\n"
        "            items_.erase(it->second);\n"
        "        } else if (items_.size() >= capacity_) {\n"
        "            auto last = items_.end(); --last;\n"
        "            map_.erase(last->first);\n"
        "            items_.pop_back();\n"
        "        }\n"
        "        items_.emplace_front(key, val);\n"
        "        map_[key] = items_.begin();\n"
        "    }\n"
        "};\n";

    const_cast<FilesystemService*>(this)->create_file(target_dir / "algorithm.cpp", cpp_content);

    std::string notes_content =
        "# FileGuard Project Architecture & Sprint Notes\n\n"
        "## Key Milestones\n"
        "- [x] High-performance C++20 transactional filesystem engine\n"
        "- [x] SQLite WAL operation journal with LIFO undo/redo\n"
        "- [x] Conflict-aware external change detection (X-0001 workflow)\n"
        "- [x] Dual independent name/location history and content revisioning\n"
        "- [x] OS Performance Lab: event latency, journal overhead, concurrent scan\n"
        "- [x] In-app document editor with live pre-hash conflict detection\n"
        "- [x] Warm editorial desktop productivity interface\n";

    const_cast<FilesystemService*>(this)->create_file(target_dir / "notes.md", notes_content);

    std::string csv_content =
        "Timestamp,Operation,DurationMs,ItemsProcessed,ThroughputOpsSec,Status\n"
        "2026-10-28T12:00:00Z,EVENT_LATENCY,0.42,1,2380.95,SUCCESS\n"
        "2026-10-28T12:00:01Z,POLLING_LATENCY,52.80,1,18.94,SUCCESS\n"
        "2026-10-28T12:00:02Z,JOURNAL_OVERHEAD,1.15,100,86956.52,SUCCESS\n"
        "2026-10-28T12:00:03Z,CONCURRENT_SCAN,14.60,1500,102739.73,SUCCESS\n";

    const_cast<FilesystemService*>(this)->create_file(target_dir / "metrics.csv", csv_content);

    std::string json_content =
        "{\n"
        "  \"system\": \"FileGuard\",\n"
        "  \"version\": \"2.0.0\",\n"
        "  \"features\": [\n"
        "    \"event_driven_monitoring\",\n"
        "    \"lifo_undo_redo\",\n"
        "    \"external_candidate_triage\",\n"
        "    \"independent_content_revisions\",\n"
        "    \"os_performance_benchmarks\"\n"
        "  ],\n"
        "  \"environment\": {\n"
        "    \"engine\": \"C++20\",\n"
        "    \"database\": \"SQLite WAL\",\n"
        "    \"ui\": \"Warm Editorial Productivity React/TS\"\n"
        "  }\n"
        "}\n";

    const_cast<FilesystemService*>(this)->create_file(target_dir / "dataset.json", json_content);

    std::string py_content =
        "# Utility script for generating telemetry data\n"
        "import sys\n\n"
        "def main():\n"
        "    print(\"FileGuard telemetry generator ready.\")\n\n"
        "if __name__ == '__main__':\n"
        "    main()\n";

    const_cast<FilesystemService*>(this)->create_file(target_dir / "subfolder" / "code_sample.py", py_content);
}

} // namespace fileguard
