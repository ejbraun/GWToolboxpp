// No stdafx: this file is also compiled into plugin DLLs (see cmake/gwtoolboxdll_plugins.cmake).
#include <Utils/SettingsDoc.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <Utils/FilePersistence.h>

namespace {
    // Canonical section key, also its <section>.json filename stem, so forbidden chars are stripped.
    std::string SanitiseSection(const std::string_view section)
    {
        std::string out;
        out.reserve(section.size());
        for (const char c : section) {
            if (std::string_view(R"(<>:"/\|?*)").find(c) == std::string_view::npos && static_cast<unsigned char>(c) >= 0x20) {
                out += c;
            }
        }
        return out;
    }

    std::filesystem::path SectionFilename(const std::string_view section)
    {
        const std::string name = SanitiseSection(section);
        auto filename = std::filesystem::path(std::u8string(name.begin(), name.end()));
        filename += L".json";
        return filename;
    }
}

bool SettingsDoc::LoadFile(const std::filesystem::path& path)
{
    std::string buffer;
    auto file_exists = false;
    {
        const FilePersistence::ScopedConfigLock config_lock;
        if (!config_lock.Acquired()) return false;
        std::error_code ec;
        file_exists = std::filesystem::exists(path, ec);
        if (ec) return false;
        if (file_exists) {
            std::ifstream file(path, std::ios::binary);
            if (!file) return false;
            buffer.assign(std::istreambuf_iterator(file), {});
            if (file.bad()) return false;
        }
    }

    decltype(sections) staged;
    if (file_exists && !buffer.empty() && glz::read<lenient_opts>(staged, buffer)) return false;
    sections = std::move(staged);
    location_on_disk = path;
    return true;
}

bool SettingsDoc::SaveFile(const std::filesystem::path& path) const
{
    std::string buffer;
    if (glz::write<glz::opts{.prettify = true}>(sections, buffer)) return false;
    const FilePersistence::ScopedConfigLock config_lock;
    if (!config_lock.Acquired()) return false;
    std::string error;
    return FilePersistence::AtomicWrite(path, buffer, error);
}

bool SettingsDoc::LoadFolder(const std::filesystem::path& folder)
{
    std::vector<std::pair<std::string, std::string>> files;
    {
        const FilePersistence::ScopedConfigLock config_lock;
        if (!config_lock.Acquired()) return false;
        std::error_code ec;
        const auto is_directory = std::filesystem::is_directory(folder, ec);
        if (ec) return false;
        if (is_directory) {
            for (std::filesystem::directory_iterator it(folder, ec), end; it != end && !ec; it.increment(ec)) {
                const auto& entry = *it;
                if (!entry.is_regular_file(ec)) {
                    if (ec) return false;
                    continue;
                }
                if (entry.path().extension() != L".json") continue;
                std::ifstream file(entry.path(), std::ios::binary);
                if (!file) return false;
                auto buffer = std::string(std::istreambuf_iterator(file), {});
                if (file.bad()) return false;
                const auto stem = entry.path().stem().u8string();
                files.emplace_back(std::string(stem.begin(), stem.end()), std::move(buffer));
            }
            if (ec) return false;
        }
    }

    decltype(sections) staged;
    for (auto& [stem, buffer] : files) {
        Section section;
        if (!buffer.empty() && glz::read<lenient_opts>(section, buffer)) return false;
        staged.insert_or_assign(std::move(stem), std::move(section));
    }
    sections = std::move(staged);
    location_on_disk = folder;
    return true;
}

bool SettingsDoc::SaveFolder(const std::filesystem::path& folder) const
{
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    files.reserve(sections.size());
    for (const auto& [name, section] : sections) {
        std::string buffer;
        if (glz::write<glz::opts{.prettify = true}>(section, buffer)) return false;
        files.emplace_back(folder / SectionFilename(name), std::move(buffer));
    }

    const FilePersistence::ScopedConfigLock config_lock;
    if (!config_lock.Acquired()) return false;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) return false;
    bool ok = true;
    for (const auto& [path, buffer] : files) {
        std::string error;
        ok = FilePersistence::AtomicWrite(path, buffer, error) && ok;
    }
    return ok;
}

bool SettingsDoc::Has(const std::string_view section, const std::string_view key) const
{
    return FindKey(section, key) != nullptr;
}

bool SettingsDoc::HasSection(const std::string_view section) const
{
    return FindSection(section) != nullptr;
}

void SettingsDoc::EraseSection(const std::string_view section)
{
    const auto found = sections.find(SanitiseSection(section));
    if (found != sections.end()) {
        sections.erase(found);
    }
}

void SettingsDoc::EraseKey(const std::string_view section, const std::string_view key)
{
    const auto found = sections.find(SanitiseSection(section));
    if (found == sections.end()) {
        return;
    }
    const auto key_found = found->second.find(key);
    if (key_found != found->second.end()) {
        found->second.erase(key_found);
    }
}

const SettingsDoc::Section* SettingsDoc::FindSection(const std::string_view section) const
{
    const auto found = sections.find(SanitiseSection(section));
    return found != sections.end() ? &found->second : nullptr;
}

const glz::raw_json* SettingsDoc::FindKey(const std::string_view section, const std::string_view key) const
{
    const auto* sec = FindSection(section);
    if (!sec) {
        return nullptr;
    }
    const auto found = sec->find(key);
    return found != sec->end() ? &found->second : nullptr;
}

SettingsDoc::Section& SettingsDoc::GetOrCreateSection(const std::string_view section)
{
    std::string name = SanitiseSection(section);
    const auto found = sections.find(name);
    if (found != sections.end()) {
        return found->second;
    }
    return sections.emplace(std::move(name), Section{}).first->second;
}
