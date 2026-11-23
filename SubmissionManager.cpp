#include "SubmissionManager.h"

#include "Archive/ArchiveExtractor.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    const std::wstring OOP_DIRECTORY_NAME = L"Участники";

    QString PathToQString(const std::filesystem::path& path)
    {
        return QString::fromStdWString(path.native());
    }

    QString Utf8ToQString(const std::string& value)
    {
        return QString::fromUtf8(value.c_str());
    }

    void AppendError(QString& destination, const QString& message)
    {
        if (message.isEmpty())
            return;

        if (!destination.isEmpty())
            destination += QLatin1Char('\n');

        destination += message;
    }

    bool IsNoSuchFileError(const std::error_code& error)
    {
        return error == std::errc::no_such_file_or_directory;
    }

    bool PathExists(
        const std::filesystem::path& path,
        bool& exists,
        std::error_code& error)
    {
        error.clear();
        const auto status = std::filesystem::symlink_status(path, error);

        if (IsNoSuchFileError(error) ||
            (!error && status.type() == std::filesystem::file_type::not_found))
        {
            error.clear();
            exists = false;
            return true;
        }

        if (error)
            return false;

        exists = true;
        return true;
    }

    bool IsReparsePoint(const std::filesystem::path& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }

    bool ValidateOrdinaryDirectory(
        const std::filesystem::path& path,
        QString& errorMessage)
    {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);

        if (error)
        {
            AppendError(
                errorMessage,
                QStringLiteral("Не удалось проверить каталог %1: %2")
                .arg(PathToQString(path), QString::fromLocal8Bit(error.message().c_str())));
            return false;
        }

        if (!std::filesystem::is_directory(status) || IsReparsePoint(path))
        {
            AppendError(
                errorMessage,
                QStringLiteral("Путь не является обычным каталогом или является Reparse Point: %1")
                .arg(PathToQString(path)));
            return false;
        }

        return true;
    }

    bool EnsureDirectoryExists(
        const std::filesystem::path& path,
        QString& errorMessage)
    {
        // Приводим путь к нормализованному абсолютному виду.
        const std::filesystem::path normalizedPath = path.lexically_normal();

        // Получаем корневую часть пути (например, D:\).
        const std::filesystem::path rootPath = normalizedPath.root_path();

        if (rootPath.empty() || !normalizedPath.is_absolute())
        {
            AppendError(
                errorMessage,
                QStringLiteral("Указан некорректный или не абсолютный путь: %1")
                .arg(PathToQString(path)));

            return false;
        }

        // Проверяем корень диска.
        // Например, D:\ должен существовать и не быть Reparse Point.
        if (!ValidateOrdinaryDirectory(rootPath, errorMessage))
            return false;

        std::filesystem::path currentPath = rootPath;

        // Последовательно проверяем каждый компонент пути.
        for (const auto& component : normalizedPath.relative_path())
        {
            if (component.empty() || component == L".")
                continue;

            // Дополнительная защита от выхода за пределы корневого каталога.
            if (component == L"..")
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Обнаружен небезопасный компонент '..' в пути: %1")
                    .arg(PathToQString(path)));

                return false;
            }

            currentPath /= component;

            // Проверяем, существует ли текущий компонент.
            bool exists = false;
            std::error_code error;

            if (!PathExists(currentPath, exists, error))
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Не удалось проверить каталог %1: %2")
                    .arg(
                        PathToQString(currentPath),
                        QString::fromLocal8Bit(error.message().c_str())));

                return false;
            }

            // Если каталога нет, создаём только один уровень.
            if (!exists)
            {
                error.clear();

                std::filesystem::create_directory(currentPath, error);

                if (error)
                {
                    AppendError(
                        errorMessage,
                        QStringLiteral("Не удалось создать каталог %1: %2")
                        .arg(
                            PathToQString(currentPath),
                            QString::fromLocal8Bit(error.message().c_str())));

                    return false;
                }
            }

            // Проверяем, что текущий компонент является обычным каталогом
            // и не является символической ссылкой или Reparse Point.
            if (!ValidateOrdinaryDirectory(currentPath, errorMessage))
                return false;
        }

        return true;
    }

    bool IsLetter(wchar_t character)
    {
        // Полные ФИО обычно содержат латинские или кириллические буквы.
        // Дополнительные диапазоны покрывают распространённые буквы с диакритикой.
        return
            (character >= L'A' && character <= L'Z') ||
            (character >= L'a' && character <= L'z') ||
            (character >= 0x00C0 && character <= 0x02AF) ||
            (character >= 0x0370 && character <= 0x052F) ||
            (character >= 0x1E00 && character <= 0x1EFF) ||
            (character >= 0x2C60 && character <= 0x2C7F);
    }

    bool IsNameSeparator(wchar_t character)
    {
        return character == L'-' ||
            character == 0x2010 || // hyphen
            character == 0x2011 || // non-breaking hyphen
            character == L'\'' ||
            character == 0x2019;   // right single quotation mark
    }

    bool IsValidNamePart(const std::wstring& part)
    {
        if (part.empty() || !IsLetter(part.front()) || !IsLetter(part.back()))
            return false;

        bool previousWasSeparator = false;
        for (wchar_t character : part)
        {
            if (IsLetter(character))
            {
                previousWasSeparator = false;
                continue;
            }

            if (IsNameSeparator(character) && !previousWasSeparator)
            {
                previousWasSeparator = true;
                continue;
            }

            return false;
        }

        return !previousWasSeparator;
    }

    bool IsValidFullName(const std::wstring& fullName)
    {
        // ФИО не должно быть пустым.
        if (fullName.empty())
            return false;

        // Запрещаем пробелы в начале и конце.
        if (fullName.front() == L' ' ||
            fullName.back() == L' ')
        {
            return false;
        }

        std::vector<std::wstring> parts;
        std::wstring current;

        for (wchar_t character : fullName)
        {
            // Разрешаем только один обычный пробел
            // между частями ФИО.
            if (character == L' ')
            {
                // Два пробела подряд недопустимы.
                if (current.empty())
                    return false;

                parts.push_back(current);
                current.clear();

                // В ФИО должно быть ровно три части.
                if (parts.size() >= 3)
                    return false;
            }
            else
            {
                // Табуляция и другие управляющие символы запрещены.
                if (character == L'\t' ||
                    character == L'\n' ||
                    character == L'\r')
                {
                    return false;
                }

                current.push_back(character);
            }
        }

        // Последняя часть не должна быть пустой.
        if (current.empty())
            return false;

        parts.push_back(current);

        // Проверяем количество частей.
        if (parts.size() != 3)
            return false;

        // Проверяем допустимость символов в каждой части.
        return IsValidNamePart(parts[0]) &&
            IsValidNamePart(parts[1]) &&
            IsValidNamePart(parts[2]);
    }

    bool IsDirectoryWithoutReparsePoint(
        const std::filesystem::path& path,
        bool& isDirectory,
        QString& errorMessage)
    {
        isDirectory = false;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);

        if (error)
        {
            AppendError(
                errorMessage,
                QStringLiteral("Не удалось прочитать сведения о %1: %2")
                .arg(PathToQString(path), QString::fromLocal8Bit(error.message().c_str())));
            return false;
        }

        if (std::filesystem::is_symlink(status) || IsReparsePoint(path))
        {
            AppendError(
                errorMessage,
                QStringLiteral("Обнаружена символическая ссылка или Reparse Point: %1")
                .arg(PathToQString(path)));
            return false;
        }

        isDirectory = std::filesystem::is_directory(status);
        return true;
    }

    bool GetDirectoryChildren(
        const std::filesystem::path& directory,
        std::vector<std::filesystem::path>& children,
        QString& errorMessage)
    {
        children.clear();
        std::error_code error;
        std::filesystem::directory_iterator iterator(directory, error);
        const std::filesystem::directory_iterator end;

        if (error)
        {
            AppendError(
                errorMessage,
                QStringLiteral("Не удалось прочитать каталог %1: %2")
                .arg(PathToQString(directory), QString::fromLocal8Bit(error.message().c_str())));
            return false;
        }

        while (iterator != end)
        {
            children.push_back(iterator->path());
            iterator.increment(error);

            if (error)
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Ошибка при просмотре каталога %1: %2")
                    .arg(PathToQString(directory), QString::fromLocal8Bit(error.message().c_str())));
                return false;
            }
        }

        return true;
    }

    bool AreStudentFolders(
        const std::vector<std::filesystem::path>& children,
        QString& errorMessage)
    {
        if (children.empty())
            return false;

        for (const auto& child : children)
        {
            bool isDirectory = false;
            if (!IsDirectoryWithoutReparsePoint(child, isDirectory, errorMessage))
                return false;

            if (!isDirectory || !IsValidFullName(child.filename().native()))
                return false;
        }

        return true;
    }

    bool FindStudentFolders(
        const std::filesystem::path& extractionRoot,
        std::vector<std::filesystem::path>& studentFolders,
        QString& errorMessage)
    {
        std::vector<std::filesystem::path> topLevel;
        if (!GetDirectoryChildren(extractionRoot, topLevel, errorMessage))
            return false;

        if (AreStudentFolders(topLevel, errorMessage))
        {
            studentFolders = std::move(topLevel);
        }
        else
        {
            // Допускается ровно одна общая папка-обёртка.
            if (topLevel.size() != 1)
            {
                AppendError(
                    errorMessage,
                    QStringLiteral(
                        "Не удалось однозначно определить папки студентов. "
                        "Ожидались папки с ФИО или одна общая папка-обёртка."));
                return false;
            }

            bool wrapperIsDirectory = false;
            if (!IsDirectoryWithoutReparsePoint(
                topLevel.front(), wrapperIsDirectory, errorMessage))
            {
                return false;
            }

            if (!wrapperIsDirectory)
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("В корне архива найден файл вместо папки студента: %1")
                    .arg(PathToQString(topLevel.front())));
                return false;
            }

            std::vector<std::filesystem::path> wrappedChildren;
            if (!GetDirectoryChildren(
                topLevel.front(), wrappedChildren, errorMessage))
            {
                return false;
            }

            if (!AreStudentFolders(wrappedChildren, errorMessage))
            {
                AppendError(
                    errorMessage,
                    QStringLiteral(
                        "Содержимое общей папки-обёртки неоднозначно: "
                        "внутри должны находиться только папки с полными ФИО."));
                return false;
            }

            studentFolders = std::move(wrappedChildren);
        }

        std::sort(
            studentFolders.begin(),
            studentFolders.end(),
            [](const std::filesystem::path& left,
                const std::filesystem::path& right)
            {
                return left.filename().native() < right.filename().native();
            });

        return true;
    }

    int GetWorkNumber(WorkType workType)
    {
        switch (workType)
        {
        case WorkType::Lab1:
            return 1;
        case WorkType::Lab2:
            return 2;
        case WorkType::CourseProject:
            return 3;
        default:
            return 0;
        }
    }

    bool ParsePositiveInteger(
        const std::wstring& digits,
        int& value)
    {
        if (digits.empty())
            return false;

        unsigned long long parsed = 0;
        for (wchar_t character : digits)
        {
            if (character < L'0' || character > L'9')
                return false;

            const unsigned long long digit =
                static_cast<unsigned long long>(character - L'0');
            if (parsed >
                (static_cast<unsigned long long>(INT_MAX) - digit) / 10ULL)
            {
                return false;
            }
            parsed = parsed * 10ULL + digit;
        }

        if (parsed == 0)
            return false;

        value = static_cast<int>(parsed);
        return true;
    }

    bool FindLatestAttempt(
        const std::filesystem::path& studentDirectory,
        int workNumber,
        int& latestAttempt,
        QString& errorMessage)
    {
        latestAttempt = 0;
        std::vector<std::filesystem::path> children;
        if (!GetDirectoryChildren(studentDirectory, children, errorMessage))
            return false;

        const std::wstring prefix(1, static_cast<wchar_t>(L'0' + workNumber));

        for (const auto& child : children)
        {
            const std::wstring name = child.filename().native();
            if (name.size() < 2 || name.compare(0, 1, prefix) != 0)
                continue;

            int attempt = 0;
            if (!ParsePositiveInteger(name.substr(1), attempt))
                continue;

            bool isDirectory = false;
            if (!IsDirectoryWithoutReparsePoint(child, isDirectory, errorMessage))
                return false;

            if (!isDirectory)
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Путь с номером попытки существует, но не является папкой: %1")
                    .arg(PathToQString(child)));
                return false;
            }

            latestAttempt = (std::max)(latestAttempt, attempt);
        }

        return true;
    }

    bool CreateTemporaryDirectory(
        const std::filesystem::path& parentDirectory,
        const std::wstring& prefix,
        std::filesystem::path& createdDirectory,
        QString& errorMessage)
    {
        static std::atomic<unsigned long long> counter{ 0 };

        for (int attempt = 0; attempt < 256; ++attempt)
        {
            const auto value = counter.fetch_add(1, std::memory_order_relaxed);
            const std::wstring name = prefix +
                std::to_wstring(GetCurrentProcessId()) + L"_" +
                std::to_wstring(value);
            auto candidate = parentDirectory / name;

            std::error_code error;
            const bool created = std::filesystem::create_directory(candidate, error);
            if (created)
            {
                createdDirectory = std::move(candidate);
                return true;
            }

            if (error && error != std::errc::file_exists)
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Не удалось создать временный каталог %1: %2")
                    .arg(PathToQString(candidate), QString::fromLocal8Bit(error.message().c_str())));
                return false;
            }
        }

        AppendError(
            errorMessage,
            QStringLiteral("Не удалось подобрать свободное имя временного каталога в %1.")
            .arg(PathToQString(parentDirectory)));
        return false;
    }

    class DirectoryGuard
    {
    public:
        DirectoryGuard() noexcept = default;

        explicit DirectoryGuard(std::filesystem::path path)
            : path_(std::move(path)),
            active_(!path_.empty())
        {
        }

        DirectoryGuard(const DirectoryGuard&) = delete;
        DirectoryGuard& operator=(const DirectoryGuard&) = delete;

        // Передача владения временным каталогом.
        DirectoryGuard(DirectoryGuard&& other) noexcept
            : path_(std::move(other.path_)),
            active_(std::exchange(other.active_, false))
        {
        }

        DirectoryGuard& operator=(DirectoryGuard&& other) noexcept
        {
            if (this != &other)
            {
                cleanupNoThrow();

                path_ = std::move(other.path_);
                active_ = std::exchange(other.active_, false);
            }

            return *this;
        }

        // Автоматическая очистка при уничтожении объекта.
        ~DirectoryGuard() noexcept
        {
            cleanupNoThrow();
        }

        // Явное удаление временного каталога.
        bool cleanup(QString& errorMessage)
        {
            if (!active_)
                return true;

            std::error_code error;

            std::filesystem::remove_all(path_, error);

            if (error)
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Не удалось удалить временный каталог %1: %2")
                    .arg(
                        PathToQString(path_),
                        QString::fromLocal8Bit(error.message().c_str())));

                return false;
            }

            active_ = false;
            return true;
        }

        // Отказ от автоматической очистки.
        // Используется после успешного переноса каталога.
        void release() noexcept
        {
            active_ = false;
        }

        // Получение пути без передачи владения каталогом.
        const std::filesystem::path& getPath() const noexcept
        {
            return path_;
        }

    private:
        void cleanupNoThrow() noexcept
        {
            if (!active_)
                return;

            std::error_code ignored;

            std::filesystem::remove_all(path_, ignored);

            active_ = false;
        }

        std::filesystem::path path_;
        bool active_ = false;
    };

    bool CopyStudentDirectoryContents(
        const std::filesystem::path& sourceDirectory,
        const std::filesystem::path& destinationDirectory,
        QString& errorMessage)
    {
        std::error_code error;
        std::filesystem::recursive_directory_iterator iterator(
            sourceDirectory, std::filesystem::directory_options::none, error);
        const std::filesystem::recursive_directory_iterator end;

        if (error)
        {
            AppendError(
                errorMessage,
                QStringLiteral("Не удалось прочитать файлы студента %1: %2")
                .arg(PathToQString(sourceDirectory), QString::fromLocal8Bit(error.message().c_str())));
            return false;
        }

        while (iterator != end)
        {
            const auto sourcePath = iterator->path();
            const auto relativePath = sourcePath.lexically_relative(sourceDirectory);

            if (relativePath.empty() || relativePath.is_absolute())
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Обнаружен некорректный относительный путь при копировании: %1")
                    .arg(PathToQString(sourcePath)));
                return false;
            }

            for (const auto& component : relativePath)
            {
                if (component == L"..")
                {
                    AppendError(
                        errorMessage,
                        QStringLiteral("Обнаружен выход за пределы папки студента: %1")
                        .arg(PathToQString(sourcePath)));
                    return false;
                }
            }

            const auto status = iterator->symlink_status(error);
            if (error)
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Не удалось проверить файл %1: %2")
                    .arg(PathToQString(sourcePath), QString::fromLocal8Bit(error.message().c_str())));
                return false;
            }

            if (std::filesystem::is_symlink(status) || IsReparsePoint(sourcePath))
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Копирование остановлено: обнаружена символическая ссылка или Reparse Point: %1")
                    .arg(PathToQString(sourcePath)));
                return false;
            }

            const auto destinationPath = destinationDirectory / relativePath;

            if (std::filesystem::is_directory(status))
            {
                error.clear();
                std::filesystem::create_directories(destinationPath, error);
                if (error)
                {
                    AppendError(
                        errorMessage,
                        QStringLiteral("Не удалось создать папку %1: %2")
                        .arg(PathToQString(destinationPath), QString::fromLocal8Bit(error.message().c_str())));
                    return false;
                }
            }
            else if (std::filesystem::is_regular_file(status))
            {
                error.clear();
                std::filesystem::create_directories(destinationPath.parent_path(), error);
                if (error)
                {
                    AppendError(
                        errorMessage,
                        QStringLiteral("Не удалось создать родительскую папку для %1: %2")
                        .arg(PathToQString(destinationPath), QString::fromLocal8Bit(error.message().c_str())));
                    return false;
                }

                error.clear();
                std::filesystem::copy_file(
                    sourcePath,
                    destinationPath,
                    std::filesystem::copy_options::none,
                    error);

                if (error)
                {
                    AppendError(
                        errorMessage,
                        QStringLiteral("Не удалось скопировать %1 в %2: %3")
                        .arg(PathToQString(sourcePath),
                            PathToQString(destinationPath),
                            QString::fromLocal8Bit(error.message().c_str())));
                    return false;
                }
            }
            else
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Неподдерживаемый тип элемента в папке студента: %1")
                    .arg(PathToQString(sourcePath)));
                return false;
            }

            iterator.increment(error);
            if (error)
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Ошибка обхода папки студента %1: %2")
                    .arg(PathToQString(sourceDirectory), QString::fromLocal8Bit(error.message().c_str())));
                return false;
            }
        }

        return true;
    }

    struct PendingSubmission
    {
        std::filesystem::path studentDirectory;
        std::filesystem::path stagingDirectory;
        std::filesystem::path finalAttemptDirectory;

        // RAII-защита временного каталога.
        DirectoryGuard stagingGuard;

        std::wstring fullName;

        int workNumber = 0;
        int attemptNumber = 0;

        bool studentDirectoryWasCreated = false;
        bool committed = false;
    };

    void CleanupPendingSubmissions(
        std::vector<PendingSubmission>& pending,
        QString& errorMessage)
    {
        for (auto& item : pending)
        {
            // Уже зафиксированные попытки не трогаем.
            if (item.committed)
                continue;

            // Удаляем временный каталог через RAII-объект.
            // При ошибке деструктор дополнительно попробует
            // выполнить очистку при уничтожении объекта.
            item.stagingGuard.cleanup(errorMessage);

            // Удаляем только пустой каталог студента,
            // если он был создан во время текущего импорта.
            if (item.studentDirectoryWasCreated)
            {
                std::error_code error;

                std::filesystem::remove(
                    item.studentDirectory,
                    error);

                if (error)
                {
                    AppendError(
                        errorMessage,
                        QStringLiteral(
                            "Не удалось удалить пустой каталог студента %1: %2")
                        .arg(
                            PathToQString(item.studentDirectory),
                            QString::fromLocal8Bit(error.message().c_str())));
                }
            }
        }
    }

    bool IsCaseInsensitivePathEqual(
        const std::filesystem::path& left,
        const std::filesystem::path& right)
    {
        const std::wstring leftValue = left.filename().native();
        const std::wstring rightValue = right.filename().native();

        return CompareStringOrdinal(
            leftValue.c_str(),
            static_cast<int>(leftValue.size()),
            rightValue.c_str(),
            static_cast<int>(rightValue.size()),
            TRUE) == CSTR_EQUAL;
    }
}

