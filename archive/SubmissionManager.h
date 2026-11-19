#pragma once

#include <QList>
#include <QString>

// Номер работы используется как первая часть имени каталога попытки:
// 1 -> ЛР №1 (каталоги 11, 12, 13 ...)
// 2 -> ЛР №2 (каталоги 21, 22, 23 ...)
// 3 -> курсовой проект (каталоги 31, 32, 33 ...)
enum class WorkType
{
    Lab1 = 1,
    Lab2 = 2,
    CourseProject = 3
};

struct SubmissionInfo
{
    QString fullName;
    int workNumber = 0;
    int attemptNumber = 0;
    QString folderPath;
    QString reviewPath;
};

class SubmissionManager
{
public:
    // Импортирует один общий архив, содержащий папки студентов.
    // Для каждого студента создаёт новую попытку в:
    // rootDirectory/ООП/ФИО/НомерПопытки
    //
    // Внешний архив и вложенные архивы извлекаются через ArchiveExtractor.
    // При успехе results содержит сведения о созданных попытках.
    // При частичной ошибке уже успешно созданные попытки остаются в results.
    static bool importArchive(
        const QString& archivePath,
        const QString& rootDirectory,
        WorkType workType,
        const QString& templatePath,
        QList<SubmissionInfo>& results,
        QString& errorMessage);
};
