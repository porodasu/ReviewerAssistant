#include "ArchiveExtractor.h"
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <system_error>
#include <string>
#include <windows.h>

std::wstring SafeConvertToWide(const char* str, UINT codePage, DWORD flags = 0) {
    if (!str || str[0] == '\0') return L"";
    int size = MultiByteToWideChar(codePage, flags, str, -1, NULL, 0);
    if (size <= 0) return L"";

    std::wstring result(size, 0);
    MultiByteToWideChar(codePage, flags, str, -1, &result[0], size);
    result.resize(size - 1);
    return result;
}

bool HasReparsePointInPath(const std::filesystem::path& basePath, const std::filesystem::path& targetPath) {

    DWORD baseAttrs = GetFileAttributesW(basePath.c_str());
    if (baseAttrs != INVALID_FILE_ATTRIBUTES && (baseAttrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return true;
    }

    auto relPath = targetPath.lexically_relative(basePath);
    if (relPath.empty()) return false;

    std::filesystem::path currentPath = basePath;
    for (const auto& part : relPath) {
        if (part == ".") continue;


        if (part == "..") return true;

        currentPath /= part;

        DWORD attrs = GetFileAttributesW(currentPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return true;
        }
    }
    return false;
}

bool ArchiveExtractor::extract(const std::filesystem::path& archivePath,
    const std::filesystem::path& destPath,
    std::string& errorMessage)
{
    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

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

    std::filesystem::path absDest = std::filesystem::absolute(destPath).lexically_normal();

    while (true) {
        r = archive_read_next_header(a, &entry);

        if (r == ARCHIVE_EOF) break;

        if (r == ARCHIVE_FATAL) {
            errorMessage = "Фатальная ошибка чтения архива: " + std::string(archive_error_string(a));
            success = false;
            break;
        }

        if (r == ARCHIVE_WARN || r == ARCHIVE_FAILED) {
            errorMessage = "Предупреждение при чтении заголовка: " + std::string(archive_error_string(a));
            success = false;
            archive_read_data_skip(a);
            continue;
        }

        if (r != ARCHIVE_OK) {
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
            if (wpath.empty()) wpath = SafeConvertToWide(raw_path, CP_ACP);
        }

        if (wpath.empty() || wpath == L".") {
            archive_read_data_skip(a);
            continue;
        }

        std::filesystem::path entryPath(wpath);
        std::filesystem::path targetPath = (destPath / entryPath).lexically_normal();
        std::filesystem::path absTarget = std::filesystem::absolute(targetPath).lexically_normal();

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

        if (HasReparsePointInPath(absDest, absTarget)) {
            errorMessage = "Ошибка безопасности: попытка записи через символическую ссылку.";
            success = false;
            archive_read_data_skip(a);
            continue;
        }

        auto filetype = archive_entry_filetype(entry);

        if (filetype == AE_IFDIR) {
            std::filesystem::create_directories(targetPath, ec);
            if (ec) {
                errorMessage = "Ошибка создания папки: " + ec.message();
                success = false;
            }
        }
        else if (filetype == AE_IFREG) {
            if (!targetPath.parent_path().empty()) {
                std::filesystem::create_directories(targetPath.parent_path(), ec);
                if (ec) {
                    errorMessage = "Ошибка создания родительской папки: " + ec.message();
                    success = false;
                    archive_read_data_skip(a);
                    continue;
                }
            }

            if (std::filesystem::exists(targetPath)) {
                errorMessage = "Файл уже существует, пропущен: " + targetPath.string();
                success = false;
                archive_read_data_skip(a);
                continue;
            }

            std::ofstream out(targetPath, std::ios::binary | std::ios::out);
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
                if (!out.good()) {
                    writeError = true;
                    break;
                }
            }

            if (n < 0) {
                errorMessage = "Ошибка чтения данных из архива: " + std::string(archive_error_string(a));
                writeError = true;
            }

            out.close();
            if (!out.good()) {
                writeError = true;
                if (n >= 0) {
                    errorMessage = "Ошибка записи файла (возможно, диск заполнен): " + targetPath.string();
                }
            }


            if (writeError) {
                success = false;
                std::filesystem::remove(targetPath, ec);
                archive_read_data_skip(a);
                continue;
            }
        }
        else {
            errorMessage = "Отклонен элемент архива неизвестного типа: " + targetPath.string();
            success = false;
            archive_read_data_skip(a);
            continue;
        }
    }

    archive_read_free(a);
    return success;
}