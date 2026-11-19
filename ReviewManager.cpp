#include "ReviewManager.h"

#include <QDomDocument>
#include <QFile>
#include <QByteArray>
#include <QFileInfo>
#include <QDir>
#include <QStringList>
#include <QList>
#include <QRegularExpression>

#include <archive.h>
#include <archive_entry.h>

// Заменяет заполнитель внутри XML-документа
bool replacePlaceholder(QDomDocument& document,
    const QString& placeholder,
    const QString& newValue)
{
    bool replaced = false;

    QDomNodeList paragraphs = document.elementsByTagName("w:p");

    for (int i = 0; i < paragraphs.count(); ++i) {

        QDomNodeList fragments =
            paragraphs.at(i).toElement().elementsByTagName("w:t");

        // Собираем полный текст абзаца
        QString fullText;

        for (int j = 0; j < fragments.count(); ++j) {
            fullText += fragments.at(j).toElement().text();
        }

        int start = fullText.indexOf(placeholder);

        if (start == -1) {
            continue;
        }

        int end = start + placeholder.length();
        int position = 0;
        bool inserted = false;

        // Изменяем только участки, содержащие заполнитель
        for (int j = 0; j < fragments.count(); ++j) {

            QDomNode textNode = fragments.at(j).firstChild();
            QString fragment = fragments.at(j).toElement().text();

            int fragmentStart = position;
            int fragmentEnd = position + fragment.length();
            position = fragmentEnd;

            int left = qMax(start, fragmentStart);
            int right = qMin(end, fragmentEnd);

            if (left >= right) {
                continue;
            }

            int localStart = left - fragmentStart;
            int localEnd = right - fragmentStart;

            QString updated = fragment.left(localStart);

            if (!inserted) {
                updated += newValue;
                inserted = true;
            }

            updated += fragment.mid(localEnd);

            textNode.setNodeValue(updated);
        }

        replaced = true;
    }

    return replaced;
}


// Возвращает текст одного абзаца, объединяя элементы w:t.
static QString paragraphText(const QDomNode& paragraph)
{
    QString result;
    QDomNodeList fragments = paragraph.toElement().elementsByTagName("w:t");
    for (int i = 0; i < fragments.count(); ++i)
        result += fragments.at(i).toElement().text();
    return result;
}

// Меняет только указанный диапазон текста абзаца.
// Остальные узлы XML (в том числе оформление) остаются на месте.
static bool replaceTextRange(QDomNode paragraph, int start, int length,
    const QString& newText)
{
    if (start < 0 || length <= 0)
        return false;

    QDomNodeList fragments = paragraph.toElement().elementsByTagName("w:t");
    int position = 0;
    bool inserted = false;
    const int end = start + length;

    for (int i = 0; i < fragments.count(); ++i) {
        QDomElement fragmentElement = fragments.at(i).toElement();
        const QString original = fragmentElement.text();
        const int fragmentStart = position;
        const int fragmentEnd = position + original.size();
        position = fragmentEnd;

        const int left = qMax(start, fragmentStart);
        const int right = qMin(end, fragmentEnd);
        if (left >= right)
            continue;

        QString updated = original.left(left - fragmentStart);
        if (!inserted) {
            updated += newText;
            inserted = true;
        }
        updated += original.mid(right - fragmentStart);

        QDomNode textNode = fragmentElement.firstChild();
        if (textNode.isNull())
            fragmentElement.appendChild(fragmentElement.ownerDocument().createTextNode(updated));
        else
            textNode.setNodeValue(updated);

        // Не даём Word/LibreOffice схлопнуть значимые пробелы.
        if (updated.startsWith(' ') || updated.endsWith(' '))
            fragmentElement.setAttribute("xml:space", "preserve");
    }

    return inserted;
}

