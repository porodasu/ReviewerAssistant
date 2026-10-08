#include "ArchiveExtractor.h"

#include <archive.h>
#include <archive_entry.h>

#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    bool isSafePath(const std::filesystem::path& path)
    {
        // Запрещаем абсолютные пути и пути с именем диска.
        if (path.is_absolute() || !path.root_name().empty())
            return false;

        // Запрещаем выход из папки назначения через "..".
        for (const auto& part : path)
        {
            if (part == "..")
                return false;
        }

        return true;
    }

    bool hasReparsePoint(const std::filesystem::path& path)
    {
        #ifdef _WIN32
        std::filesystem::path current;

        if (!path.root_name().empty())
            current = path.root_name();

        if (!path.root_directory().empty())
            current /= path.root_directory();

        for (const auto& part : path.relative_path())
        {
            current /= part;

            DWORD attributes =
                GetFileAttributesW(current.c_str());

            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            {
                return true;
            }
        }
        #endif

        return false;
    }

    std::string archiveError(struct archive* archive)
    {
        const char* message = archive_error_string(archive);

        if (message != nullptr)
            return message;

        return "Неизвестная ошибка libarchive";
    }
}

bool ArchiveExtractor::extract(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& destination,
    std::string& errorMessage)
{

    errorMessage.clear();

    // Проверяем существование архива.
    if (!std::filesystem::exists(archivePath))
    {
        errorMessage = "Архив не найден: " + archivePath.string();
        return false;
    }

    // Создаём папку назначения.
    std::error_code filesystemError;

    std::filesystem::create_directories(destination, filesystemError);

    if (filesystemError)
    {
        errorMessage =
            "Не удалось создать папку назначения: " +
            destination.string() +
            ". " +
            filesystemError.message();

        return false;
    }

    // Создаём объект для чтения архива.
    struct archive* archive = archive_read_new();

    if (archive == nullptr)
    {
        errorMessage = "Не удалось создать объект libarchive.";
        return false;
    }

    // Поддерживаем доступные форматы и фильтры.
    archive_read_support_filter_all(archive);
    archive_read_support_format_all(archive);

    // Открываем архив.
    if (archive_read_open_filename(
        archive,
        archivePath.string().c_str(),
        10240) != ARCHIVE_OK)
    {
        errorMessage =
            "Не удалось открыть архив: " +
            archiveError(archive);

        archive_read_free(archive);
        return false;
    }

    struct archive_entry* entry = nullptr;


    // Читаем все элементы архива.
    while (true)
    {
        int result = archive_read_next_header(archive, &entry);

        if (result == ARCHIVE_EOF)
            break;

        if (result != ARCHIVE_OK)
        {
            errorMessage =
                "Ошибка чтения архива: " +
                archiveError(archive);

            archive_read_free(archive);
            return false;
        }

        const char* entryName = archive_entry_pathname(entry);

        if (entryName == nullptr || entryName[0] == '\0')
        {
            errorMessage = "Архив содержит элемент с пустым именем.";

            archive_read_free(archive);
            return false;
        }

        std::string entryNameString(entryName);

        // Не разрешаем обратный слеш.
        // На Windows он является разделителем пути.
        if (entryNameString.find('\\') != std::string::npos)
        {
            errorMessage =
                "Небезопасный путь в архиве: " +
                entryNameString;

            archive_read_free(archive);
            return false;
        }

        std::filesystem::path relativePath =
            std::filesystem::u8path(entryNameString);

        // Защита от ../../file.txt и абсолютных путей.
        if (!isSafePath(relativePath))
        {
            errorMessage =
                "Небезопасный путь в архиве: " +
                entryNameString;

            archive_read_free(archive);
            return false;
        }

        std::filesystem::path outputPath =
            destination / relativePath;

        if (hasReparsePoint(outputPath))
        {
            errorMessage =
                "Небезопасный путь: обнаружена символическая ссылка "
                "или точка повторного анализа: " +
                outputPath.string();

            archive_read_free(archive);
            return false;
        }

        // Если это папка.
        if (archive_entry_filetype(entry) == AE_IFDIR)
        {
            std::filesystem::create_directories(
                outputPath,
                filesystemError);

            if (filesystemError)
            {
                errorMessage =
                    "Не удалось создать папку: " +
                    outputPath.string() +
                    ". " +
                    filesystemError.message();

                archive_read_free(archive);
                return false;
            }

            continue;
        }

        // Пока поддерживаем обычные файлы.
        if (archive_entry_filetype(entry) != AE_IFREG)
        {
            errorMessage =
                "Неподдерживаемый тип элемента архива: " +
                entryNameString;

            archive_read_free(archive);
            return false;
        }

        // Создаём родительскую папку.
        std::filesystem::create_directories(
            outputPath.parent_path(),
            filesystemError);

        if (filesystemError)
        {
            errorMessage =
                "Не удалось создать папку: " +
                outputPath.parent_path().string() +
                ". " +
                filesystemError.message();

            archive_read_free(archive);
            return false;
        }

        // Не перезаписываем существующие файлы.
        if (std::filesystem::exists(outputPath, filesystemError))
        {
            errorMessage =
                "Файл уже существует: " +
                outputPath.string();

            archive_read_free(archive);
            return false;
        }

        if (filesystemError)
        {
            errorMessage =
                "Не удалось проверить существование файла: " +
                outputPath.string() +
                ". " +
                filesystemError.message();

            archive_read_free(archive);
            return false;
        }

        // Открываем файл для записи.
        std::ofstream outputFile(
            outputPath,
            std::ios::binary);

        if (!outputFile)
        {
            errorMessage =
                "Не удалось создать файл: " +
                outputPath.string();

            archive_read_free(archive);
            return false;
        }

        // Копируем содержимое файла из архива.
        const void* buffer = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;

        while (true)
        {
            int readResult =
                archive_read_data_block(
                    archive,
                    &buffer,
                    &size,
                    &offset);

            if (readResult == ARCHIVE_EOF)
                break;

            if (readResult != ARCHIVE_OK)
            {
                errorMessage =
                    "Ошибка чтения файла из архива: " +
                    entryNameString +
                    ". " +
                    archiveError(archive);

                archive_read_free(archive);
                return false;
            }

            outputFile.write(
                static_cast<const char*>(buffer),
                static_cast<std::streamsize>(size));

            if (!outputFile)
            {
                errorMessage =
                    "Ошибка записи файла: " +
                    outputPath.string();

                archive_read_free(archive);
                return false;
            }
        }

        outputFile.close();

        if (!outputFile)
        {
            errorMessage =
                "Ошибка закрытия файла: " +
                outputPath.string();

            archive_read_free(archive);
            return false;
        }
    }

    archive_read_close(archive);
    archive_read_free(archive);

    return true;
}