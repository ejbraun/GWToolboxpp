#pragma once

#include <Enums.h>

#include <commonIncludes.h>
#include <GWCA/GameEntities/Agent.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>

// Contains information about the current instance which either has to be kept between function calls or is expensive to compute
class InstanceInfo {
public:
    struct MiniPetStatus {
        std::optional<uint32_t> poppedMinipetId = std::nullopt;
        std::chrono::time_point<std::chrono::steady_clock> lastPop = std::chrono::steady_clock::now();
    };

    static InstanceInfo& getInstance() { 
        static InstanceInfo info;
        return info;
    }
    std::string getDecodedAgentName(GW::AgentID);
    std::string getDecodedItemName(uint32_t);
    
    bool canPopAgent() const;
    bool hasMinipetPopped() const;
    std::optional<float> getInstanceProgress() const;
    int getInstanceId() const { return instanceId; }
    void storeTarget(const GW::AgentLiving* agent, int storageId);
    const GW::AgentLiving* retrieveTarget(int storageId) const;
    bool isStoredTarget(const GW::AgentLiving& agent) const;
    DoorStatus getDoorStatus(DoorID id) const { return doorStatus.contains(id) ? doorStatus.at(id) : DoorStatus::Closed; } // Add third state "Unknown"?

    void initialize();
    void terminate();

    InstanceInfo(const InstanceInfo&) = delete;
    InstanceInfo(InstanceInfo&&) = delete;

private:

    InstanceInfo() = default;
    void resetRuntime();
    void onMissionProgress(uint8_t id, float filled, bool created);
    std::unordered_map<GW::AgentID, std::wstring> decodedAgentNames;
    std::unordered_map<uint32_t, std::wstring> decodedItemNames;
    std::unordered_map<int, GW::AgentID> storedTargets;
    
    std::unordered_map<DoorID, DoorStatus> doorStatus;
    int instanceId = 0;
    bool instanceIsCompleted = false;
    MiniPetStatus mpStatus;
    uint64_t decodeGeneration = 0;
    mutable std::mutex missionProgressMutex;
    bool trackingMissionProgress = false;
    std::optional<uint8_t> missionProgressId;
    std::optional<float> missionProgress;
};
