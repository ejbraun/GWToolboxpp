#pragma once
#include <cstdint>
struct ToolboxArtifactMetadata {
    uint32_t size;
    uint32_t abi;
    uint32_t version;
    const char* name;
    const char* build;
};
using ToolboxArtifactMetadataFn = const ToolboxArtifactMetadata* (*)();
