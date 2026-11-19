#include "ArchiveExtractor.h"

#include <archive.h>
#include <archive_entry.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <climits>
#include <exception>
#include <cwctype>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    constexpr int MAX_ARCHIVE_DEPTH = 3;
    constexpr std::size_t COPY_BUFFER_SIZE = 16 * 1024;

    struct ArchiveDeleter
    {
        void operator()(struct archive* value) const noexcept
        {
            if (value != nullptr)
                archive_read_free(value);
        }
    };

    using ArchivePtr = std::unique_ptr<struct archive, ArchiveDeleter>;

    void AppendError(std::string& destination, const std::string& message)
    {
        if (message.empty())
            return;

        if (!destination.empty())
            destination += "\n";

        destination += message;
    }

    std::string GetArchiveError(struct archive* archiveHandle)
    {
        const char* message = archiveHandle
            ? archive_error_string(archiveHandle)
            : nullptr;

        return (message != nullptr && message[0] != '\0')
            ? std::string(message)
            : std::string("неизвестная ошибка LibArchive");
    }

    std::string PathToUtf8(const std::filesystem::path& path)
    {
        const auto value = path.u8string(); // C++17: std::string в UTF-8
        return std::string(value.begin(), value.end());
    }

    std::wstring SafeConvertToWide(const char* str, UINT codePage, DWORD flags = 0)
    {
        if (str == nullptr || str[0] == '\0')
            return L"";

        const int requiredSize = MultiByteToWideChar(
            codePage, flags, str, -1, nullptr, 0);

        if (requiredSize <= 0)
            return L"";

        std::wstring result(static_cast<std::size_t>(requiredSize), L'\0');

        const int convertedSize = MultiByteToWideChar(
            codePage,
            flags,
            str,
            -1,
            result.data(),
            requiredSize);

        if (convertedSize <= 0)
            return L"";

        result.resize(static_cast<std::size_t>(convertedSize));
        if (!result.empty() && result.back() == L'\0')
            result.pop_back();

        return result;
    }

    std::wstring GetArchiveEntryPathWide(struct archive_entry* entry)
    {
        if (entry == nullptr)
            return L"";

        // Prefer the explicit UTF-8 accessor. It avoids relying on the active
        // Windows code page for entries whose ZIP metadata declares UTF-8.
        const char* utf8Path = archive_entry_pathname_utf8(entry);
        if (utf8Path != nullptr && utf8Path[0] != '\0')
        {
            std::wstring converted = SafeConvertToWide(
                utf8Path, CP_UTF8, MB_ERR_INVALID_CHARS);
            if (!converted.empty())
                return converted;
        }

        // For legacy ZIP names, libarchive may expose a wide path after
        // applying zip:hdrcharset=CP866. Keep this fallback for compatibility
        // with the project's existing CP866 archives.
        const wchar_t* widePath = archive_entry_pathname_w(entry);
        if (widePath != nullptr && widePath[0] != L'\0')
            return std::wstring(widePath);

        // Last resort for formats/builds where neither accessor above can
        // provide a path. UTF-8 is attempted first, then the Windows ACP.
        const char* rawPath = archive_entry_pathname(entry);
        if (rawPath == nullptr || rawPath[0] == '\0')
            return L"";

        std::wstring converted = SafeConvertToWide(
            rawPath, CP_UTF8, MB_ERR_INVALID_CHARS);
        if (!converted.empty())
            return converted;

        return SafeConvertToWide(rawPath, CP_ACP);
    }

    bool HasReparsePointInPath(
        const std::filesystem::path& basePath,
        const std::filesystem::path& targetPath)
    {
        const DWORD baseAttributes = GetFileAttributesW(basePath.c_str());
        if (baseAttributes != INVALID_FILE_ATTRIBUTES &&
            (baseAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            return true;
        }

        const auto relativePath = targetPath.lexically_relative(basePath);
        if (relativePath.empty())
            return false;

        std::filesystem::path currentPath = basePath;

        for (const auto& part : relativePath)
        {
            if (part == L".")
                continue;

            if (part == L"..")
                return true;

            currentPath /= part;

            const DWORD attributes = GetFileAttributesW(currentPath.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                return true;
            }
        }

        return false;
    }

    bool IsArchiveExtension(const std::filesystem::path& path)
    {
        std::wstring extension = path.extension().wstring();
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });

        static const std::vector<std::wstring> archiveExtensions =
        {
            L".zip", L".tar", L".gz", L".tgz", L".7z", L".rar",
            L".bz2", L".xz", L".cab", L".iso"
        };

        return std::find(
            archiveExtensions.begin(),
            archiveExtensions.end(),
            extension) != archiveExtensions.end();
    }

    bool SkipCurrentEntry(
        struct archive* archiveHandle,
        std::string& errorMessage,
        const std::string& context)
    {
        const int result = archive_read_data_skip(archiveHandle);
        if (result == ARCHIVE_OK || result == ARCHIVE_EOF)
            return true;

        AppendError(
            errorMessage,
            context + ": " + GetArchiveError(archiveHandle));
        return false;
    }

    bool PathComponentsEqual(
        const std::filesystem::path& left,
        const std::filesystem::path& right)
    {
        const std::wstring& leftValue = left.native();
        const std::wstring& rightValue = right.native();

        if (leftValue.size() > static_cast<std::size_t>(INT_MAX) ||
            rightValue.size() > static_cast<std::size_t>(INT_MAX))
        {
            return false;
        }

        return CompareStringOrdinal(
            leftValue.c_str(),
            static_cast<int>(leftValue.size()),
            rightValue.c_str(),
            static_cast<int>(rightValue.size()),
            TRUE) == CSTR_EQUAL;
    }

    bool IsSubPath(
        const std::filesystem::path& basePath,
        const std::filesystem::path& targetPath)
    {
        // Windows paths are case-insensitive. A component-by-component
        // comparison must therefore also ignore case.
        auto baseIterator = basePath.begin();
        auto targetIterator = targetPath.begin();

        for (; baseIterator != basePath.end(); ++baseIterator, ++targetIterator)
        {
            if (targetIterator == targetPath.end() ||
                !PathComponentsEqual(*baseIterator, *targetIterator))
            {
                return false;
            }
        }

        return true;
    }

    std::wstring ToUpperAscii(std::wstring value)
    {
        for (wchar_t& character : value)
        {
            if (character >= L'a' && character <= L'z')
                character = static_cast<wchar_t>(character - L'a' + L'A');
        }

        return value;
    }

    bool IsReservedWindowsDeviceName(const std::wstring& component)
    {
        // Windows reserves these names even when an extension is appended,
        // for example CON.txt or LPT1.log.
        const std::size_t dotPosition = component.find(L'.');
        std::wstring baseName = component.substr(0, dotPosition);

        while (!baseName.empty() &&
            (baseName.back() == L' ' || baseName.back() == L'.'))
        {
            baseName.pop_back();
        }

        baseName = ToUpperAscii(std::move(baseName));

        if (baseName == L"CON" || baseName == L"PRN" ||
            baseName == L"AUX" || baseName == L"NUL" ||
            baseName == L"CONIN$" || baseName == L"CONOUT$")
        {
            return true;
        }

        for (int number = 1; number <= 9; ++number)
        {
            const wchar_t digit = static_cast<wchar_t>(L'0' + number);
            if (baseName == std::wstring(L"COM") + digit ||
                baseName == std::wstring(L"LPT") + digit)
            {
                return true;
            }
        }

        // Windows also treats the superscript digits ¹, ² and ³ as device
        // name suffixes for COM and LPT.
        static const wchar_t* superscriptDigits[] =
        {
            L"\u00B9", L"\u00B2", L"\u00B3"
        };

        for (const wchar_t* digit : superscriptDigits)
        {
            if (baseName == std::wstring(L"COM") + digit ||
                baseName == std::wstring(L"LPT") + digit)
            {
                return true;
            }
        }

        return false;
    }

    bool IsSafeWindowsRelativePath(
        const std::filesystem::path& entryPath,
        std::string& reason)
    {
        reason.clear();

        // Reject drive-qualified, rooted, UNC and device paths before any
        // destination path is constructed.
        if (entryPath.is_absolute() || entryPath.has_root_name() ||
            entryPath.has_root_directory())
        {
            reason = "путь является абсолютным или содержит корень Windows";
            return false;
        }

        for (const auto& part : entryPath)
        {
            const std::wstring component = part.native();

            if (component.empty() || component == L".")
                continue;

            if (component == L"..")
            {
                reason = "путь содержит компонент '..'";
                return false;
            }

            // These characters are forbidden in ordinary Windows file and
            // directory names. A colon is also forbidden to block NTFS ADS.
            if (component.find_first_of(L"<>:\"|?*") != std::wstring::npos)
            {
                reason = "имя содержит недопустимый символ Windows или NTFS-поток";
                return false;
            }

            for (wchar_t character : component)
            {
                if (character >= 0 && character < 32)
                {
                    reason = "имя содержит управляющий символ";
                    return false;
                }
            }

            if (component.back() == L'.' || component.back() == L' ')
            {
                reason = "имя заканчивается точкой или пробелом";
                return false;
            }

            if (IsReservedWindowsDeviceName(component))
            {
                reason = "использовано зарезервированное системное имя Windows";
                return false;
            }
        }

        return true;
    }

    bool CheckPathExists(
        const std::filesystem::path& path,
        bool& pathExists,
        std::error_code& error)
    {
        error.clear();
        const auto status = std::filesystem::symlink_status(path, error);

        if (error == std::errc::no_such_file_or_directory)
        {
            error.clear();
            pathExists = false;
            return true;
        }

        if (error)
            return false;

        pathExists = status.type() != std::filesystem::file_type::not_found;
        return true;
    }

    bool ValidateDestinationDirectory(
        const std::filesystem::path& destinationPath,
        bool& destinationExists,
        std::string& errorMessage)
    {
        std::error_code error;
        if (!CheckPathExists(destinationPath, destinationExists, error))
        {
            AppendError(
                errorMessage,
                "Не удалось проверить каталог назначения " +
                PathToUtf8(destinationPath) + ": " + error.message());
            return false;
        }

        if (!destinationExists)
            return true;

        const auto status = std::filesystem::symlink_status(destinationPath, error);
        if (error)
        {
            AppendError(
                errorMessage,
                "Не удалось получить сведения о каталоге назначения " +
                PathToUtf8(destinationPath) + ": " + error.message());
            return false;
        }

        if (!std::filesystem::is_directory(status))
        {
            AppendError(
                errorMessage,
                "Путь назначения уже существует, но не является каталогом: " +
                PathToUtf8(destinationPath));
            return false;
        }

        const DWORD attributes = GetFileAttributesW(destinationPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            AppendError(
                errorMessage,
                "Каталог назначения недоступен или является Reparse Point: " +
                PathToUtf8(destinationPath));
            return false;
        }

        std::filesystem::directory_iterator iterator(destinationPath, error);
        if (error)
        {
            AppendError(
                errorMessage,
                "Не удалось проверить содержимое каталога назначения " +
                PathToUtf8(destinationPath) + ": " + error.message());
            return false;
        }

        if (iterator != std::filesystem::directory_iterator())
        {
            AppendError(
                errorMessage,
                "Каталог назначения уже содержит файлы. Для безопасной распаковки "
                "укажите новый или пустой каталог: " + PathToUtf8(destinationPath));
            return false;
        }

        return true;
    }

    bool CreateTemporaryDirectory(
        const std::filesystem::path& parentPath,
        std::filesystem::path& temporaryPath,
        std::string& errorMessage)
    {
        static std::atomic<unsigned long long> counter{ 0 };

        // The directory is created atomically. The process ID plus an
        // incrementing counter makes collisions very unlikely; create_directory
        // remains the final authority and safely handles an existing name.
        for (int attempt = 0; attempt < 128; ++attempt)
        {
            const unsigned long long value =
                counter.fetch_add(1, std::memory_order_relaxed);

            const std::wstring directoryName =
                L".archive_extract_" +
                std::to_wstring(GetCurrentProcessId()) + L"_" +
                std::to_wstring(value);

            const std::filesystem::path candidate = parentPath / directoryName;
            std::error_code error;
            const bool created = std::filesystem::create_directory(candidate, error);

            if (created)
            {
                temporaryPath = candidate;
                return true;
            }

            if (error && error != std::errc::file_exists)
            {
                AppendError(
                    errorMessage,
                    "Не удалось создать временный каталог " +
                    PathToUtf8(candidate) + ": " + error.message());
                return false;
            }
        }

        AppendError(
            errorMessage,
            "Не удалось подобрать свободное имя для временного каталога распаковки.");
        return false;
    }

    class TemporaryDirectoryGuard
    {
    public:
        explicit TemporaryDirectoryGuard(std::filesystem::path path)
            : path_(std::move(path))
        {
        }

        TemporaryDirectoryGuard(const TemporaryDirectoryGuard&) = delete;
        TemporaryDirectoryGuard& operator=(const TemporaryDirectoryGuard&) = delete;

        ~TemporaryDirectoryGuard()
        {
            if (active_)
            {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }
        }

        bool cleanup(std::string& errorMessage)
        {
            if (!active_)
                return true;

            std::error_code error;
            std::filesystem::remove_all(path_, error);

            if (error)
            {
                AppendError(
                    errorMessage,
                    "Не удалось удалить временный каталог " +
                    PathToUtf8(path_) + ": " + error.message());
                return false;
            }

            active_ = false;
            return true;
        }

        void release() noexcept
        {
            active_ = false;
        }

    private:
        std::filesystem::path path_;
        bool active_ = true;
    };

    bool FlattenSingleRootDirectory(
        const std::filesystem::path& extractionRoot,
        const std::wstring& archiveStem,
        std::string& errorMessage)
    {
        const std::filesystem::path innerDirectory = extractionRoot / archiveStem;
        std::error_code error;
        const auto innerStatus = std::filesystem::symlink_status(innerDirectory, error);

        if (error == std::errc::no_such_file_or_directory)
        {
            error.clear();
            return true;
        }

        if (error)
        {
            AppendError(
                errorMessage,
                "Не удалось проверить каталог верхнего уровня " +
                PathToUtf8(innerDirectory) + ": " + error.message());
            return false;
        }

        if (!std::filesystem::is_directory(innerStatus))
            return true;

        // Preserve the previous behavior: flatten only when the archive has
        // exactly one top-level directory and its name matches the archive.
        std::filesystem::directory_iterator rootIterator(extractionRoot, error);
        if (error)
        {
            AppendError(
                errorMessage,
                "Не удалось просмотреть временный каталог " +
                PathToUtf8(extractionRoot) + ": " + error.message());
            return false;
        }

        const std::filesystem::directory_iterator end;
        bool onlyExpectedDirectory = true;

        for (; rootIterator != end; rootIterator.increment(error))
        {
            if (error)
                break;

            if (rootIterator->path().lexically_normal() !=
                innerDirectory.lexically_normal())
            {
                onlyExpectedDirectory = false;
                break;
            }
        }

        if (error)
        {
            AppendError(
                errorMessage,
                "Ошибка просмотра временного каталога " +
                PathToUtf8(extractionRoot) + ": " + error.message());
            return false;
        }

        if (!onlyExpectedDirectory)
            return true;

        std::vector<std::filesystem::path> children;
        std::filesystem::directory_iterator childIterator(innerDirectory, error);
        if (error)
        {
            AppendError(
                errorMessage,
                "Не удалось прочитать содержимое " +
                PathToUtf8(innerDirectory) + ": " + error.message());
            return false;
        }

        for (; childIterator != end; childIterator.increment(error))
        {
            if (error)
                break;
            children.push_back(childIterator->path());
        }

        if (error)
        {
            AppendError(
                errorMessage,
                "Ошибка чтения содержимого " +
                PathToUtf8(innerDirectory) + ": " + error.message());
            return false;
        }

        // Check all destinations before moving anything. Since the extraction
        // root contains only innerDirectory, this also prevents accidental
        // replacement if the filesystem changed unexpectedly.
        for (const auto& child : children)
        {
            const std::filesystem::path destination =
                extractionRoot / child.filename();
            bool destinationExists = false;

            if (!CheckPathExists(destination, destinationExists, error))
            {
                AppendError(
                    errorMessage,
                    "Не удалось проверить путь " + PathToUtf8(destination) +
                    ": " + error.message());
                return false;
            }

            if (destinationExists)
            {
                AppendError(
                    errorMessage,
                    "Невозможно объединить содержимое вложенного архива: " +
                    PathToUtf8(destination) + " уже существует.");
                return false;
            }
        }

        for (const auto& child : children)
        {
            const std::filesystem::path destination =
                extractionRoot / child.filename();
            error.clear();
            std::filesystem::rename(child, destination, error);

            if (error)
            {
                AppendError(
                    errorMessage,
                    "Не удалось переместить " + PathToUtf8(child) +
                    " в " + PathToUtf8(destination) + ": " + error.message());
                return false;
            }
        }

        error.clear();
        const bool removed = std::filesystem::remove(innerDirectory, error);
        if (error || !removed)
        {
            AppendError(
                errorMessage,
                "Не удалось удалить пустой каталог " +
                PathToUtf8(innerDirectory) +
                (error ? ": " + error.message() : ": каталог не был удалён."));
            return false;
        }

        return true;
    }
}

