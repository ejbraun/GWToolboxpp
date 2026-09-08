#pragma once

#include <string>
#include <string_view>

class ConfigProfiles final {
public:
    static void PrepareInitialProfile();
    static void Initialize();
    static void ProcessGameThreadActions();
    static void ProcessPendingActions();
    static void Terminate();
    static void DrawSettings();

    static bool QueueSaveAsProfile(std::string_view profile_name, std::string* error = nullptr);
    static bool QueueSaveProfile(std::string_view profile_name, std::string* error = nullptr);
    static bool QueueSaveCurrentProfile(std::string* error = nullptr);
    static bool QueueLoadProfile(std::string_view profile_name, std::string* error = nullptr);
    static bool QueueSaveGlobalSettings(std::string* error = nullptr);
    static bool QueueLoadGlobalSettings(std::string* error = nullptr);
};
