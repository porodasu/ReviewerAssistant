#pragma once

#include <string>
#include <filesystem>

class ArchiveExtractor
{
public:
    bool extract(const std::filesystem::path& archivePath,
                 const std::filesystem::path& destPath,
                 std::string& errorMessage);
};