
#include "DocumentReader.h"
#include <QElapsedTimer>
#include <QDebug>
#include <QFile>
#include <QXmlStreamReader>
#include <QByteArray>
#include <archive.h>
#include <archive_entry.h>
#include <string>
#include <QAxObject>
#include <QFileInfo>
#include <QDir>
#include <QVariant>

bool DocumentReader::readTxt(const QString& path,
    QString& text,
    QString& errorMessage)
{
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorMessage = "Не удалось открыть файл: " + file.errorString();
        return false;
    }

    text = QString::fromUtf8(file.readAll());

    return true;
}


bool DocumentReader::readDocx(const QString& path,
    QString& text,
    QString& errorMessage)
{
    text.clear();
    errorMessage.clear();

    // Создаём объект для чтения архива DOCX
    struct archive* reader = archive_read_new();

    if (reader == nullptr) {
        errorMessage = "Не удалось создать объект libarchive";
        return false;
    }

    archive_read_support_filter_all(reader);
    archive_read_support_format_zip(reader);

    // Открываем DOCX с поддержкой путей Windows
    std::wstring widePath = path.toStdWString();

    if (archive_read_open_filename_w(
        reader, widePath.c_str(), 10240) != ARCHIVE_OK) {
        errorMessage = "Не удалось открыть DOCX";
        archive_read_free(reader);
        return false;
    }

    QByteArray xmlData;
    struct archive_entry* entry = nullptr;
    bool found = false;

    int result;

    // Ищем основной XML-документ внутри DOCX
    while ((result = archive_read_next_header(
        reader, &entry)) == ARCHIVE_OK) {

        const char* name = archive_entry_pathname(entry);

        if (name != nullptr &&
            QByteArray(name) == "word/document.xml") {

            found = true;

            char buffer[8192];
            la_ssize_t bytesRead;

            // Считываем XML частями
            while ((bytesRead = archive_read_data(
                reader, buffer, sizeof(buffer))) > 0) {
                xmlData.append(buffer,
                    static_cast<qsizetype>(bytesRead));
            }

            if (bytesRead < 0) {
                errorMessage = "Ошибка чтения содержимого DOCX";
                archive_read_free(reader);
                return false;
            }

            break;
        }
    }

    archive_read_free(reader);

    if (!found) {
        errorMessage = "Не удалось найти word/document.xml";
        return false;
    }

    // Преобразуем XML в обычный текст
    QXmlStreamReader xml(xmlData);

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            QString name = xml.name().toString();

            if (name == "t") {
                text += xml.readElementText();
            }
            else if (name == "tab") {
                text += '\t';
            }
            else if (name == "br" || name == "cr") {
                text += '\n';
            }
        }
        else if (xml.isEndElement() &&
            xml.name().toString() == "p") {
            text += '\n';
        }
    }

    if (xml.hasError()) {
        text.clear();
        errorMessage = "Ошибка обработки XML: "
            + xml.errorString();
        return false;
    }

    return true;
}


bool DocumentReader::readDoc(const QString& path,
    QString& text,
    QString& errorMessage)
{
    text.clear();
    errorMessage.clear();

    

    // Запускаем Microsoft Word через COM
    QAxObject word("Word.Application");

    

    if (word.isNull()) {
        errorMessage = "Не удалось запустить Microsoft Word.";
        return false;
    }

    word.setProperty("Visible", false);
    word.setProperty("DisplayAlerts", 0);

    
    // Получаем исходное значение настройки
    QVariant oldSecurity = word.dynamicCall("AutomationSecurity");

    // Устанавливаем режим отключения макросов
    word.dynamicCall("AutomationSecurity", QVariant(3));

    // Читаем настройку тем же способом
    QVariant currentSecurity = word.dynamicCall("AutomationSecurity");

    if (!currentSecurity.isValid() || currentSecurity.toInt() != 3) {

        errorMessage = QString(
            "Не удалось подтвердить отключение макросов.\n"
            "Исходное значение: %1\n"
            "Текущее значение: %2"
        )
            .arg(oldSecurity.isValid() ? oldSecurity.toString() : "недоступно")
            .arg(currentSecurity.isValid() ? currentSecurity.toString() : "недоступно");

        word.dynamicCall("Quit()");
        return false;
    }

    QAxObject* documents = word.querySubObject("Documents");

    if (documents == nullptr) {
        errorMessage = "Не удалось получить список документов Word.";
        word.dynamicCall("Quit()");
        return false;
    }

    QString fullPath = QDir::toNativeSeparators(
        QFileInfo(path).absoluteFilePath());

    // Открываем документ только для чтения
    QAxObject* document = documents->querySubObject(
        "Open(const QVariant&, const QVariant&, const QVariant&, const QVariant&)",
        QVariant(fullPath),
        QVariant(false),
        QVariant(true),
        QVariant(false)
    );

    

    if (document == nullptr) {
        errorMessage = "Microsoft Word не смог открыть документ.";
        delete documents;
        word.dynamicCall("Quit()");
        return false;
    }

    

    

    QAxObject* content = document->querySubObject("Content");

    

    bool success = (content != nullptr);

    if (success) {

        

        text = content->property("Text").toString();

        

        text.replace('\r', '\n');
        delete content;
    }
    else {
        errorMessage = "Не удалось получить текст документа.";
    }


    delete document;
    delete documents;


    word.dynamicCall("Quit()");



    return success;
}


