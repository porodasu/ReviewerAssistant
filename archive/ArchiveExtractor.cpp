#include "ArchiveExtractor.h"
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <system_error>
#include <string>
#include <windows.h>

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

    while (true) {
        r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) {
            break;
        }
        if (r != ARCHIVE_OK) {
            if (r == ARCHIVE_FATAL) {
                errorMessage = archive_error_string(a);
                success = false;
                break;
            }
            continue;
        }

        std::filesystem::path entryPath;
        bool pathFound = false;

        const wchar_t* path_w = archive_entry_pathname_w(entry);

        if (path_w && path_w[0] != L'\0') {
            entryPath = std::filesystem::path(path_w);
            pathFound = true;
        }
        else {
            const char* raw_path = archive_entry_pathname(entry);
            if (raw_path && raw_path[0] != '\0') {
                int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw_path, -1, NULL, 0);
                std::wstring wpath;
                if (size > 0) {
                    wpath.resize(size - 1);
                    MultiByteToWideChar(CP_UTF8, 0, raw_path, -1, &wpath[0], size);
                }
                else {
                    size = MultiByteToWideChar(CP_ACP, 0, raw_path, -1, NULL, 0);
                    if (size > 0) {
                        wpath.resize(size - 1);
                        MultiByteToWideChar(CP_ACP, 0, raw_path, -1, &wpath[0], size);
                    }
                }
                entryPath = std::filesystem::path(wpath);
                pathFound = true;
            }
        }

        if (!pathFound || entryPath.empty() || entryPath == L".") {
            archive_read_data_skip(a);
            continue;
        }

        std::filesystem::path targetPath = destPath / entryPath;
        targetPath = targetPath.lexically_normal();

        auto filetype = archive_entry_filetype(entry);

        if (filetype == AE_IFDIR) {
            std::filesystem::create_directories(targetPath, ec);
        }
        else {
            if (!targetPath.parent_path().empty()) {
                std::filesystem::create_directories(targetPath.parent_path(), ec);
            }

            std::ofstream out(targetPath, std::ios::binary);
            if (!out.is_open()) {
                errorMessage = "Не удалось создать файл: " + targetPath.string();
                success = false;
                archive_read_data_skip(a);
                continue;
            }

            char buff[16384];
            la_ssize_t n;
            while ((n = archive_read_data(a, buff, sizeof(buff))) > 0) {
                out.write(buff, n);
            }
        }
    }

    archive_read_free(a);
    return success;
}