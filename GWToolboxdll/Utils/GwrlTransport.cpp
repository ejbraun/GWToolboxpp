#include "GwrlTransport.h"
#include <bcrypt.h>
#include <fstream>
#include <format>
#include <cstring>
#include <memory>

namespace Gwrl {
    uint64_t ProcessStarted(const HANDLE process)
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
        return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }

    std::string FileSha256(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) < 0) return {};
        std::array<char, 65536> bytes{};
        auto valid = true;
        while (file.read(bytes.data(), bytes.size()) || file.gcount()) {
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(file.gcount()), 0) < 0) {
                valid = false;
                break;
            }
        }
        std::array<UCHAR, 32> digest{};
        valid = valid && !file.bad() && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
        BCryptDestroyHash(hash);
        if (!valid) return {};
        constexpr auto hex = "0123456789abcdef";
        std::string result;
        for (const auto byte : digest) {
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
        return result;
    }

    namespace {
        struct HandleCloser { void operator()(void* handle) const { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } };
        bool SameUser(const HANDLE process)
        {
            HANDLE remote = nullptr, local = nullptr;
            if (!OpenProcessToken(process, TOKEN_QUERY, &remote)) return false;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &local)) {
                CloseHandle(remote);
                return false;
            }
            alignas(TOKEN_USER) std::array<std::byte, 512> a{}, b{};
            DWORD size = 0;
            const auto valid = GetTokenInformation(remote, TokenUser, a.data(), static_cast<DWORD>(a.size()), &size)
                && GetTokenInformation(local, TokenUser, b.data(), static_cast<DWORD>(b.size()), &size)
                && EqualSid(reinterpret_cast<TOKEN_USER*>(a.data())->User.Sid, reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid);
            CloseHandle(remote);
            CloseHandle(local);
            return valid;
        }

        bool Transfer(const HANDLE pipe, const HANDLE stop, void* bytes, const DWORD count, const bool write)
        {
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!operation.hEvent) return false;
            DWORD transferred = 0;
            const auto immediate = write ? WriteFile(pipe, bytes, count, &transferred, &operation)
                : ReadFile(pipe, bytes, count, &transferred, &operation);
            auto success = immediate != FALSE;
            if (!success && GetLastError() == ERROR_IO_PENDING) {
                const HANDLE events[] = {stop, operation.hEvent};
                success = WaitForMultipleObjects(2, events, FALSE, 2000) == WAIT_OBJECT_0 + 1;
                if (!success) CancelIoEx(pipe, &operation);
                success = GetOverlappedResult(pipe, &operation, &transferred, TRUE) && success;
            }
            CloseHandle(operation.hEvent);
            return success && transferred == count;
        }
    }

    std::optional<Bootstrap> ReadBootstrap(const std::wstring& client)
    {
        const auto name = std::format(L"Local\\GWRL.Bootstrap.v1.{}.{}", GetCurrentProcessId(), client);
        const auto mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (!mapping) return std::nullopt;
        const auto view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Bootstrap));
        Bootstrap descriptor{};
        if (view) {
            memcpy(&descriptor, view, sizeof(descriptor));
            UnmapViewOfFile(view);
        }
        CloseHandle(mapping);
        if (!view || descriptor.magic != Magic || descriptor.major != Major || descriptor.size != sizeof(Bootstrap)
            || descriptor.reserved != 0 || descriptor.published != 1 || descriptor.target_pid != GetCurrentProcessId()
            || descriptor.target_started != ProcessStarted(GetCurrentProcess()) || descriptor.hold_plugins != 0
            || descriptor.session_id[32] || descriptor.transaction_id[64] || descriptor.pipe_name[191]
            || !IsIdentifier(descriptor.session_id, 32) || strlen(descriptor.session_id) != 32
            || (descriptor.transaction_id[0] && !IsIdentifier(descriptor.transaction_id))) return std::nullopt;
        if (!std::wstring_view(descriptor.pipe_name).starts_with(L"\\\\.\\pipe\\GWRL.v1.")) return std::nullopt;
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, descriptor.controller_pid);
        if (!process) return std::nullopt;
        DWORD controller_session = 0, target_session = 0;
        const auto valid = ProcessIdToSessionId(descriptor.controller_pid, &controller_session)
            && ProcessIdToSessionId(descriptor.target_pid, &target_session) && controller_session == target_session
            && descriptor.controller_started != 0
            && ProcessStarted(process) == descriptor.controller_started && SameUser(process);
        CloseHandle(process);
        return valid ? std::optional(descriptor) : std::nullopt;
    }

    Transport::~Transport()
    {
        Stop();
        if (thread_.joinable()) thread_.join();
        if (stop_event_) CloseHandle(stop_event_);
    }

    void Transport::Start()
    {
        if (!stopped_) return;
        if (thread_.joinable()) thread_.join();
        if (stop_event_) CloseHandle(stop_event_);
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event_) return;
        stopped_ = false;
        try { thread_ = std::thread([this] { Run(); }); }
        catch (...) { stopped_ = true; }
    }

    void Transport::Stop()
    {
        if (stop_event_) SetEvent(stop_event_);
    }

    std::optional<Bootstrap> Transport::Connection() const
    {
        std::scoped_lock lock(mutex_);
        return bootstrap_;
    }

    bool Transport::Send(std::string message)
    {
        std::scoped_lock lock(mutex_);
        if (!connected_ || message.empty() || message.size() > MaximumPayload || outgoing_.size() >= MaximumQueue) return false;
        outgoing_.push_back(std::move(message));
        ++pending_writes_;
        return true;
    }

    std::vector<std::string> Transport::Receive()
    {
        std::scoped_lock lock(mutex_);
        std::vector<std::string> result;
        while (!incoming_.empty()) {
            result.push_back(std::move(incoming_.front()));
            incoming_.pop_front();
        }
        return result;
    }

    void Transport::Run()
    {
        try {
        while (WaitForSingleObject(stop_event_, 1000) == WAIT_TIMEOUT) {
            const auto descriptor = ReadBootstrap(client_);
            if (!descriptor) continue;
            const auto pipe = CreateFileW(descriptor->pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) continue;
            const std::unique_ptr<void, HandleCloser> pipe_guard(pipe);
            const auto current = ReadBootstrap(client_);
            ULONG server = 0;
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!current || current->controller_started != descriptor->controller_started
                || strcmp(current->session_id, descriptor->session_id) || !GetNamedPipeServerProcessId(pipe, &server) || server != descriptor->controller_pid
                || !SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
                continue;
            }
            {
                std::scoped_lock lock(mutex_);
                incoming_.clear();
                outgoing_.clear();
                pending_writes_ = 0;
                bootstrap_ = descriptor;
                ++generation_;
                connected_ = true;
            }
            auto healthy = true;
            while (healthy && WaitForSingleObject(stop_event_, 20) == WAIT_TIMEOUT) {
                std::string message;
                {
                    std::scoped_lock lock(mutex_);
                    if (!outgoing_.empty()) {
                        message = std::move(outgoing_.front());
                        outgoing_.pop_front();
                    }
                }
                if (!message.empty()) {
                    const auto size = static_cast<uint32_t>(message.size());
                    std::vector<char> frame(sizeof(size) + message.size());
                    memcpy(frame.data(), &size, sizeof(size));
                    memcpy(frame.data() + sizeof(size), message.data(), message.size());
                    healthy = Transfer(pipe, stop_event_, frame.data(), static_cast<DWORD>(frame.size()), true);
                    --pending_writes_;
                    if (!healthy) break;
                }
                DWORD available = 0, remaining = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, &remaining)) break;
                if (!available) continue;
                if (remaining < 5 || remaining > MaximumPayload + 4) break;
                std::vector<char> frame(remaining);
                if (!Transfer(pipe, stop_event_, frame.data(), remaining, false)) break;
                uint32_t size = 0;
                memcpy(&size, frame.data(), sizeof(size));
                if (size != remaining - 4) break;
                std::scoped_lock lock(mutex_);
                if (incoming_.size() >= MaximumQueue) break;
                incoming_.emplace_back(frame.data() + 4, size);
            }
            connected_ = false;
            std::scoped_lock lock(mutex_);
            outgoing_.clear();
            pending_writes_ = 0;
        }
        } catch (...) { }
        connected_ = false;
        {
            std::scoped_lock lock(mutex_);
            outgoing_.clear();
            pending_writes_ = 0;
        }
        stopped_ = true;
    }
}
