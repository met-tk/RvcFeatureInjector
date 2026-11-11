#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr wchar_t kMarkerName[] = L".taketo-rvc-state";
constexpr char kMarkerContents[] = "TAKETO-RVCFEATUREINJECTOR-STATE-V1\n";
constexpr DWORD kPinnedSharing = FILE_SHARE_READ;

struct Failure {
    int status;
    std::wstring message;
    DWORD error;
};

[[noreturn]] void fail(int status, const std::wstring& message, DWORD error = 0) {
    throw Failure{status, message, error};
}

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { reset(); }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE); }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    void reset() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(value_, INVALID_HANDLE_VALUE)); }
private:
    HANDLE value_;
};

class Search {
public:
    explicit Search(HANDLE value) : value_(value) {}
    ~Search() { if (value_ != INVALID_HANDLE_VALUE) FindClose(value_); }
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

bool sameName(const std::wstring& left, const wchar_t* right) {
    return _wcsicmp(left.c_str(), right) == 0;
}

std::vector<std::wstring> validateRoot(const std::wstring& input) {
    if (input.size() < 4 || !iswalpha(input[0]) || input[1] != L':' ||
        (input[2] != L'\\' && input[2] != L'/')) fail(3, L"Expected a local absolute drive path");
    std::wstring path(input);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.back() == L'\\') path.pop_back();
    std::vector<std::wstring> parts{path.substr(0, 3)};
    size_t begin = 3;
    while (begin < path.size()) {
        const size_t end = path.find(L'\\', begin);
        const auto part = path.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ' ||
            part.find_first_of(L"<>:\"|?*") != std::wstring::npos ||
            std::any_of(part.begin(), part.end(), [](wchar_t c) { return c < L' '; })) {
            fail(3, L"Ambiguous or invalid state directory component");
        }
        parts.push_back(part);
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    if (parts.size() < 4 || !sameName(parts[parts.size() - 3], L"Taketo") ||
        !sameName(parts[parts.size() - 2], L"RvcFeatureInjector") || !sameName(parts.back(), L"state")) {
        fail(3, L"Only the dedicated Taketo\\RvcFeatureInjector\\state directory is allowed");
    }
    return parts;
}

struct Entry {
    fs::path path;
    Handle handle;
    bool directory;
};

Entry openPinned(const fs::path& path, bool deletionAccess, bool readContents, int missingStatus) {
    const DWORD access = FILE_READ_ATTRIBUTES | (deletionAccess ? DELETE : 0) | (readContents ? FILE_READ_DATA : 0);
    Handle handle(CreateFileW(path.c_str(), access, kPinnedSharing, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        fail(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? missingStatus : 3,
             L"Cannot exclusively pin directory entry: " + path.wstring(), error);
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(handle.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle.get(), &info)) {
        fail(3, L"Cannot inspect pinned directory entry: " + path.wstring(), GetLastError());
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) fail(3, L"Reparse points are not allowed: " + path.wstring());
    if (deletionAccess && (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) fail(3, L"Read-only state entry: " + path.wstring());
    return Entry{path, std::move(handle), (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0};
}

void validateMarker(const Entry& marker) {
    if (marker.directory) fail(2, L"State marker is not an owned file");
    char contents[sizeof(kMarkerContents)]{};
    DWORD count = 0;
    if (!ReadFile(marker.handle.get(), contents, sizeof(contents), &count, nullptr)) {
        fail(3, L"Cannot read state ownership marker", GetLastError());
    }
    if (count != sizeof(kMarkerContents) - 1 ||
        !std::equal(contents, contents + count, kMarkerContents)) fail(2, L"Foreign state marker preserved");
}

void collectTree(const fs::path& directory, bool root, size_t depth, std::vector<Entry>& entries) {
    if (depth > 128) fail(3, L"State directory depth exceeds the cleanup limit");
    WIN32_FIND_DATAW data{};
    Search search(FindFirstFileW((directory / L"*").c_str(), &data));
    if (search.get() == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND && !root) return;
        fail(3, L"Cannot enumerate state directory: " + directory.wstring(), GetLastError());
    }
    bool foundMarker = false;
    do {
        const std::wstring name(data.cFileName);
        if (name == L"." || name == L"..") continue;
        if (root && sameName(name, kMarkerName)) {
            if (foundMarker) fail(3, L"Ambiguous state ownership marker");
            foundMarker = true;
            continue; // The verified marker is already pinned and will be removed last.
        }
        const fs::path childPath = directory / name;
        entries.push_back(openPinned(childPath, true, false, 3));
        if (entries.back().directory) collectTree(childPath, false, depth + 1, entries);
    } while (FindNextFileW(search.get(), &data));
    if (GetLastError() != ERROR_NO_MORE_FILES) fail(3, L"State directory enumeration failed", GetLastError());
    if (root && !foundMarker) fail(3, L"Pinned state marker disappeared from its directory");
}

int cleanup(const std::wstring& input, bool remove) {
    const auto parts = validateRoot(input);
    std::vector<Entry> ancestors;
    fs::path path(parts[0]);
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (i > 0) path /= parts[i];
        ancestors.push_back(openPinned(path, false, false, 2));
        if (!ancestors.back().directory) fail(3, L"State ancestor is not a directory");
    }
    path /= parts.back();
    std::vector<Entry> entries;
    entries.push_back(openPinned(path, true, false, 2));
    if (!entries.front().directory) fail(3, L"State root is not a directory");
    entries.push_back(openPinned(path / kMarkerName, true, true, 2));
    validateMarker(entries[1]);
    // Parents and children deny write/delete sharing throughout validation and deletion.
    // This also prevents changing a pinned empty directory into a junction.
    collectTree(path, true, 0, entries);
    if (remove) {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(it->handle.get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
                fail(3, L"Cannot delete pinned state entry: " + it->path.wstring(), GetLastError());
            }
            it->handle.reset();
        }
    }
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || (std::wstring(argv[1]) != L"--check" && std::wstring(argv[1]) != L"--remove")) {
        std::wcerr << L"Usage: rvc-state-cleanup.exe --check|--remove ROOT\n";
        return 64;
    }
    try {
        return cleanup(argv[2], std::wstring(argv[1]) == L"--remove");
    } catch (const Failure& error) {
        std::wcerr << error.message;
        if (error.error) std::wcerr << L" (Windows error " << error.error << L")";
        std::wcerr << L'\n';
        return error.status;
    } catch (const std::exception&) {
        std::wcerr << L"State cleanup failed; remaining entries preserved\n";
        return 3;
    }
}
