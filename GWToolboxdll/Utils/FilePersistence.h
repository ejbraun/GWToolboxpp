#pragma once

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace FilePersistence {
    class ScopedConfigLock {
    public:
        explicit ScopedConfigLock(const DWORD timeout_ms = 2000)
            : handle_(CreateMutexW(nullptr, FALSE, L"Local\\GWToolboxpp.ConfigIO.v1"))
        {
            if (!handle_) {
                error_ = std::format("CreateMutexW failed with Windows error {}", GetLastError());
                return;
            }
            const auto result = WaitForSingleObject(handle_, timeout_ms);
            acquired_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
            if (!acquired_) {
                error_ = result == WAIT_TIMEOUT
                    ? "Timed out waiting for another Guild Wars client to finish accessing Toolbox settings"
                    : std::format("WaitForSingleObject failed with Windows error {}", GetLastError());
            }
        }

        ~ScopedConfigLock()
        {
            if (acquired_) ReleaseMutex(handle_);
            if (handle_) CloseHandle(handle_);
        }

        ScopedConfigLock(const ScopedConfigLock&) = delete;
        ScopedConfigLock& operator=(const ScopedConfigLock&) = delete;

        [[nodiscard]] bool Acquired() const { return acquired_; }
        [[nodiscard]] const std::string& Error() const { return error_; }

    private:
        HANDLE handle_ = nullptr;
        bool acquired_ = false;
        std::string error_;
    };

    namespace detail {
        class ScopedHandle {
        public:
            explicit ScopedHandle(const HANDLE value) : value_(value) {}
            ~ScopedHandle()
            {
                if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
            }

            ScopedHandle(const ScopedHandle&) = delete;
            ScopedHandle& operator=(const ScopedHandle&) = delete;

            [[nodiscard]] bool Valid() const { return value_ != INVALID_HANDLE_VALUE; }
            [[nodiscard]] HANDLE Get() const { return value_; }

        private:
            HANDLE value_ = INVALID_HANDLE_VALUE;
        };

        inline bool WriteAll(const HANDLE file, const std::string_view bytes, std::string& error)
        {
            size_t written = 0;
            while (written < bytes.size()) {
                const auto remaining = bytes.size() - written;
                const auto chunk_size = static_cast<DWORD>(std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
                DWORD chunk_written = 0;
                if (!WriteFile(file, bytes.data() + written, chunk_size, &chunk_written, nullptr)) {
                    error = std::format("WriteFile failed with Windows error {}", GetLastError());
                    return false;
                }
                if (!chunk_written) {
                    error = "WriteFile completed without writing any settings data";
                    return false;
                }
                written += chunk_written;
            }
            return true;
        }
    }

    inline bool AtomicWrite(const std::filesystem::path& path, const std::string_view bytes, std::string& error)
    {
        error.clear();
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::error_code directory_error;
            std::filesystem::create_directories(parent, directory_error);
            if (directory_error) {
                error = std::format("Unable to create '{}': {}", parent.string(), directory_error.message());
                return false;
            }
        }

        static std::atomic_uint64_t next_temp_id = 0;
        auto temporary = std::filesystem::path{};
        HANDLE temporary_handle = INVALID_HANDLE_VALUE;
        DWORD create_error = ERROR_FILE_EXISTS;
        for (size_t attempt = 0; attempt < 32; ++attempt) {
            temporary = path;
            temporary += std::format(L".tmp.{}.{}", GetCurrentProcessId(), next_temp_id.fetch_add(1, std::memory_order_relaxed));
            temporary_handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (temporary_handle != INVALID_HANDLE_VALUE) break;
            create_error = GetLastError();
            if (create_error != ERROR_FILE_EXISTS && create_error != ERROR_ALREADY_EXISTS) break;
        }
        if (temporary_handle == INVALID_HANDLE_VALUE) {
            error = std::format("Unable to create temporary settings file '{}' (Windows error {})", temporary.string(), create_error);
            return false;
        }

        bool temporary_ready = false;
        {
            const detail::ScopedHandle file(temporary_handle);
            temporary_ready = detail::WriteAll(file.Get(), bytes, error);
            if (temporary_ready && !FlushFileBuffers(file.Get())) {
                error = std::format("Unable to flush temporary settings file '{}' (Windows error {})", temporary.string(), GetLastError());
                temporary_ready = false;
            }
        }
        if (!temporary_ready) {
            DeleteFileW(temporary.c_str());
            return false;
        }

        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto move_error = GetLastError();
            error = std::format("Unable to replace settings file '{}' (Windows error {})", path.string(), move_error);
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    }
}
