add_library(plugin_base INTERFACE)
target_sources(plugin_base INTERFACE
    "plugins/Base/dllmain.cpp"
    "plugins/Base/stl.h"
    "plugins/Base/ToolboxPlugin.h"
    "plugins/Base/ToolboxPlugin.cpp"
    "plugins/Base/PluginUtils.h"
    "plugins/Base/PluginUtils.cpp"
    "plugins/Base/ToolboxUIPlugin.h"
    "plugins/Base/ToolboxUIPlugin.cpp"
    "GWToolboxdll/RectF.h"
    "GWToolboxdll/MinimapPlugin.h"
    "GWToolboxdll/ToolboxIni.cpp"
    "GWToolboxdll/Utils/SettingsDoc.cpp")
target_include_directories(plugin_base INTERFACE
    "plugins/Base"
    "GWToolboxdll" # careful here, we only get access to exported and header functions!
    )
target_link_libraries(plugin_base INTERFACE
    imgui
    glaze::glaze
    gwca
    IconFontCppHeaders
    GWToolboxdll # for GetFont
    )
target_compile_definitions(plugin_base INTERFACE BUILD_DLL)

macro(add_tb_plugin PLUGIN)
    add_library(${PLUGIN} SHARED)
    file(GLOB SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/plugins/${PLUGIN}/*.h"
        "${PROJECT_SOURCE_DIR}/plugins/${PLUGIN}/*.cpp")
    target_sources(${PLUGIN} PRIVATE ${SOURCES})
    target_include_directories(${PLUGIN} PRIVATE "${PROJECT_SOURCE_DIR}/plugins/${PLUGIN}")
    target_link_libraries(${PLUGIN} PRIVATE plugin_base)
    target_compile_options(${PLUGIN} PRIVATE /wd4201 /wd4505)
    target_compile_options(${PLUGIN} PRIVATE /Gy)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${PLUGIN} PRIVATE $<$<CONFIG:Debug>:/Z7 /Od>)
        target_link_options(${PLUGIN} PRIVATE /OPT:REF /OPT:ICF /SAFESEH:NO)
        target_link_options(${PLUGIN} PRIVATE $<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO>)
    else()
        target_compile_options(${PLUGIN} PRIVATE /W4 /WX)
        target_compile_options(${PLUGIN} PRIVATE $<$<NOT:$<CONFIG:Debug>>:/GL>)
        target_compile_options(${PLUGIN} PRIVATE $<$<CONFIG:Debug>:/ZI /Od>)
        target_link_options(${PLUGIN} PRIVATE /WX /OPT:REF /OPT:ICF /SAFESEH:NO)
        target_link_options(${PLUGIN} PRIVATE $<$<NOT:$<CONFIG:Debug>>:/LTCG /INCREMENTAL:NO>)
    endif()
    target_link_options(${PLUGIN} PRIVATE $<$<CONFIG:Debug>:/IGNORE:4098 /OPT:NOREF /OPT:NOICF>)
    target_link_options(${PLUGIN} PRIVATE $<$<CONFIG:RelWithDebInfo>:/OPT:NOICF>)
    set_target_properties(${PLUGIN} PROPERTIES FOLDER "plugins/")

    set(_tb_plugin_version "")
    if(${ARGC} GREATER 1)
        set(_tb_plugin_version "${ARGV1}")
    endif()
    set(_tb_manifest_extra)
    if("${PLUGIN}" STREQUAL "DBBox")
        set(_tb_artifact_name "${PLUGIN}")
        set(_tb_manifest_extra -DABI=${GWTOOLBOX_PLUGIN_ABI} -DBUILD_ID=${GWTOOLBOX_FORK_BUILD_ID})
        math(EXPR _tb_version_high "${_tb_plugin_version} >> 16")
        math(EXPR _tb_version_low "${_tb_plugin_version} & 65535")
        configure_file("${PROJECT_SOURCE_DIR}/plugins/Base/ArtifactVersion.generated.h.in"
            "${CMAKE_BINARY_DIR}/generated/${PLUGIN}/ArtifactVersion.generated.h" @ONLY)
        target_include_directories(${PLUGIN} PRIVATE "${CMAKE_BINARY_DIR}/generated/${PLUGIN}")
        target_sources(${PLUGIN} PRIVATE "${PROJECT_SOURCE_DIR}/plugins/Base/ArtifactMetadata.cpp"
            "${PROJECT_SOURCE_DIR}/plugins/Base/ArtifactVersion.rc")
    endif()
    add_custom_command(TARGET ${PLUGIN} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DNAME=${PLUGIN}
                -DDLL=$<TARGET_FILE:${PLUGIN}>
                -DOUTPUT=$<TARGET_FILE_DIR:${PLUGIN}>/${PLUGIN}.version.json
                -DVERSION=${_tb_plugin_version}
                ${_tb_manifest_extra}
                -P "${PROJECT_SOURCE_DIR}/plugins/Base/write-plugin-manifest.cmake"
        COMMENT "Writing ${PLUGIN}.version.json"
        VERBATIM)
endmacro()

option(GWTOOLBOX_BUILD_EXAMPLE_PLUGIN "Build the ExamplePlugin sample (plugin-author reference; skipped in CI to save build time and keep it out of release artifacts)" ON)
if(GWTOOLBOX_BUILD_EXAMPLE_PLUGIN)
    add_tb_plugin(ExamplePlugin)
endif()

add_tb_plugin(DBBox ${DBBOX_PLUGIN_VERSION})

set(DBBOX_FEATURES
    AgentPopTimer
    ArmorSwap
    ChestOpener
    DeathPenaltyTimer
    DhuumCalculator
    Dialogs
    HeartbeatPlugin
    LootNotifier
    PartyReorder
    PitsSoulsWindow
    ProjectileIndicator
    ShadowstepPredictor
    SkinChanger
    Slowload
    SpeedrunScriptingTools
    TargetDetector
    TacticalMinimap
    TrackerAdvanced)

set(DBBOX_SOURCES
    "${PROJECT_SOURCE_DIR}/plugins/Base/AsyncStringDecoder.cpp"
    "${PROJECT_SOURCE_DIR}/plugins/Base/AsyncStringDecoder.h"
    "${PROJECT_SOURCE_DIR}/plugins/Base/BackupManager.cpp"
    "${PROJECT_SOURCE_DIR}/plugins/Base/BackupManager.h"
    "${PROJECT_SOURCE_DIR}/plugins/Base/Pathing.cpp"
    "${PROJECT_SOURCE_DIR}/plugins/Base/Pathing.h"
    "${PROJECT_SOURCE_DIR}/plugins/Base/Rendering.cpp"
    "${PROJECT_SOURCE_DIR}/plugins/Base/Rendering.h")

file(GLOB SCRIPTING_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/plugins/Scripting/*.h"
    "${PROJECT_SOURCE_DIR}/plugins/Scripting/*.cpp")
list(APPEND DBBOX_SOURCES ${SCRIPTING_SOURCES})

foreach(FEATURE IN LISTS DBBOX_FEATURES)
    file(GLOB FEATURE_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/plugins/${FEATURE}/*.h"
        "${PROJECT_SOURCE_DIR}/plugins/${FEATURE}/*.cpp")
    list(APPEND DBBOX_SOURCES ${FEATURE_SOURCES})
    target_include_directories(DBBox PRIVATE "${PROJECT_SOURCE_DIR}/plugins/${FEATURE}")
endforeach()

target_sources(DBBox PRIVATE ${DBBOX_SOURCES})
find_path(DBBOX_EARCUT_INCLUDE_DIR "mapbox/earcut.hpp" REQUIRED)
target_include_directories(DBBox PRIVATE
    "${PROJECT_SOURCE_DIR}/plugins/Scripting"
    "${DBBOX_EARCUT_INCLUDE_DIR}")
target_compile_definitions(DBBox PRIVATE DBBOX_BUILD)
target_compile_options(DBBox PRIVATE "/FI${PROJECT_SOURCE_DIR}/plugins/Base/stl.h")
target_link_libraries(DBBox PRIVATE directxtexloader)

add_custom_command(TARGET DBBox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PROJECT_SOURCE_DIR}/plugins/TrackerAdvanced/profiles"
        "$<TARGET_FILE_DIR:DBBox>/TrackerAdvancedProfiles")

set(SCTRACKER_PLUGIN_VERSION 14 CACHE STRING "SCTracker plugin protocol version (see PluginVersion.generated.h.in)" FORCE)

add_tb_plugin(SCTracker ${SCTRACKER_PLUGIN_VERSION})
target_link_libraries(SCTracker PRIVATE Core RestClient)

configure_file(
    "${PROJECT_SOURCE_DIR}/plugins/SCTracker/PluginVersion.generated.h.in"
    "${CMAKE_BINARY_DIR}/generated/SCTracker/PluginVersion.generated.h"
    @ONLY)
target_include_directories(SCTracker PRIVATE "${CMAKE_BINARY_DIR}/generated/SCTracker")
