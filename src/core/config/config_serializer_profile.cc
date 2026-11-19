#include "config_serializer.hh"

#include "c_resource.hh"
#include "logger.hh"
#include "util.hh"

#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <sys/stat.h>
#include <algorithm>
#include <set>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cctype>

namespace VkHook
{
    // --- Per-app profile system ---

    std::string ConfigSerializer::getProfilePath(const std::string& gameName)
    {
        std::string configsDir = getConfigsDir();
        if (configsDir.empty() || !isSafePathComponent(gameName))
            return "";

        return configsDir + "/" + gameName + ".conf";
    }

    std::string ConfigSerializer::ensureGameProfile(const std::string& gameName)
    {
        if (gameName.empty())
            return "";

        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
            return "";

        mkdir(configsDir.c_str(), 0755);

        const std::string profilePath = getProfilePath(gameName);
        if (profilePath.empty())
            return "";

        struct stat st;
        if (stat(profilePath.c_str(), &st) == 0)
        {
            Logger::info("Found existing profile for " + gameName);
            return profilePath;
        }

        // Create default profile with empty effects
        std::string contents = "# VkHook profile for " + gameName + "\n";
        contents += "# Auto-created on first launch\n\n";
        contents += "effects = \n";
        if (!writeAtomically(profilePath, contents))
        {
            Logger::err("Could not create profile for " + gameName + ": " + profilePath);
            return "";
        }

        Logger::info("Created default profile for " + gameName + ": " + profilePath);

        return profilePath;
    }

    std::string ConfigSerializer::getShaderProfilePath(const std::string& gameName, const std::string& profileName)
    {
        const std::string base = getBaseConfigDir();
        if (base.empty() || !isSafePathComponent(gameName) || !isSafePathComponent(profileName))
            return "";
        const std::string shaderDir = base + "/configs/shaders/";
        const std::string gameProfilePath = shaderDir + gameName + "@" + profileName + ".ini";
        const std::string importedPresetPath = shaderDir + profileName + ".ini";
        std::error_code ec;
        if (std::filesystem::exists(gameProfilePath, ec))
            return gameProfilePath;
        ec.clear();
        if (std::filesystem::exists(importedPresetPath, ec))
            return importedPresetPath;
        return gameProfilePath;
    }

    std::string ConfigSerializer::getShaderProfileSidecarPath(const std::string& path)
    {
        if (path.empty())
            return "";
        const auto presetPath = std::filesystem::path(path);
        return (presetPath.parent_path() /
                ("." + presetPath.filename().string() + "_disabled-effectvalues")).string();
    }

    bool ConfigSerializer::isSafePathComponent(const std::string& name)
    {
        if (name.empty() || name == "." || name == "..")
            return false;
        for (const unsigned char c : name)
            if (c < 0x20 || c == 0x7f || c == '/' || c == '\\')
                return false;
        return true;
    }

    std::string ConfigSerializer::getGameShaderProfilePath(const std::string& gameName, const std::string& profileName)
    {
        const std::string base = ConfigSerializer::getBaseConfigDir();
        if (base.empty() || !isSafePathComponent(gameName) || !isSafePathComponent(profileName))
            return "";
        return base + "/configs/shaders/" + gameName + "@" + profileName + ".ini";
    }

    std::string ConfigSerializer::getRenamedShaderProfilePath(const std::string& gameName, const std::string& oldName,
                                                              const std::string& newName)
    {
        const std::string oldPath = getShaderProfilePath(gameName, oldName);
        if (oldPath.empty())
            return "";
        const std::filesystem::path oldFilePath(oldPath);
        const std::string prefix = gameName + "@";
        const bool gameSpecific = oldFilePath.filename().string().compare(0, prefix.size(), prefix) == 0;
        return oldFilePath.parent_path().string() + "/" + (gameSpecific ? prefix + newName : newName) + ".ini";
    }

    std::string ConfigSerializer::getImportedShaderProfilePath(const std::string& name)
    {
        return getBaseConfigDir() + "/configs/shaders/" + name + ".ini";
    }