// Ищем и меняем поле по подписи, только в шапке рецензии.
// stopLabel нужен для случая «специальность ... шифр ...».
static bool replaceFieldByLabel(QDomDocument& document,
    const QString& label, const QString& stopLabel,
    const QString& newValue, QString& errorMessage)
{
    QDomNode found;
    int valueStart = -1;
    int valueLength = 0;
    int matches = 0;

    QDomNodeList paragraphs = document.elementsByTagName("w:p");
    for (int i = 0; i < paragraphs.count(); ++i) {
        QDomNode paragraph = paragraphs.at(i);
        QString text = paragraphText(paragraph);

        // Замечания не должны использоваться для поиска реквизитов.
        if (text.trimmed().compare("Замечания", Qt::CaseInsensitive) == 0)
            break;

        int labelPos = text.indexOf(label, 0, Qt::CaseInsensitive);
        if (labelPos < 0)
            continue;

        int start = labelPos + label.length();
        while (start < text.size() && text.at(start).isSpace())
            ++start;

        int end = text.size();
        if (!stopLabel.isEmpty()) {
            int stop = text.indexOf(stopLabel, start, Qt::CaseInsensitive);
            if (stop >= 0)
                end = stop;
        }
        while (end > start && text.at(end - 1).isSpace())
            --end;

        if (start >= end) {
            errorMessage = "Поле «" + label + "» пустое или имеет неожиданный формат";
            return false;
        }

        ++matches;
        found = paragraph;
        valueStart = start;
        valueLength = end - start;
    }

    if (matches != 1) {
        errorMessage = "Не удалось однозначно найти поле «" + label
            + "» в предыдущей рецензии";
        return false;
    }
    if (!replaceTextRange(found, valueStart, valueLength, newValue)) {
        errorMessage = "Не удалось обновить поле «" + label + "»";
        return false;
    }
    return true;
}

// Обновляет дату в строке «Рецензент ...» и не трогает другие даты.
static bool replaceReviewDate(QDomDocument& document,
    const QString& newDate, QString& errorMessage)
{
    const QRegularExpression datePattern(R"(\b\d{2}\.\d{2}\.\d{4}\b)");
    QDomNode found;
    int start = -1;
    int length = 0;
    int matches = 0;

    QDomNodeList paragraphs = document.elementsByTagName("w:p");
    for (int i = 0; i < paragraphs.count(); ++i) {
        QDomNode paragraph = paragraphs.at(i);
        const QString text = paragraphText(paragraph);
        if (text.trimmed() == "Замечания")
            break;
        if (!text.contains("Рецензент", Qt::CaseInsensitive))
            continue;

        QRegularExpressionMatch match = datePattern.match(text);
        if (!match.hasMatch())
            continue;
        ++matches;
        found = paragraph;
        start = match.capturedStart();
        length = match.capturedLength();
    }

    if (matches != 1) {
        errorMessage = "Не удалось однозначно определить дату рецензента";
        return false;
    }
    if (!replaceTextRange(found, start, length, newDate)) {
        errorMessage = "Не удалось обновить дату рецензента";
        return false;
    }
    return true;
}

// В старом шаблоне {GEN} подставляется после слова «студент»:
// пустая строка => «студент», «ка» => «студентка».
static bool replaceStudentForm(QDomDocument& document,
    const QString& genValue, QString& errorMessage)
{
    const QString normalized = genValue.trimmed().toLower();
    QString newForm;
    if (normalized.isEmpty() || normalized == "студент")
        newForm = "студент";
    else if (normalized == "ка" || normalized == "студентка")
        newForm = "студентка";
    else {
        errorMessage = "Неизвестная форма обращения к студенту: " + genValue;
        return false;
    }

    const QRegularExpression formPattern(R"((^|\s)(студент(?:ка)?)(?=\s|$))",
        QRegularExpression::CaseInsensitiveOption);
    QDomNode found;
    int start = -1;
    int length = 0;
    int matches = 0;

    QDomNodeList paragraphs = document.elementsByTagName("w:p");
    for (int i = 0; i < paragraphs.count(); ++i) {
        QDomNode paragraph = paragraphs.at(i);
        const QString text = paragraphText(paragraph);
        if (text.trimmed() == "Замечания")
            break;
        if (!text.contains("ФДО ТУСУР", Qt::CaseInsensitive))
            continue;
        QRegularExpressionMatch match = formPattern.match(text);
        if (!match.hasMatch())
            continue;
        ++matches;
        found = paragraph;
        start = match.capturedStart(2);
        length = match.capturedLength(2);
    }

    if (matches != 1) {
        errorMessage = "Не удалось однозначно найти форму обращения в рецензии";
        return false;
    }
    if (!replaceTextRange(found, start, length, newForm)) {
        errorMessage = "Не удалось изменить форму обращения";
        return false;
    }
    return true;
}