bool ArchiveExtractor::extract(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& destPath,
    std::string& errorMessage)
{
    errorMessage.clear();

    try
    {
        std::error_code filesystemError;
        const std::filesystem::path absoluteDestination =
            std::filesystem::absolute(destPath, filesystemError).lexically_normal();

        if (filesystemError)
        {
            errorMessage =
                "Не удалось определить абсолютный путь каталога назначения: " +
                filesystemError.message();
            return false;
        }

        const std::filesystem::path destinationParent =
            absoluteDestination.parent_path();

        if (destinationParent.empty() || absoluteDestination.filename().empty())
        {
            errorMessage = "Указан некорректный путь каталога назначения.";
            return false;
        }

        // The staging directory must be a sibling of the final destination so
        // that the final directory rename stays on the same volume.
        std::filesystem::create_directories(destinationParent, filesystemError);
        if (filesystemError)
        {
            errorMessage =
                "Не удалось создать родительский каталог назначения " +
                PathToUtf8(destinationParent) + ": " + filesystemError.message();
            return false;
        }

        bool destinationInitiallyExists = false;
        if (!ValidateDestinationDirectory(
            absoluteDestination, destinationInitiallyExists, errorMessage))
        {
            return false;
        }

        std::filesystem::path temporaryDirectory;
        if (!CreateTemporaryDirectory(
            destinationParent, temporaryDirectory, errorMessage))
        {
            return false;
        }

        TemporaryDirectoryGuard temporaryDirectoryGuard(temporaryDirectory);

        // All top-level and nested extraction happens below this private
        // staging directory. The user-visible destination is not populated
        // until the whole archive tree has been processed successfully.
        if (!extractInternal(
            archivePath, temporaryDirectory, errorMessage, 0))
        {
            temporaryDirectoryGuard.cleanup(errorMessage);
            if (errorMessage.empty())
            {
                errorMessage =
                    "Распаковка завершилась ошибкой без дополнительной информации.";
            }
            return false;
        }

        // Recheck the destination immediately before committing. If it did not
        // exist at the beginning, a path created by another process is treated
        // as a collision and is never removed. An initially existing empty
        // directory may be replaced, but only while it is still empty.
        bool destinationCurrentlyExists = false;
        if (!ValidateDestinationDirectory(
            absoluteDestination, destinationCurrentlyExists, errorMessage))
        {
            temporaryDirectoryGuard.cleanup(errorMessage);
            return false;
        }

        if (destinationCurrentlyExists && !destinationInitiallyExists)
        {
            AppendError(
                errorMessage,
                "Каталог назначения появился во время распаковки; результаты "
                "не перенесены: " + PathToUtf8(absoluteDestination));
            temporaryDirectoryGuard.cleanup(errorMessage);
            return false;
        }

        if (destinationCurrentlyExists)
        {
            filesystemError.clear();
            const bool removed =
                std::filesystem::remove(absoluteDestination, filesystemError);

            if (filesystemError || !removed)
            {
                AppendError(
                    errorMessage,
                    "Не удалось заменить пустой каталог назначения " +
                    PathToUtf8(absoluteDestination) +
                    (filesystemError
                        ? ": " + filesystemError.message()
                        : ": каталог не был удалён."));
                temporaryDirectoryGuard.cleanup(errorMessage);
                return false;
            }
        }

        filesystemError.clear();
        std::filesystem::rename(
            temporaryDirectory, absoluteDestination, filesystemError);

        if (filesystemError)
        {
            AppendError(
                errorMessage,
                "Не удалось перенести результаты распаковки в " +
                PathToUtf8(absoluteDestination) + ": " +
                filesystemError.message());
            temporaryDirectoryGuard.cleanup(errorMessage);
            return false;
        }

        temporaryDirectoryGuard.release();
        return true;
    }
    catch (const std::filesystem::filesystem_error& exception)
    {
        errorMessage = "Ошибка файловой системы: ";
        errorMessage += exception.what();

        if (exception.code())
            errorMessage += " (" + exception.code().message() + ")";

        return false;
    }
    catch (const std::exception& exception)
    {
        errorMessage = "Непредвиденная ошибка при распаковке: ";
        errorMessage += exception.what();
        return false;
    }
    catch (...)
    {
        errorMessage = "Неизвестная ошибка при распаковке.";
        return false;
    }
}