    std::vector<std::string> ConfigSerializer::listShaderProfilesForGame(const std::string& gameName)
    {
        std::vector<std::string> profiles;
        const std::string base = getBaseConfigDir();
        if (base.empty() || gameName.empty())
            return profiles;
        const std::string dir = base + "/configs/shaders";
        UniqueDir d(opendir(dir.c_str()));
        if (!d)
            return profiles;
        const std::string prefix = gameName + "@";
        struct dirent* entry;
        while ((entry = readdir(d.get())) != nullptr)
        {
            const std::string name = entry->d_name;
            if (name.size() <= 4 || name.substr(name.size() - 4) != ".ini")
                continue;
            if (name.compare(0, prefix.size(), prefix) == 0)
                profiles.push_back(name.substr(prefix.size(), name.size() - prefix.size() - 4));
            else if (name.find('@') == std::string::npos)
                profiles.push_back(name.substr(0, name.size() - 4));
        }
        std::sort(profiles.begin(), profiles.end());
        profiles.erase(std::unique(profiles.begin(), profiles.end()), profiles.end());
        return profiles;
    }

    bool ConfigSerializer::setLastShaderProfile(const std::string& gameName, const std::string& profileName)
    {
        if (!isSafePathComponent(gameName) || !isSafePathComponent(profileName))
            return false;
        const auto path = getBaseConfigDir() + "/configs/shaders/" + gameName + ".last-profile";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        if (ec)
            return false;
        return writeAtomically(path, profileName + "\n");
    }

    std::string ConfigSerializer::getLastShaderProfile(const std::string& gameName)
    {
        if (!isSafePathComponent(gameName))
            return "";
        std::ifstream file(getBaseConfigDir() + "/configs/shaders/" + gameName + ".last-profile");
        std::string profileName;
        std::getline(file, profileName);
        return profileName;
    }