// Обновление только известных полей уже заполненного документа.
// Замечания, оценка и произвольные правки преподавателя не меняются.
static bool updatePreviousReviewXml(QDomDocument& document,
    const QMap<QString, QString>& replacements, QString& errorMessage)
{
    for (const QString& key : { QString("{DD.MM.YYYY}"), QString("{SPEC}"),
                               QString("{LOGIN}"), QString("{CITY}"), QString("{GEN}") }) {
        if (!replacements.contains(key))
            continue;

        const QString value = replacements.value(key).trimmed();
        // Для формы обращения пустое значение значит «студент».
        if (value.isEmpty() && key != "{GEN}")
            continue;

        // Если заполнитель сохранился, заменяем его напрямую.
        // В данном шаблоне {GEN} — окончание после слова «студент».
        QString placeholderValue = replacements.value(key);
        if (key == "{GEN}") {
            const QString lower = value.toLower();
            if (lower == "студент")
                placeholderValue.clear();
            else if (lower == "студентка")
                placeholderValue = "ка";
        }
        if (replacePlaceholder(document, key, placeholderValue))
            continue;

        if (key == "{DD.MM.YYYY}") {
            if (!replaceReviewDate(document, value, errorMessage))
                return false;
        }
        else if (key == "{SPEC}") {
            if (!replaceFieldByLabel(document, "специальность", "шифр", value, errorMessage))
                return false;
        }
        else if (key == "{LOGIN}") {
            if (!replaceFieldByLabel(document, "шифр", QString(), value, errorMessage))
                return false;
        }
        else if (key == "{CITY}") {
            if (!replaceFieldByLabel(document, "представительство", QString(), value, errorMessage))
                return false;
        }
        else if (key == "{GEN}") {
            if (!replaceStudentForm(document, replacements.value(key), errorMessage))
                return false;
        }
    }
    return true;
}

// Работа с замечаниями
bool replaceRemarksInXml(
    QDomDocument& document,
    const QStringList& newRemarks,
    QString& errorMessage)
{
    if (newRemarks.isEmpty()) {
        errorMessage = "Укажите хотя бы одно замечание";
        return false;
    }

    for (const QString& remark : newRemarks) {
        if (remark.trimmed().isEmpty()) {
            errorMessage = "Замечания не должны быть пустыми";
            return false;
        }
    }

    // Находим заголовок "Замечания"
    QDomNode heading;
    QDomNodeList paragraphs = document.elementsByTagName("w:p");

    for (int i = 0; i < paragraphs.count(); ++i) {
        QDomNode paragraph = paragraphs.at(i);
        QDomNodeList fragments =
            paragraph.toElement().elementsByTagName("w:t");

        QString text;

        for (int j = 0; j < fragments.count(); ++j) {
            text += fragments.at(j).toElement().text();
        }

        if (text.trimmed() == "Замечания") {
            heading = paragraph;
            break;
        }
    }

    if (heading.isNull()) {
        errorMessage = "Раздел «Замечания» не найден";
        return false;
    }

    // Собираем существующие замечания после заголовка
    QList<QDomNode> oldRemarks;

    QDomNode node = heading.nextSibling();

    while (!node.isNull()) {

        if (node.isElement()) {

            // В нашем шаблоне замечания идут подряд
            // до конца раздела
            if (node.nodeName() != "w:p") {
                break;
            }

            QDomNodeList fragments =
                node.toElement().elementsByTagName("w:t");

            QString text;

            for (int j = 0; j < fragments.count(); ++j) {
                text += fragments.at(j).toElement().text();
            }

            if (!text.trimmed().isEmpty()) {
                oldRemarks.append(node);
            }
        }

        node = node.nextSibling();
    }

    if (oldRemarks.isEmpty()) {
        errorMessage = "Не найдены исходные замечания";
        return false;
    }

    // Копируем образец первого пункта списка
    QDomNode example = oldRemarks.first().cloneNode(true);

    // Вспомогательная функция замены текста абзаца
    auto setRemarkText = [&](QDomNode paragraph,
        const QString& newText) -> bool {

            QDomNodeList fragments =
                paragraph.toElement().elementsByTagName("w:t");

            if (fragments.count() == 0) {
                return false;
            }

            for (int j = 0; j < fragments.count(); ++j) {

                QDomElement fragment = fragments.at(j).toElement();

                while (!fragment.firstChild().isNull()) {
                    fragment.removeChild(fragment.firstChild());
                }

                if (j == 0) {
                    fragment.appendChild(
                        document.createTextNode(newText)
                    );

                    fragment.setAttribute("xml:space", "preserve");
                }
            }

            return true;
        };

    int oldCount = oldRemarks.size();
    int newCount = newRemarks.size();

    // Изменяем существующие пункты
    for (int i = 0; i < qMin(oldCount, newCount); ++i) {

        if (!setRemarkText(oldRemarks[i], newRemarks[i])) {
            errorMessage = "Не удалось изменить текст замечания";
            return false;
        }
    }

    QDomNode parent = heading.parentNode();

    // Если новых замечаний меньше — удаляем лишние
    if (newCount < oldCount) {

        for (int i = oldCount - 1; i >= newCount; --i) {

            if (parent.removeChild(oldRemarks[i]).isNull()) {
                errorMessage = "Не удалось удалить замечание";
                return false;
            }
        }
    }

    // Если новых замечаний больше — добавляем пункты
    if (newCount > oldCount) {

        QDomNode lastRemark = oldRemarks.last();

        for (int i = oldCount; i < newCount; ++i) {

            QDomNode newParagraph = example.cloneNode(true);

            if (!setRemarkText(newParagraph, newRemarks[i])) {
                errorMessage = "Не удалось создать замечание";
                return false;
            }

            QDomNode inserted =
                parent.insertAfter(newParagraph, lastRemark);

            if (inserted.isNull()) {
                errorMessage = "Не удалось добавить замечание";
                return false;
            }

            lastRemark = inserted;
        }
    }

    return true;
}

