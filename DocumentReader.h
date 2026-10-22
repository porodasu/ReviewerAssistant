
#pragma once
#include <QString>

class DocumentReader {
public:
    static bool readTxt(const QString& path,
        QString& text,
        QString& errorMessage);

    static bool readDocx(const QString& path,
        QString& text,
        QString& errorMessage);

    static bool readDoc(const QString& path,
        QString& text,
        QString& errorMessage);
};
