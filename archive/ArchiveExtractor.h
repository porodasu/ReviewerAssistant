#pragma once

#include <filesystem>
#include <string>

class ArchiveExtractor
{
public:
    // Распаковывает архив в destPath.
    // Сначала извлекает всё содержимое во временный каталог рядом с destPath.
    // Итоговый каталог появляется только после успешной обработки архива.
    // Если destPath уже существует, он должен быть пустым обычным каталогом.
    // Вложенные архивы извлекаются в отдельные каталоги внутри текущей распаковки.
    // Размер и количество файлов намеренно не ограничиваются.
    // При ошибке возвращает false и записывает описание в errorMessage.
    bool extract(
        const std::filesystem::path& archivePath,
        const std::filesystem::path& destPath,
        std::string& errorMessage);

private:
    bool extractInternal(
        const std::filesystem::path& archivePath,
        const std::filesystem::path& destPath,
        std::string& errorMessage,
        int depth);
};