bool ReviewManager::readDocxText(
    const QString& path,
    QString& text,
    QString& errorMessage)
{
    text.clear();
    errorMessage.clear();

    // 1. Читаем DOCX как обычный файл
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly)) {
        errorMessage = "Не удалось открыть DOCX: " + file.errorString();
        return false;
    }

    QByteArray fileData = file.readAll();
    file.close();

    // 2. Открываем DOCX как ZIP-архив
    struct archive* reader = archive_read_new();

    archive_read_support_format_zip(reader);

    if (archive_read_open_memory(
        reader,
        fileData.constData(),
        fileData.size()) != ARCHIVE_OK) {

        errorMessage = "Не удалось прочитать DOCX как ZIP-архив";
        archive_read_free(reader);
        return false;
    }

    // 3. Ищем word/document.xml
    QByteArray xmlData;
    struct archive_entry* entry = nullptr;

    while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {

        QString entryName = QString::fromUtf8(
            archive_entry_pathname(entry)
        );

        if (entryName != "word/document.xml") {
            archive_read_data_skip(reader);
            continue;
        }

        char buffer[8192];
        la_ssize_t count;

        while ((count = archive_read_data(
            reader, buffer, sizeof(buffer))) > 0) {

            xmlData.append(buffer, count);
        }

        if (count < 0) {
            errorMessage = "Ошибка чтения document.xml";
            archive_read_free(reader);
            return false;
        }

        break;
    }

    archive_read_free(reader);

    if (xmlData.isEmpty()) {
        errorMessage = "В DOCX не найден word/document.xml";
        return false;
    }

    // 4. Преобразуем XML в дерево элементов
    QDomDocument document;

    if (!document.setContent(xmlData)) {
        errorMessage = "Не удалось разобрать XML документа";
        return false;
    }

    // 5. Собираем текст каждого абзаца
    QDomNodeList paragraphs = document.elementsByTagName("w:p");

    for (int i = 0; i < paragraphs.count(); ++i) {

        QDomElement paragraph = paragraphs.at(i).toElement();
        QDomNodeList fragments = paragraph.elementsByTagName("w:t");

        for (int j = 0; j < fragments.count(); ++j) {
            text += fragments.at(j).toElement().text();
        }

        text += '\n';
    }

    return true;
}

