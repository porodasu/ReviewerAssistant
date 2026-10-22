#include "ArchiveExtractor.h"
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <system_error>
#include <string>
#include <vector>
#include <windows.h>

// Вспомогательная функция для безопасной конвертации (исправляет проблему size - 1)
std::wstring SafeConvertToWide(const char* str, UINT codePage, DWORD flags = 0) {
    if (!str || str[0] == '\0') return L"";
    int size = MultiByteToWideChar(codePage, flags, str, -1, NULL, 0);
    if (size <= 0) return L"";

    std::wstring result(size, 0);
    MultiByteToWideChar(codePage, flags, str, -1, &result[0], size);
    result.resize(size - 1); // Убираем нуль-терминатор в конце
    return result;
}

// Проверка на символические ссылки (Reparse Points) в пути
bool IsReparsePoint(const std::filesystem::path& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool ArchiveExtractor::extract(const std::filesystem::path& archivePath,
    const std::filesystem::path& destPath,
    std::string& errorMessage)
{
    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    // Ваше решение для правильной кодировки
    archive_read_set_options(a, "zip:hdrcharset=CP866");

    int r = archive_read_open_filename_w(a, archivePath.wstring().c_str(), 10240);
    if (r != ARCHIVE_OK) {
        errorMessage = archive_error_string(a);
        archive_read_free(a);
        return false;
    }

    struct archive_entry* entry;
    bool success = true;
    std::error_code ec;

    // Получаем абсолютный путь назначения (для защиты от Zip Slip)
    std::filesystem::path absDest = std::filesystem::absolute(destPath).lexically_normal();

    while (true) {
        r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;

        if (r != ARCHIVE_OK) {
            if (r == ARCHIVE_FATAL) {
                errorMessage = archive_error_string(a);
                success = false;
                break;
            }
            continue;
        }

        std::wstring wpath;
        const wchar_t* path_w = archive_entry_pathname_w(entry);

        if (path_w && path_w[0] != L'\0') {
            wpath = path_w;
        }
        else {
            const char* raw_path = archive_entry_pathname(entry);
            wpath = SafeConvertToWide(raw_path, CP_UTF8, MB_ERR_INVALID_CHARS);
            if (wpath.empty()) {
                wpath = SafeConvertToWide(raw_path, CP_ACP);
            }
        }

        if (wpath.empty() || wpath == L".") {
            archive_read_data_skip(a);
            continue;
        }

        std::filesystem::path entryPath(wpath);
        std::filesystem::path targetPath = (destPath / entryPath).lexically_normal();
        std::filesystem::path absTarget = std::filesystem::absolute(targetPath).lexically_normal();

        // 1. ЗАЩИТА ОТ ZIP SLIP (Path Traversal)
        // Проверяем, что целевой путь начинается с пути назначения
        bool isSubPath = true;
        auto itDest = absDest.begin();
        auto itTarget = absTarget.begin();
        for (; itDest != absDest.end(); ++itDest, ++itTarget) {
            if (itTarget == absTarget.end() || *itDest != *itTarget) {
                isSubPath = false;
                break;
            }
        }

        if (!isSubPath) {
            errorMessage = "Ошибка безопасности: попытка записи за пределы папки назначения.";
            success = false;
            archive_read_data_skip(a);
            continue;
        }

        // 5. ЗАЩИТА ОТ REPARSE POINTS (Символических ссылок)
        // Проверяем, не является ли родительская папка ссылкой
        if (std::filesystem::exists(targetPath.parent_path())) {
            if (IsReparsePoint(targetPath.parent_path())) {
                errorMessage = "Ошибка безопасности: попытка записи через символическую ссылку.";
                success = false;
                archive_read_data_skip(a);
                continue;
            }
        }

        auto filetype = archive_entry_filetype(entry);

        if (filetype == AE_IFDIR) {
            std::filesystem::create_directories(targetPath, ec);
            // 4. Проверка ошибок создания директории
            if (ec) {
                errorMessage = "Ошибка создания папки: " + ec.message();
                success = false;
            }
        }
        else {
            if (!targetPath.parent_path().empty()) {
                std::filesystem::create_directories(targetPath.parent_path(), ec);
                if (ec) {
                    errorMessage = "Ошибка создания родительской папки: " + ec.message();
                    success = false;
                    archive_read_data_skip(a);
                    continue;
                }
            }

            // 2. ЗАЩИТА ОТ ПЕРЕЗАПИСИ СУЩЕСТВУЮЩИХ ФАЙЛОВ
            if (std::filesystem::exists(targetPath)) {
                errorMessage = "Файл уже существует, пропущен: " + targetPath.string();
                success = false; // Помечаем как false, чтобы пользователь знал, что не всё распаковалось
                archive_read_data_skip(a);
                continue;
            }

            std::ofstream out(targetPath, std::ios::binary | std::ios::out);
            // 4. Проверка открытия файла
            if (!out.is_open()) {
                errorMessage = "Не удалось создать файл: " + targetPath.string();
                success = false;
                archive_read_data_skip(a);
                continue;
            }

            char buff[16384];
            la_ssize_t n;
            bool writeError = false;

            while ((n = archive_read_data(a, buff, sizeof(buff))) > 0) {
                out.write(buff, n);
                // 4. Проверка ошибок записи данных
                if (!out.good()) {
                    writeError = true;
                    break;
                }
            }

            if (writeError) {
                errorMessage = "Ошибка записи в файл: " + targetPath.string();
                success = false;
            }
        }
    }

    archive_read_free(a);
    return success;
}