    bool ConfigSerializer::createShaderProfile(const std::string& gameName, const std::string& profileName,
                                               const std::string& copyFromProfile)
    {
        // Creation always targets a game-specific file. The regular lookup
        // intentionally falls back to imported global presets for selection.
        const std::string path = getGameShaderProfilePath(gameName, profileName);
        if (path.empty())
            return false;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        if (ec)
            return false;

        std::string contents = "Techniques=\nTechniqueSorting=\n";
        std::string sourceSidecar;
        if (!copyFromProfile.empty())
        {
            const std::string sourcePath = getShaderProfilePath(gameName, copyFromProfile);
            std::ifstream source(sourcePath, std::ios::binary);
            if (sourcePath.empty() || !source)
                return false;
            contents.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
            if (source.bad())
                return false;
            // an owned profile's sidecar is part of what "inherit" means:
            // without it the copy loses built-ins and applies as foreign
            std::ifstream sidecar(getShaderProfileSidecarPath(sourcePath), std::ios::binary);
            if (sidecar)
                sourceSidecar.assign(std::istreambuf_iterator<char>(sidecar), std::istreambuf_iterator<char>());
        }

        UniqueFd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644));
        if (!fd)
            return false;
        size_t offset = 0;
        bool success = true;
        while (offset < contents.size())
        {
            const ssize_t written = write(fd.get(), contents.data() + offset, contents.size() - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
            {
                success = false;
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (!fd.close())
            success = false;
        if (success && !sourceSidecar.empty())
            success = writeAtomically(getShaderProfileSidecarPath(path), sourceSidecar);
        if (!success)
        {
            unlink(path.c_str());
            std::remove(getShaderProfileSidecarPath(path).c_str());
        }
        return success;
    }

    bool ConfigSerializer::deleteShaderProfile(const std::string& gameName, const std::string& profileName)
    {
        const std::string path = getShaderProfilePath(gameName, profileName);
        if (path.empty())
            return false;
        std::remove(getShaderProfileSidecarPath(path).c_str());
        return std::remove(path.c_str()) == 0;
    }

    bool ConfigSerializer::isValidShaderProfileName(const std::string& name)
    {
        return isSafePathComponent(name) && name.find('@') == std::string::npos;
    }

    bool ConfigSerializer::renameShaderProfile(const std::string& gameName, const std::string& oldName,
                                               const std::string& newName)
    {
        if (gameName.empty() || oldName.empty() || !isValidShaderProfileName(newName))
            return false;
        if (oldName == newName)
            return true;

        const std::string oldPath = getShaderProfilePath(gameName, oldName);
        if (oldPath.empty())
            return false;

        std::error_code ec;
        if (!std::filesystem::exists(oldPath, ec) || ec)
            return false;

        // keep the preset's kind: a game-specific file stays game-specific and
        // an imported global preset stays global
        const std::filesystem::path oldFilePath(oldPath);
        const std::string dir = oldFilePath.parent_path().string();
        const std::string prefix = gameName + "@";
        const std::string newPath = getRenamedShaderProfilePath(gameName, oldName, newName);

        // a name is taken when either kind would collide, not just the same kind
        ec.clear();
        if (std::filesystem::exists(dir + "/" + newName + ".ini", ec) || ec)
            return false;
        ec.clear();
        if (std::filesystem::exists(dir + "/" + prefix + newName + ".ini", ec) || ec)
            return false;

        const std::string oldSidecar = getShaderProfileSidecarPath(oldPath);
        const std::string newSidecar = getShaderProfileSidecarPath(newPath);
        ec.clear();
        const bool hasSidecar = std::filesystem::exists(oldSidecar, ec) && !ec;
        ec.clear();
        if (std::filesystem::exists(newSidecar, ec) || ec)
            return false;

        // the sidecar carries the instance list, so it moves with the ini;
        // move it first and undo both if the ini move fails
        if (hasSidecar && std::rename(oldSidecar.c_str(), newSidecar.c_str()) != 0)
            return false;
        if (std::rename(oldPath.c_str(), newPath.c_str()) != 0)
        {
            if (hasSidecar)
                std::rename(newSidecar.c_str(), oldSidecar.c_str());
            return false;
        }

        // the last-used pointer is keyed by name, so move it too, rolling the
        // renames back if it cannot be written
        if (getLastShaderProfile(gameName) == oldName)
        {
            const std::string lastPath = getBaseConfigDir() + "/configs/shaders/" + gameName + ".last-profile";
            if (!writeAtomically(lastPath, newName + "\n"))
            {
                std::rename(newPath.c_str(), oldPath.c_str());
                if (hasSidecar)
                    std::rename(newSidecar.c_str(), oldSidecar.c_str());
                return false;
            }
        }

        return true;
    }

    std::string ConfigSerializer::importShaderProfile(const std::string& sourcePath)
    {
        if (sourcePath.empty())
            return "";
        std::ifstream source(sourcePath, std::ios::binary);
        if (!source)
            return "";
        const std::string contents{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
        if (source.bad())
            return "";

        const std::string shaderDir = getBaseConfigDir() + "/configs/shaders";
        std::error_code ec;
        std::filesystem::create_directories(shaderDir, ec);
        if (ec)
            return "";

        // '@' separates game from profile in the managed naming scheme, so a
        // global preset carrying one would be invisible to every game.
        std::string base = std::filesystem::path(sourcePath).stem().string();
        std::replace(base.begin(), base.end(), '@', '-');
        std::replace(base.begin(), base.end(), '\\', '-');
        if (base.empty() || base == "." || base == "..")
            base = "imported";

        std::string name = base;
        for (int suffix = 1; ; ++suffix)
        {
            std::error_code existsEc;
            if (!std::filesystem::exists(shaderDir + "/" + name + ".ini", existsEc))
                break;
            name = base + "-" + std::to_string(suffix);
        }
        if (!writeAtomically(getImportedShaderProfilePath(name), contents))
            return "";
        return name;
    }

    bool ConfigSerializer::writeShaderProfile(const std::string& path, const std::string& ini,
                                              const std::string& sidecar)
    {
        if (path.empty())
            return false;
        std::error_code ec;
        if (const auto parent = std::filesystem::path(path).parent_path(); !parent.empty())
            std::filesystem::create_directories(parent, ec);
        if (ec)
            return false;
        // the sidecar is always written, so the ini is recognised as ours and
        // applies its stack exactly on the next load
        return writeAtomically(path, ini) && writeAtomically(getShaderProfileSidecarPath(path), sidecar);
    }

    void ConfigSerializer::readShaderProfile(const std::string& path, std::string& ini, std::string& sidecar)
    {
        auto slurp = [](const std::string& file) {
            std::ifstream stream(file, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        };
        ini = slurp(path);
        sidecar = slurp(getShaderProfileSidecarPath(path));
    }


} // namespace VkHook
