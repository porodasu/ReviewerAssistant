#pragma once

#include <QString>
#include <QStringList>
#include <QMap>

class ReviewManager
{
public:
    static bool fillTemplate(
        const QString& inputPath,
        const QString& outputPath,
        const QMap<QString, QString>& replacements,
        QString& errorMessage,
        const QStringList& newRemarks = {},
        bool updateFilledFields = false,
        bool editRemarks = false
    );

    static bool readDocxText(
        const QString& path,
        QString& text,
        QString& errorMessage,
        bool remarksOnly = false
    );
};