bool SubmissionManager::importArchive(
    const QString& archivePath,
    const QString& rootDirectory,
    WorkType workType,
    const QString& templatePath,
    QList<SubmissionInfo>& results,
    QString& errorMessage)
{
    results.clear();
    errorMessage.clear();

    // Параметр оставлен для совместимости с существующим вызовом из mainwindow.cpp.
    // Импорт больше не копирует шаблоны и не создаёт черновики рецензий.
    Q_UNUSED(templatePath);

    try
    {
        const int workNumber = GetWorkNumber(workType);
        if (workNumber == 0)
        {
            errorMessage = QStringLiteral("Указан неизвестный тип работы.");
            return false;
        }

        if (archivePath.trimmed().isEmpty() || rootDirectory.trimmed().isEmpty())
        {
            errorMessage = QStringLiteral("Не указан путь к архиву или корневому каталогу.");
            return false;
        }

        std::error_code filesystemError;
        const std::filesystem::path inputArchive =
            std::filesystem::absolute(
                std::filesystem::path(archivePath.toStdWString()), filesystemError)
            .lexically_normal();

        if (filesystemError)
        {
            errorMessage = QStringLiteral("Не удалось определить путь к архиву: %1")
                .arg(QString::fromLocal8Bit(filesystemError.message().c_str()));
            return false;
        }

        const auto archiveStatus = std::filesystem::symlink_status(inputArchive, filesystemError);
        if (filesystemError || !std::filesystem::is_regular_file(archiveStatus) ||
            IsReparsePoint(inputArchive))
        {
            errorMessage = QStringLiteral("Исходный архив недоступен или не является обычным файлом: %1")
                .arg(PathToQString(inputArchive));
            if (filesystemError)
                AppendError(errorMessage, QString::fromLocal8Bit(filesystemError.message().c_str()));
            return false;
        }

        std::filesystem::path rootPath =
            std::filesystem::absolute(
                std::filesystem::path(rootDirectory.toStdWString()), filesystemError)
            .lexically_normal();

        if (filesystemError)
        {
            errorMessage = QStringLiteral("Не удалось определить корневой каталог: %1")
                .arg(QString::fromLocal8Bit(filesystemError.message().c_str()));
            return false;
        }

        const std::filesystem::path oopDirectory = rootPath / OOP_DIRECTORY_NAME;
        if (!EnsureDirectoryExists(oopDirectory, errorMessage))
            return false;

        // Сначала распаковываем весь внешний архив во временное хранилище.
        // ArchiveExtractor также раскрывает поддерживаемые вложенные архивы.
        std::filesystem::path extractionDirectory;
        if (!CreateTemporaryDirectory(
            oopDirectory,
            L".submission_unpack_",
            extractionDirectory,
            errorMessage))
        {
            return false;
        }
        DirectoryGuard extractionGuard(extractionDirectory);

        ArchiveExtractor extractor;
        std::string extractionError;
        if (!extractor.extract(inputArchive, extractionDirectory, extractionError))
        {
            errorMessage = QStringLiteral("Не удалось распаковать входной архив: %1")
                .arg(Utf8ToQString(extractionError));
            return false;
        }

        std::vector<std::filesystem::path> studentFolders;
        if (!FindStudentFolders(extractionDirectory, studentFolders, errorMessage))
            return false;

        // Дополнительно проверяем, что два каталога не указывают на одно имя
        // с учётом регистронезависимой семантики Windows.
        for (std::size_t i = 0; i < studentFolders.size(); ++i)
        {
            for (std::size_t j = i + 1; j < studentFolders.size(); ++j)
            {
                if (IsCaseInsensitivePathEqual(studentFolders[i], studentFolders[j]))
                {
                    errorMessage = QStringLiteral("В архиве обнаружены повторяющиеся ФИО с разным регистром: %1")
                        .arg(PathToQString(studentFolders[i].filename()));
                    return false;
                }
            }
        }

        std::vector<PendingSubmission> pending;
        pending.reserve(studentFolders.size());

        for (const auto& sourceStudentDirectory : studentFolders)
        {
            const std::wstring fullName = sourceStudentDirectory.filename().native();
            const std::filesystem::path studentDirectory = oopDirectory / fullName;

            bool studentDirectoryExists = false;
            filesystemError.clear();
            if (!PathExists(studentDirectory, studentDirectoryExists, filesystemError))
            {
                errorMessage = QStringLiteral("Не удалось проверить папку студента %1: %2")
                    .arg(PathToQString(studentDirectory), QString::fromLocal8Bit(filesystemError.message().c_str()));
                CleanupPendingSubmissions(pending, errorMessage);
                return false;
            }

            int latestAttempt = 0;
            if (studentDirectoryExists)
            {
                if (!ValidateOrdinaryDirectory(studentDirectory, errorMessage) ||
                    !FindLatestAttempt(studentDirectory, workNumber, latestAttempt, errorMessage))
                {
                    CleanupPendingSubmissions(pending, errorMessage);
                    return false;
                }
            }

            if (latestAttempt == INT_MAX)
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Невозможно создать следующую попытку для %1: номер попытки исчерпан.")
                    .arg(QString::fromStdWString(fullName)));
                CleanupPendingSubmissions(pending, errorMessage);
                return false;
            }

            bool studentDirectoryWasCreated = false;
            if (!studentDirectoryExists)
            {
                filesystemError.clear();
                const bool created = std::filesystem::create_directory(
                    studentDirectory, filesystemError);

                if (filesystemError || !created)
                {
                    AppendError(
                        errorMessage,
                        QStringLiteral("Не удалось создать каталог студента %1%2")
                        .arg(PathToQString(studentDirectory),
                            filesystemError
                            ? QStringLiteral(": ") + QString::fromLocal8Bit(filesystemError.message().c_str())
                            : QStringLiteral(".")));
                    CleanupPendingSubmissions(pending, errorMessage);
                    return false;
                }

                studentDirectoryWasCreated = true;
            }

            const int nextAttempt = latestAttempt + 1;
            const std::filesystem::path finalAttemptDirectory =
                studentDirectory /
                (std::to_wstring(workNumber) + std::to_wstring(nextAttempt));

            bool finalAttemptExists = false;
            filesystemError.clear();
            if (!PathExists(finalAttemptDirectory, finalAttemptExists, filesystemError) ||
                finalAttemptExists)
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Каталог следующей попытки уже существует или недоступен: %1")
                    .arg(PathToQString(finalAttemptDirectory)));
                if (studentDirectoryWasCreated)
                {
                    std::error_code ignored;
                    std::filesystem::remove(studentDirectory, ignored);
                }
                CleanupPendingSubmissions(pending, errorMessage);
                return false;
            }

            std::filesystem::path stagingDirectory;

            if (!CreateTemporaryDirectory(
                studentDirectory,
                L".submission_stage_",
                stagingDirectory,
                errorMessage))
            {
                if (studentDirectoryWasCreated)
                {
                    std::error_code ignored;
                    std::filesystem::remove(studentDirectory, ignored);
                }

                CleanupPendingSubmissions(pending, errorMessage);
                return false;
            }

            // Сразу передаём владение временной папкой RAII-объекту.
            // Если впоследствии произойдёт исключение,
            // локальный объект автоматически удалит каталог.
            DirectoryGuard stagingGuard(std::move(stagingDirectory));

            PendingSubmission item;

            item.studentDirectory = studentDirectory;

            // Сохраняем путь для дальнейшей работы.
            item.stagingDirectory = stagingGuard.getPath();

            item.finalAttemptDirectory = finalAttemptDirectory;

            item.fullName = fullName;

            item.workNumber = workNumber;
            item.attemptNumber = nextAttempt;

            item.studentDirectoryWasCreated = studentDirectoryWasCreated;

            // Передаём владение временным каталогом структуре PendingSubmission.
            item.stagingGuard = std::move(stagingGuard);

            pending.push_back(std::move(item));

            PendingSubmission& current = pending.back();

            if (!CopyStudentDirectoryContents(
                sourceStudentDirectory, current.stagingDirectory, errorMessage))
            {
                CleanupPendingSubmissions(pending, errorMessage);
                return false;
            }

        }

        // Все попытки подготовлены во временных каталогах, но ещё не перенесены
        // в окончательные папки. Сначала удаляем временную распаковку исходного
        // архива. Если очистка не удалась, ни одна новая попытка ещё не создана:
        // удаляем подготовленные staging-каталоги и завершаем импорт с ошибкой.
        if (!extractionGuard.cleanup(errorMessage))
        {
            CleanupPendingSubmissions(pending, errorMessage);
            return false;
        }

        // Стадии всех студентов подготовлены. Теперь поочерёдно фиксируем их
        // атомарным переименованием в окончательные каталоги попыток.
        for (auto& item : pending)
        {
            bool destinationExists = false;
            filesystemError.clear();
            if (!PathExists(item.finalAttemptDirectory, destinationExists, filesystemError) ||
                destinationExists)
            {
                AppendError(
                    errorMessage,
                    QStringLiteral("Не удалось зафиксировать попытку: каталог назначения уже существует или недоступен: %1")
                    .arg(PathToQString(item.finalAttemptDirectory)));
                CleanupPendingSubmissions(pending, errorMessage);
                if (!results.isEmpty())
                {
                    AppendError(
                        errorMessage,
                        QStringLiteral("Импорт завершён частично: уже созданные попытки сохранены в results."));
                }
                return false;
            }

            // MoveFileW не заменяет уже существующий каталог назначения.
            // Папка staging находится рядом с final, поэтому перенос остаётся
            // в пределах одного тома и выполняется как атомарное переименование.

            if (!MoveFileW(
                item.stagingDirectory.c_str(),
                item.finalAttemptDirectory.c_str()))
            {
                const std::error_code moveError(
                    static_cast<int>(GetLastError()), std::system_category());
                AppendError(
                    errorMessage,
                    QStringLiteral("Не удалось перенести подготовленную попытку в %1: %2")
                    .arg(PathToQString(item.finalAttemptDirectory),
                        QString::fromLocal8Bit(moveError.message().c_str())));
                CleanupPendingSubmissions(pending, errorMessage);
                if (!results.isEmpty())
                {
                    AppendError(
                        errorMessage,
                        QStringLiteral("Импорт завершён частично: уже созданные попытки сохранены в results."));
                }
                return false;
            }

            item.committed = true;

            // Каталог staging успешно перемещён в окончательную папку.
            // Запрещаем DirectoryGuard удалять его при уничтожении.
            item.stagingGuard.release();

            SubmissionInfo info;
            info.fullName = QString::fromStdWString(item.fullName);
            info.workNumber = item.workNumber;
            info.attemptNumber = item.attemptNumber;
            info.folderPath = PathToQString(item.finalAttemptDirectory);
            info.reviewPath.clear(); // Рецензия создаётся отдельно через ReviewManager.
            results.append(info);
        }

        return true;
    }
    catch (const std::filesystem::filesystem_error& exception)
    {
        errorMessage = QStringLiteral("Ошибка файловой системы: %1 (%2)")
            .arg(QString::fromLocal8Bit(exception.what()),
                QString::fromLocal8Bit(exception.code().message().c_str()));
        return false;
    }
    catch (const std::exception& exception)
    {
        errorMessage = QStringLiteral("Непредвиденная ошибка импорта: %1")
            .arg(QString::fromLocal8Bit(exception.what()));
        return false;
    }
    catch (...)
    {
        errorMessage = QStringLiteral("Неизвестная ошибка при импорте архива.");
        return false;
    }
}
