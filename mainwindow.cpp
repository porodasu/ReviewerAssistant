#include "mainwindow.h"
#include <QDir>
#include <QStandardPaths> // для нахождения стандартных путей
#include <QMessageBox> // отображение
#include <QStatusBar> // отобрадение чего-либо в нижнем углу (logs)
#include <QTreeWidgetItem> // для каталогов участников
#include <QFileInfo> // для просмотра информации о файле
#include <QDesktopServices> // доступ к рабочему столу (действиям)
#include <QUrl> // прямое обращение по ссылкам



void addFolderItems(QTreeWidgetItem* parent, const QString& path) // открытие через рекурсию
{
    QDir dir(path);

    QFileInfoList files = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::Name
    );

    for (const QFileInfo& file : files) {
        QTreeWidgetItem* item = new QTreeWidgetItem(parent);

        item->setText(0, file.fileName()); // хранение ФИО

        item->setData(0, Qt::UserRole, file.absoluteFilePath()); // хранение полного пути

        // Если это папка, открываем её содержимое
        if (file.isDir() && !file.isSymLink()) { // isSymLink - для убирания символических ссылок
            addFolderItems(item, file.absoluteFilePath());
        }
    }
}


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);

    // Находим папку "Документы"
    QString documents = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);

    if (documents.isEmpty()) {
        QMessageBox::critical(this, "Ошибка",
            "Не удалось найти папку Документы");
        return;
    }

    QDir dir(documents);

    // Проверяем и создаём рабочий каталог
    QString folder = "ReviewerAssistant/Участники";

    if (!dir.mkpath(folder)) {
        QMessageBox::critical(this, "Ошибка",
            "Не удалось создать рабочий каталог");
        return;
    }

    statusBar()->showMessage("Рабочий каталог готов");

    // Открываем папку участников
    QDir participantsDir(dir.filePath(folder));

    // Получаем список папок участников
    QStringList folders = participantsDir.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name
    );

    // Добавляем участников в дерево
    for (const QString& name : folders) {
        QTreeWidgetItem* item =
            new QTreeWidgetItem(ui.treeParticipants);

        item->setText(0, name);
    }

    // При выборе участника показываем его файлы (через сигналы)

    connect(ui.treeParticipants, &QTreeWidget::itemClicked,
        this, [this, participantsDir](QTreeWidgetItem* item, int) {

            // Очищаем список файлов
            ui.treeFiles->clear();

            // Получаем путь к папке выбранного участника
            QString path = participantsDir.filePath(item->text(0));

            // Загружаем все папки и файлы
            addFolderItems(ui.treeFiles->invisibleRootItem(), path); // invisibleRootItem() - для невидимого каталога
        });


    // Открываем файл двойным щелчком
    connect(ui.treeFiles, &QTreeWidget::itemDoubleClicked,
        this, [this](QTreeWidgetItem* item, int) {

            // Получаем полный путь к файлу
            QString path = item->data(0, Qt::UserRole).toString();

            QFileInfo file(path);

            // Папки не открываем как документы
            if (!file.exists() || !file.isFile()) {
                return;
            }

            // Открываем файл стандартной программой Windows
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
                QMessageBox::warning(this, "Ошибка",
                    "Не удалось открыть выбранный файл");
            }
        });



}

MainWindow::~MainWindow()
{}

