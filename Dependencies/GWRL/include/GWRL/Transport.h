#pragma once
#include <GWRL/Wire.h>
#include <Windows.h>
#include <atomic>
#include <array>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace Gwrl {
    uint64_t ProcessStarted(HANDLE process);
    std::string FileSha256(const std::filesystem::path& path);
    std::optional<Bootstrap> ReadBootstrap(const std::wstring& client = L"toolbox");

    struct ReceivedFrame {
        std::string json;
        uint64_t generation = 0;
        std::string recipient;
    };
    enum class Delivery { Queued, Disconnected, Oversized, QueueFull };

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
        bool Send(std::string message, uint64_t generation = 0, uint64_t* ticket = nullptr);
        Delivery SendModule(std::string message, uint64_t generation, uint64_t registration);
        void CancelModule(uint64_t registration = 0);
        bool Flushed(uint64_t ticket, uint64_t generation) const;
        std::vector<ReceivedFrame> ReceiveFrames();
        std::vector<std::string> Receive();

    private:
        void Run();
        struct OutgoingFrame {
            std::string json;
            uint64_t registration = 0;
            uint64_t ticket = 0;
        };
        std::wstring client_;
        std::thread thread_;
        HANDLE stop_event_ = nullptr;
        std::atomic_bool stopped_ = true;
        std::atomic_bool connected_ = false;
        std::atomic_size_t pending_writes_ = 0;
        std::atomic_uint64_t generation_ = 0;
        mutable std::mutex mutex_;
        std::optional<Bootstrap> bootstrap_;
        std::deque<ReceivedFrame> incoming_;
        std::deque<OutgoingFrame> outgoing_;
        uint64_t next_ticket_ = 0, written_ticket_ = 0, last_registration_ = 0;
    };
}
