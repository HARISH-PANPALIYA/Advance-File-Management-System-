#include "passthrough.hpp"

#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

static uint32_t self_pid() {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

BackingStore::BackingStore(std::filesystem::path root, Journal& journal)
    : root_(std::move(root)), journal_(journal)
{
    std::filesystem::create_directories(root_);
}

std::filesystem::path BackingStore::resolve(const std::filesystem::path& rel) const {
    return root_ / rel;
}

std::vector<uint8_t> BackingStore::read(const std::filesystem::path& rel) const {
    auto full = resolve(rel);
    std::ifstream f(full, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot read: " + full.string());
    return {std::istreambuf_iterator<char>(f), {}};
}

void BackingStore::write(const std::filesystem::path& rel,
                         const std::vector<uint8_t>& contents)
{
    auto full  = resolve(rel);
    auto kind  = std::filesystem::exists(full) ? EventKind::Modify : EventKind::Create;
    journal_.append(EventRecord::now(kind, full, std::nullopt, self_pid()));

    std::filesystem::create_directories(full.parent_path());
    std::ofstream f(full, std::ios::binary | std::ios::trunc);
    if (!f)
        throw std::runtime_error("Cannot write: " + full.string());
    f.write(reinterpret_cast<const char*>(contents.data()),
            static_cast<std::streamsize>(contents.size()));
}

void BackingStore::remove(const std::filesystem::path& rel) {
    auto full = resolve(rel);
    journal_.append(EventRecord::now(EventKind::Delete, full, std::nullopt, self_pid()));
    std::filesystem::remove(full);
}

void BackingStore::rename(const std::filesystem::path& old_rel,
                          const std::filesystem::path& new_rel)
{
    auto old_full = resolve(old_rel);
    auto new_full = resolve(new_rel);
    journal_.append(EventRecord::now(EventKind::Rename, new_full, old_full, self_pid()));
    std::filesystem::create_directories(new_full.parent_path());
    std::filesystem::rename(old_full, new_full);
}
