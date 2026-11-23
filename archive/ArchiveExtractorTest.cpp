#include "ArchiveExtractor.h"

#include <filesystem>
#include <iostream>
#include <string>

int main()
{
    ArchiveExtractor extractor;

    std::string errorMessage;

    const std::filesystem::path archivePath =
        R"(D:\repos\test.zip)";

    const std::filesystem::path destination =
        R"(D:\repos\test_extracted_3)";

    const bool success =
        extractor.extract(
            archivePath,
            destination,
            errorMessage);

    if (success)
    {
        std::cout << "���� ������: ����� ����������."
            << std::endl;

        return 0;
    }

    std::cout << "������: "
        << errorMessage
        << std::endl;

    return 1;
}