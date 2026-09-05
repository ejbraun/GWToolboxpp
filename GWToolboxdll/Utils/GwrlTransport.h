#pragma once
#include "GwrlProtocol.h"
#include <Windows.h>
#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace Gwrl {
    uint64_t ProcessStarted(HANDLE process);
    std::string FileSha256(const std::filesystem::path& path);
    std::optional<Bootstrap> ReadBootstrap(const std::wstring& client = L"toolbox");

    class Transport {
    public:
        explicit Transport(std::wstring client = L"toolbox") : client_(std::move(client)) { }
        ~Transport();
        void Start();
        void Stop();
        bool Stopped() const { return stopped_.load(); }
        bool Connected() const { return connected_.load(); }
        bool Flushed() const { return pending_writes_.load() == 0; }
        uint64_t Generation() const { return generation_.load(); }
        std::optional<Bootstrap> Connection() const;
        bool Send(std::string message);
        std::vector<std::string> Receive();

    private:
        void Run();
        std::wstring client_;
        std::thread thread_;
        HANDLE stop_event_ = nullptr;
        std::atomic_bool stopped_ = true;
        std::atomic_bool connected_ = false;
        std::atomic_size_t pending_writes_ = 0;
        std::atomic_uint64_t generation_ = 0;
        mutable std::mutex mutex_;
        std::optional<Bootstrap> bootstrap_;
        std::deque<std::string> incoming_;
        std::deque<std::string> outgoing_;
    };
}
