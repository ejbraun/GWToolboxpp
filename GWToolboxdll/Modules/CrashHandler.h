#pragma once

#include <ToolboxModule.h>

class CrashHandler : public ToolboxModule {
    CrashHandler() = default;
    ~CrashHandler() = default;
public:
    static CrashHandler& Instance()
    {
        static CrashHandler instance;
        return instance;
    }

    [[nodiscard]] const char* Name() const override { return "Crash Handler"; }
    bool HasSettings() override { return false; }

    void Initialize() override;
    static void SetPluginDiagnostics(std::string diagnostics);
    static void SetUpdateDiagnostics(std::string diagnostics);

    void Terminate() override;

    static void GWCAPanicHandler(
        void*,
        const char* expr,
        const char* file,
        unsigned int line,
        const char* function);
    static void FatalAssert(const char* expr, const char* file, const unsigned line);

    static LONG WINAPI Crash(EXCEPTION_POINTERS* pExceptionPointers, const char* extra_info = nullptr);
};
