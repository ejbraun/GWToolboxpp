#include "ArtifactMetadata.h"
#include "ArtifactVersion.generated.h"
extern "C" __declspec(dllexport) const ToolboxArtifactMetadata* ToolboxArtifactInfo()
{
    static constexpr ToolboxArtifactMetadata info{
        sizeof(ToolboxArtifactMetadata), TB_ARTIFACT_ABI, TB_ARTIFACT_VERSION,
        TB_ARTIFACT_NAME, TB_ARTIFACT_BUILD};
    return &info;
}