bool ArchiveExtractor::extractInternal(
    const std::filesystem::path& archivePath,
    const std::filesystem::path& destPath,
    std::string& errorMessage,
    int depth)
{
    if (depth > MAX_ARCHIVE_DEPTH)
    {
        AppendError(
            errorMessage,
            "Превышена максимальная глубина вложенности архивов.");
        return false;
    }

    ArchivePtr archiveHandle(archive_read_new());
    if (!archiveHandle)
    {
        AppendError(errorMessage, "Не удалось создать объект чтения LibArchive.");
        return false;
    }

    if (archive_read_support_format_all(archiveHandle.get()) != ARCHIVE_OK)
    {
        AppendError(
            errorMessage,
            "Не удалось включить поддержку форматов архивов: " +
            GetArchiveError(archiveHandle.get()));
        return false;
    }

    if (archive_read_support_filter_all(archiveHandle.get()) != ARCHIVE_OK)
    {
        AppendError(
            errorMessage,
            "Не удалось включить поддержку фильтров архивов: " +
            GetArchiveError(archiveHandle.get()));
        return false;
    }

    // Оставляем текущую настройку CP866 для ZIP, поскольку она использовалась
    // в предыдущей версии проекта. ARCHIVE_WARN здесь не считаем фатальной:
    // опция относится только к ZIP и может быть неприменима к другим форматам.
    const int optionsResult = archive_read_set_options(
        archiveHandle.get(), "zip:hdrcharset=CP866");

    if (optionsResult == ARCHIVE_FAILED || optionsResult == ARCHIVE_FATAL)
    {
        AppendError(
            errorMessage,
            "Не удалось настроить обработку заголовков ZIP: " +
            GetArchiveError(archiveHandle.get()));
        return false;
    }

    const int openResult = archive_read_open_filename_w(
        archiveHandle.get(), archivePath.wstring().c_str(), 10240);

    if (openResult != ARCHIVE_OK)
    {
        AppendError(
            errorMessage,
            "Не удалось открыть архив " + PathToUtf8(archivePath) + ": " +
            GetArchiveError(archiveHandle.get()));
        return false;
    }

    std::error_code filesystemError;
    const std::filesystem::path absoluteDestination =
        std::filesystem::absolute(destPath, filesystemError).lexically_normal();

    if (filesystemError)
    {
        AppendError(
            errorMessage,
            "Не удалось определить абсолютный путь каталога назначения " +
            PathToUtf8(destPath) + ": " + filesystemError.message());
        archive_read_close(archiveHandle.get());
        return false;
    }

    bool success = true;
    struct archive_entry* entry = nullptr;

    while (true)
    {
        const int headerResult = archive_read_next_header(
            archiveHandle.get(), &entry);

        if (headerResult == ARCHIVE_EOF)
            break;

        if (headerResult == ARCHIVE_FATAL)
        {
            AppendError(
                errorMessage,
                "Фатальная ошибка чтения заголовка архива: " +
                GetArchiveError(archiveHandle.get()));
            success = false;
            break;
        }

        if (headerResult == ARCHIVE_WARN || headerResult == ARCHIVE_FAILED)
        {
            AppendError(
                errorMessage,
                "Не удалось корректно прочитать запись архива: " +
                GetArchiveError(archiveHandle.get()));
            success = false;

            if (!SkipCurrentEntry(
                archiveHandle.get(),
                errorMessage,
                "Не удалось пропустить повреждённую запись архива"))
            {
                break;
            }

            continue;
        }

        if (headerResult != ARCHIVE_OK)
        {
            AppendError(
                errorMessage,
                "Неожиданный результат чтения заголовка архива: " +
                GetArchiveError(archiveHandle.get()));
            success = false;
            break;
        }

        const std::wstring widePath = GetArchiveEntryPathWide(entry);

        if (widePath.empty())
        {
            AppendError(
                errorMessage,
                "В архиве обнаружена запись с пустым или некорректным именем.");
            success = false;

            if (!SkipCurrentEntry(
                archiveHandle.get(),
                errorMessage,
                "Не удалось пропустить запись с некорректным именем"))
            {
                break;
            }

            continue;
        }

        if (widePath == L".")
        {
            if (!SkipCurrentEntry(
                archiveHandle.get(),
                errorMessage,
                "Не удалось пропустить корневую запись архива"))
            {
                success = false;
                break;
            }

            continue;
        }

        const std::filesystem::path entryPath(widePath);

        std::string unsafePathReason;
        if (!IsSafeWindowsRelativePath(entryPath, unsafePathReason))
        {
            AppendError(
                errorMessage,
                "Ошибка безопасности: отклонён путь из архива '" +
                PathToUtf8(entryPath) + "': " + unsafePathReason + ".");
            success = false;

            if (!SkipCurrentEntry(
                archiveHandle.get(),
                errorMessage,
                "Не удалось пропустить запись с небезопасным путём"))
            {
                break;
            }

            continue;
        }

        const std::filesystem::path targetPath =
            (absoluteDestination / entryPath).lexically_normal();

        if (!IsSubPath(absoluteDestination, targetPath))
        {
            AppendError(errorMessage, "Ошибка безопасности: Zip Slip.");
            success = false;

            if (!SkipCurrentEntry(
                archiveHandle.get(),
                errorMessage,
                "Не удалось пропустить запись с опасным путём"))
            {
                break;
            }

            continue;
        }

        if (HasReparsePointInPath(absoluteDestination, targetPath))
        {
            AppendError(
                errorMessage,
                "Ошибка безопасности: путь содержит символическую ссылку или Reparse Point.");
            success = false;

            if (!SkipCurrentEntry(
                archiveHandle.get(),
                errorMessage,
                "Не удалось пропустить запись, содержащую Reparse Point"))
            {
                break;
            }

            continue;
        }

        const auto fileType = archive_entry_filetype(entry);

        if (fileType == AE_IFDIR)
        {
            filesystemError.clear();
            std::filesystem::create_directories(targetPath, filesystemError);

            if (filesystemError)
            {
                AppendError(
                    errorMessage,
                    "Ошибка создания папки " + PathToUtf8(targetPath) + ": " +
                    filesystemError.message());
                success = false;
            }
        }
        else if (fileType == AE_IFREG)
        {
            if (!targetPath.parent_path().empty())
            {
                filesystemError.clear();
                std::filesystem::create_directories(
                    targetPath.parent_path(), filesystemError);

                if (filesystemError)
                {
                    AppendError(
                        errorMessage,
                        "Ошибка создания родительской папки для " +
                        PathToUtf8(targetPath) + ": " +
                        filesystemError.message());
                    success = false;

                    if (!SkipCurrentEntry(
                        archiveHandle.get(),
                        errorMessage,
                        "Не удалось пропустить файл после ошибки создания папки"))
                    {
                        break;
                    }

                    continue;
                }
            }

            filesystemError.clear();
            const bool targetAlreadyExists =
                std::filesystem::exists(targetPath, filesystemError);

            if (filesystemError)
            {
                AppendError(
                    errorMessage,
                    "Не удалось проверить существование файла " +
                    PathToUtf8(targetPath) + ": " +
                    filesystemError.message());
                success = false;

                if (!SkipCurrentEntry(
                    archiveHandle.get(),
                    errorMessage,
                    "Не удалось пропустить файл после ошибки проверки существования"))
                {
                    break;
                }

                continue;
            }

            if (targetAlreadyExists)
            {
                AppendError(
                    errorMessage,
                    "Файл уже существует, он пропущен: " +
                    PathToUtf8(targetPath));
                success = false;

                if (!SkipCurrentEntry(
                    archiveHandle.get(),
                    errorMessage,
                    "Не удалось пропустить уже существующий файл"))
                {
                    break;
                }

                continue;
            }

            std::ofstream outputFile(
                targetPath,
                std::ios::binary | std::ios::out);

            if (!outputFile.is_open())
            {
                AppendError(
                    errorMessage,
                    "Не удалось создать файл: " + PathToUtf8(targetPath));
                success = false;

                if (!SkipCurrentEntry(
                    archiveHandle.get(),
                    errorMessage,
                    "Не удалось пропустить файл после ошибки создания"))
                {
                    break;
                }

                continue;
            }

            char buffer[COPY_BUFFER_SIZE];
            la_ssize_t bytesRead = 0;
            bool writeError = false;

            while ((bytesRead = archive_read_data(
                archiveHandle.get(), buffer, sizeof(buffer))) > 0)
            {
                outputFile.write(buffer, static_cast<std::streamsize>(bytesRead));

                if (!outputFile.good())
                {
                    AppendError(
                        errorMessage,
                        "Ошибка записи файла (возможно, закончилось место на диске): " +
                        PathToUtf8(targetPath));
                    writeError = true;
                    break;
                }
            }

            if (bytesRead < 0)
            {
                AppendError(
                    errorMessage,
                    "Ошибка чтения данных из архива для файла " +
                    PathToUtf8(targetPath) + ": " +
                    GetArchiveError(archiveHandle.get()));
                writeError = true;
            }

            outputFile.close();
            if (outputFile.fail())
            {
                AppendError(
                    errorMessage,
                    "Ошибка завершения записи файла: " +
                    PathToUtf8(targetPath));
                writeError = true;
            }

            if (writeError)
            {
                success = false;

                filesystemError.clear();
                std::filesystem::remove(targetPath, filesystemError);
                if (filesystemError)
                {
                    AppendError(
                        errorMessage,
                        "Не удалось удалить частично записанный файл " +
                        PathToUtf8(targetPath) + ": " +
                        filesystemError.message());
                }

                if (!SkipCurrentEntry(
                    archiveHandle.get(),
                    errorMessage,
                    "Не удалось пропустить остаток повреждённой записи"))
                {
                    break;
                }

                continue;
            }

            if (IsArchiveExtension(targetPath))
            {
                const std::filesystem::path nestedDestination =
                    targetPath.parent_path() / targetPath.stem();

                bool nestedDestinationExists = false;
                filesystemError.clear();
                if (!CheckPathExists(
                    nestedDestination, nestedDestinationExists, filesystemError))
                {
                    AppendError(
                        errorMessage,
                        "Не удалось проверить каталог для вложенного архива " +
                        PathToUtf8(nestedDestination) + ": " +
                        filesystemError.message());
                    success = false;
                    continue;
                }

                if (nestedDestinationExists)
                {
                    AppendError(
                        errorMessage,
                        "Нельзя распаковать вложенный архив " +
                        PathToUtf8(targetPath.filename()) +
                        ": каталог назначения уже существует: " +
                        PathToUtf8(nestedDestination) +
                        ". Существующее содержимое не изменено.");
                    success = false;
                    continue;
                }

                // First extract into a newly created sibling directory. The
                // final destination appears only after the complete nested
                // archive has been read and validated. This prevents merging
                // with pre-existing data and avoids leaving partial results.
                std::filesystem::path temporaryNestedDirectory;
                if (!CreateTemporaryDirectory(
                    targetPath.parent_path(),
                    temporaryNestedDirectory,
                    errorMessage))
                {
                    success = false;
                    continue;
                }

                TemporaryDirectoryGuard temporaryDirectoryGuard(
                    temporaryNestedDirectory);

                std::string nestedError;
                const bool nestedExtractionSucceeded = extractInternal(
                    targetPath,
                    temporaryNestedDirectory,
                    nestedError,
                    depth + 1);

                if (!nestedExtractionSucceeded)
                {
                    success = false;
                    AppendError(
                        errorMessage,
                        "Ошибка во вложенном архиве " +
                        PathToUtf8(targetPath.filename()) + ": " +
                        nestedError);
                    temporaryDirectoryGuard.cleanup(errorMessage);
                    continue;
                }

                if (!FlattenSingleRootDirectory(
                    temporaryNestedDirectory,
                    targetPath.stem().wstring(),
                    nestedError))
                {
                    success = false;
                    AppendError(
                        errorMessage,
                        "Не удалось подготовить содержимое вложенного архива " +
                        PathToUtf8(targetPath.filename()) + ": " +
                        nestedError);
                    temporaryDirectoryGuard.cleanup(errorMessage);
                    continue;
                }

                // Recheck immediately before committing. rename() itself also
                // reports a collision if another process created the path.
                filesystemError.clear();
                if (!CheckPathExists(
                    nestedDestination, nestedDestinationExists, filesystemError) ||
                    nestedDestinationExists)
                {
                    AppendError(
                        errorMessage,
                        "Не удалось завершить распаковку вложенного архива: " +
                        PathToUtf8(nestedDestination) +
                        (filesystemError
                            ? ": " + filesystemError.message()
                            : " уже существует."));
                    success = false;
                    temporaryDirectoryGuard.cleanup(errorMessage);
                    continue;
                }

                filesystemError.clear();
                std::filesystem::rename(
                    temporaryNestedDirectory,
                    nestedDestination,
                    filesystemError);

                if (filesystemError)
                {
                    AppendError(
                        errorMessage,
                        "Не удалось перенести временные результаты вложенного архива в " +
                        PathToUtf8(nestedDestination) + ": " +
                        filesystemError.message());
                    success = false;
                    temporaryDirectoryGuard.cleanup(errorMessage);
                    continue;
                }

                temporaryDirectoryGuard.release();

                // Delete the nested archive only after the extraction has been
                // committed. If deletion fails, the extracted data remains
                // intact and the error is reported to the caller.
                filesystemError.clear();
                const bool archiveRemoved =
                    std::filesystem::remove(targetPath, filesystemError);

                if (filesystemError || !archiveRemoved)
                {
                    AppendError(
                        errorMessage,
                        "Вложенный архив распакован, но исходный файл не удалось удалить: " +
                        PathToUtf8(targetPath) +
                        (filesystemError
                            ? ": " + filesystemError.message()
                            : ": файл не был удалён."));
                    success = false;
                }
            }
        }
        else
        {
            AppendError(
                errorMessage,
                "Отклонён элемент архива неподдерживаемого типа: " +
                PathToUtf8(targetPath));
            success = false;

            if (!SkipCurrentEntry(
                archiveHandle.get(),
                errorMessage,
                "Не удалось пропустить элемент неподдерживаемого типа"))
            {
                break;
            }
        }
    }

    const int closeResult = archive_read_close(archiveHandle.get());
    if (closeResult != ARCHIVE_OK)
    {
        AppendError(
            errorMessage,
            "Ошибка при закрытии архива: " +
            GetArchiveError(archiveHandle.get()));
        success = false;
    }

    return success;
}
