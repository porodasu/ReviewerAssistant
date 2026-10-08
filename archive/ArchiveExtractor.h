#pragma once

#include <filesystem>
#include <string>

class ArchiveExtractor
{
public:
    bool extract(
        const std::filesystem::path& archivePath,
        const std::filesystem::path& destination,
        std::string& errorMessage
    );
};