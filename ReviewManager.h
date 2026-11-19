#pragma once

#include <QString>
#include <QMap>
#include <QStringList>

class ReviewManager
{
public:
    static bool fillTemplate(
        const QString& inputPath,
        const QString& outputPath,
        const QMap<QString, QString>& replacements,
        QString& errorMessage,
        const QStringList& newRemarks = {}
    );

    static bool readDocxText(
        const QString& path,
        QString& text,
        QString& errorMessage
    );
};