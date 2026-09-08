#include "ObjectiveTimerHistory.h"

#include <Windows.h>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <format>
#include <map>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace {
    class FileHandle {
    public:
        explicit FileHandle(HANDLE handle) : handle_(handle) {}
        ~FileHandle() { Close(); }
        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;
        [[nodiscard]] HANDLE Get() const { return handle_; }
        void Close()
        {
            if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    private:
        HANDLE handle_;
    };

    class HistoryLock {
    public:
        explicit HistoryLock(const std::filesystem::path& path)
            : file_(CreateFileW((path.wstring() + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr))
        {
            if (file_.Get() == INVALID_HANDLE_VALUE)
                throw std::system_error(GetLastError(), std::system_category(), "open history lock");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            OVERLAPPED overlapped{};
            while (!LockFileEx(file_.Get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) {
                const auto error = GetLastError();
                if (error != ERROR_LOCK_VIOLATION || std::chrono::steady_clock::now() >= deadline)
                    throw std::system_error(error, std::system_category(), "lock history file");
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
    private:
        // A stable sidecar keeps the lock valid when the JSON file is atomically replaced.
        FileHandle file_;
    };

    std::vector<ObjectiveTimerHistory::Run> ReadRuns(const std::filesystem::path& path)
    {
        const FileHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.Get() == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) return {};
            throw std::system_error(error, std::system_category(), "open history file");
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.Get(), &size))
            throw std::system_error(GetLastError(), std::system_category(), "get history size");
        if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > std::string{}.max_size())
            throw std::runtime_error("history file is too large");
        auto json = std::string(static_cast<size_t>(size.QuadPart), '\0');
        size_t offset = 0;
        while (offset < json.size()) {
            DWORD read = 0;
            const auto bytes = static_cast<DWORD>(std::min<size_t>(json.size() - offset, MAXDWORD));
            if (!ReadFile(file.Get(), json.data() + offset, bytes, &read, nullptr))
                throw std::system_error(GetLastError(), std::system_category(), "read history file");
            if (!read) throw std::runtime_error("history file changed during read");
            offset += read;
        }
        std::vector<ObjectiveTimerHistory::Run> runs;
        constexpr glz::opts opts{.error_on_unknown_keys = false};
        if (const auto error = glz::read<opts>(runs, json); error)
            throw std::runtime_error("invalid history JSON: " + glz::format_error(error, json));
        return runs;
    }

    void ReplaceRuns(const std::filesystem::path& path, const std::vector<ObjectiveTimerHistory::Run>& runs)
    {
        std::string json;
        if (glz::write_json(runs, json)) throw std::runtime_error("could not serialize run history");

        static std::atomic_uint64_t sequence = 0;
        std::filesystem::path temporary;
        HANDLE handle = INVALID_HANDLE_VALUE;
        for (size_t attempt = 0; attempt < 100 && handle == INVALID_HANDLE_VALUE; ++attempt) {
            temporary = path.wstring() + std::format(L".{}.{}.tmp", GetCurrentProcessId(), ++sequence);
            handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
                throw std::system_error(GetLastError(), std::system_category(), "create temporary history file");
        }
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("could not create a temporary history file");
        FileHandle file(handle);
        try {
            size_t offset = 0;
            while (offset < json.size()) {
                DWORD written = 0;
                const auto bytes = static_cast<DWORD>(std::min<size_t>(json.size() - offset, MAXDWORD));
                if (!WriteFile(file.Get(), json.data() + offset, bytes, &written, nullptr))
                    throw std::system_error(GetLastError(), std::system_category(), "write history file");
                if (!written) throw std::runtime_error("incomplete history write");
                offset += written;
            }
            if (!FlushFileBuffers(file.Get()))
                throw std::system_error(GetLastError(), std::system_category(), "flush history file");
            file.Close();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                const auto error = GetLastError();
                if ((error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)
                    || std::chrono::steady_clock::now() >= deadline)
                    throw std::system_error(error, std::system_category(), "replace history file");
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
        catch (...) {
            file.Close();
            DeleteFileW(temporary.c_str());
            throw;
        }
    }
}

std::string ObjectiveTimerHistory::Identity(const Run& run)
{
    if (run.run_id && !run.run_id->empty()) return *run.run_id;
    return std::format("legacy:{}:{}:{}", run.utc_start, run.instance_start, run.name);
}

ObjectiveTimerHistory::Result ObjectiveTimerHistory::Load(const std::filesystem::path& folder, const size_t limit)
{
    Result result;
    try {
        if (!std::filesystem::exists(folder)) return result;
        std::vector<std::filesystem::path> files;
        for (const auto& file : std::filesystem::directory_iterator(folder)) {
            if (file.is_regular_file() && file.path().filename().wstring().starts_with(L"ObjectiveTimerRuns_")
                && file.path().extension() == L".json")
                files.push_back(file.path());
        }
        std::ranges::sort(files, std::greater{});
        for (const auto& file : files) {
            if (result.runs.size() >= limit) break;
            try {
                auto runs = ReadRuns(file);
                std::ranges::sort(runs, [](const Run& a, const Run& b) { return a.utc_start > b.utc_start; });
                for (auto& run : runs) {
                    if (result.runs.size() >= limit) break;
                    result.runs.push_back(std::move(run));
                }
            }
            catch (const std::exception& error) {
                result.errors.push_back(std::format("{}: {}", file.string(), error.what()));
            }
        }
    }
    catch (const std::exception& error) {
        result.errors.push_back(error.what());
    }
    return result;
}

ObjectiveTimerHistory::Result ObjectiveTimerHistory::Save(const std::filesystem::path& folder, const std::vector<Run>& snapshot)
{
    Result result;
    try {
        std::filesystem::create_directories(folder);
        std::map<std::filesystem::path, std::vector<const Run*>> by_file;
        for (const auto& run : snapshot) {
            const time_t started = run.utc_start;
            tm utc{};
            if (gmtime_s(&utc, &started)) throw std::runtime_error("invalid run start time");
            const auto filename = std::format(L"ObjectiveTimerRuns_{:04}-{:02}-{:02}.json", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
            by_file[folder / filename].push_back(&run);
        }
        for (const auto& [path, updates] : by_file) {
            try {
                const HistoryLock lock(path);
                auto runs = ReadRuns(path);
                std::unordered_map<std::string, size_t> index;
                for (size_t i = 0; i < runs.size(); ++i) index[Identity(runs[i])] = i;
                for (const auto run : updates) {
                    const auto [it, inserted] = index.emplace(Identity(*run), runs.size());
                    if (inserted) runs.push_back(*run);
                    else runs[it->second] = *run;
                }
                ReplaceRuns(path, runs);
            }
            catch (const std::exception& error) {
                result.errors.push_back(std::format("{}: {}", path.string(), error.what()));
            }
        }
    }
    catch (const std::exception& error) {
        result.errors.push_back(error.what());
    }
    return result;
}