bool ReviewManager::fillTemplate(
    const QString& inputPath,
    const QString& outputPath,
    const QMap<QString, QString>& replacements,
    QString& errorMessage,
    const QStringList& newRemarks,
    bool updateFilledFields)
{
    errorMessage.clear();

    // Не позволяем перезаписать исходный шаблон
    if (QFileInfo(inputPath).absoluteFilePath().compare(
        QFileInfo(outputPath).absoluteFilePath(),
        Qt::CaseInsensitive) == 0) {
        errorMessage = "Нельзя перезаписывать исходный шаблон";
        return false;
    }

    if (QFile::exists(outputPath)) {
        errorMessage = "Выходной файл уже существует";
        return false;
    }

    QFile file(inputPath);

    if (!file.open(QIODevice::ReadOnly)) {
        errorMessage = "Не удалось открыть шаблон: " + file.errorString();
        return false;
    }

    QByteArray inputData = file.readAll();
    file.close();

    // Открываем DOCX как ZIP
    struct archive* reader = archive_read_new();
    archive_read_support_format_zip(reader);

    if (archive_read_open_memory(
        reader, inputData.constData(),
        static_cast<size_t>(inputData.size())) != ARCHIVE_OK) {
        errorMessage = "Не удалось открыть DOCX как ZIP";
        archive_read_free(reader);
        return false;
    }

    // Создаём новый ZIP
    struct archive* writer = archive_write_new();
    archive_write_set_format_zip(writer);

    // Освобождение ресурсов при ошибке или завершении
    auto cleanup = [&](bool removeOutput) {
        archive_write_free(writer);
        archive_read_free(reader);

        if (removeOutput)
            QFile::remove(outputPath);
        };

    // Используем Unicode-путь для Windows
    QString nativePath = QDir::toNativeSeparators(
        QFileInfo(outputPath).absoluteFilePath()
    );

    if (archive_write_open_filename_w(
        writer,
        reinterpret_cast<const wchar_t*>(nativePath.utf16())
    ) != ARCHIVE_OK) {
        errorMessage = "Не удалось создать выходной DOCX";
        cleanup(true);
        return false;
    }

    struct archive_entry* entry = nullptr;
    char buffer[8192];
    bool xmlFound = false;
    int status;

    while ((status = archive_read_next_header(reader, &entry))
        == ARCHIVE_OK) {

        QString name = QString::fromUtf8(
            archive_entry_pathname(entry)
        );

        QByteArray data;
        la_ssize_t count;

        // Читаем содержимое элемента архива
        while ((count = archive_read_data(
            reader, buffer, sizeof(buffer))) > 0) {
            data.append(buffer, count);
        }

        if (count < 0) {
            errorMessage = "Ошибка чтения элемента DOCX";
            cleanup(true);
            return false;
        }

        if (name == "word/document.xml") {
            xmlFound = true;

            QDomDocument document;

            if (!document.setContent(
                data,
                QDomDocument::ParseOption::PreserveSpacingOnlyNodes))
            {
                errorMessage = "Ошибка разбора XML";
                cleanup(true);
                return false;
            }

            // Для новой рецензии заменяем заполнители, для копии предыдущей
            // обновляем поля по их подписям в шапке документа.
            if (updateFilledFields) {
                if (!updatePreviousReviewXml(document, replacements, errorMessage)) {
                    cleanup(true);
                    return false;
                }
            }
            else {
                for (auto it = replacements.cbegin();
                    it != replacements.cend(); ++it) {

                    if (!replacePlaceholder(document, it.key(), it.value())) {
                        errorMessage = "Не найден заполнитель: " + it.key();
                        cleanup(true);
                        return false;
                    }
                }
            }

            // Если переданы замечания, заменяем их
            if (!newRemarks.isEmpty()) {

                if (!replaceRemarksInXml(
                    document, newRemarks, errorMessage)) {

                    cleanup(true);
                    return false;
                }
            }

            data = document.toByteArray();
        }

        // Сохраняем элемент в новом архиве
        struct archive_entry* newEntry = archive_entry_clone(entry);
        archive_entry_set_size(newEntry, data.size());

        int headerStatus = archive_write_header(writer, newEntry);
        archive_entry_free(newEntry);

        if (headerStatus != ARCHIVE_OK) {
            errorMessage = "Ошибка записи элемента DOCX";
            cleanup(true);
            return false;
        }

        // Записываем все байты элемента
        qsizetype writtenTotal = 0;

        while (writtenTotal < data.size()) {
            la_ssize_t written = archive_write_data(
                writer,
                data.constData() + writtenTotal,
                static_cast<size_t>(data.size() - writtenTotal)
            );

            if (written <= 0) {
                errorMessage = "Ошибка записи данных DOCX";
                cleanup(true);
                return false;
            }

            writtenTotal += written;
        }
    }

    if (status != ARCHIVE_EOF || !xmlFound) {
        errorMessage = "DOCX повреждён или отсутствует document.xml";
        cleanup(true);
        return false;
    }

    if (archive_write_close(writer) != ARCHIVE_OK) {
        errorMessage = "Ошибка завершения записи DOCX";
        cleanup(true);
        return false;
    }

    cleanup(false);
    return true;
}