#include "ArchiveExtractor.h"

#include <iostream>
#include <filesystem>

int main()
{
    ArchiveExtractor extractor;

    std::string errorMessage;

    const std::filesystem::path archivePath =
        R"(D:\repos\test.zip)";

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
            R"(D:\repos\test_extracted_2)";

        const bool success =
            extractor.extract(
                archivePath,
                destination,
                errorMessage);

        if (success)
        {
            std::cout << "Тест прошёл: архив распакован."
                << std::endl;
            return 0;
        }

        std::cout << "Ошибка: "
            << errorMessage
            << std::endl;

        return 1;
    }