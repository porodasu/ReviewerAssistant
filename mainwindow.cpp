#include "mainwindow.h"
#include "DocumentReader.h"
#include <QDir>
#include "archive/ArchiveExtractor.h" // распоковщик Никиты
#include <filesystem> // для пути
#include <QDialog> // отдельное диалоговое окно
#include <QPlainTextEdit> // отображает обычный текст
#include <QVBoxLayout> // вертикальное расположение
#include <QFile> // взаимодействие с файлами
#include <QStandardPaths> // для нахождения стандартных путей
#include <QMessageBox> // отображение
#include <QStatusBar> // отобрадение чего-либо в нижнем углу (logs)
#include <QTreeWidgetItem> // для каталогов участников
#include <QFileInfo> // для просмотра информации о файле
#include <QDesktopServices> // доступ к рабочему столу (действиям)
#include <QUrl> // прямое обращение по ссылкам
#include <QFileDialog> // выбор папок и файлов в графическом интерфейсе



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


    connect(ui.treeFiles, &QTreeWidget::currentItemChanged,
        this, [this](QTreeWidgetItem* current, QTreeWidgetItem*) {

            // Если элемент не выбран
            if (current == nullptr) {
                ui.btnPreviewDocument->setEnabled(false);
                return;
            }

            // Получаем путь выбранного элемента
            QString path = current->data(0, Qt::UserRole).toString();
            QFileInfo file(path);

            // Проверяем, что это существующий файл .docx
            QString extension = file.suffix().toLower();

            bool canPreview = file.isFile() &&
                (extension == "txt" ||
                 extension == "docx" ||
                 extension == "doc");

            ui.btnPreviewDocument->setEnabled(canPreview);
        });

    // нажатие

    connect(ui.btnPreviewDocument, &QPushButton::clicked,
        this, [this]() {

            QTreeWidgetItem* item = ui.treeFiles->currentItem();

            if (item == nullptr) {
                return;
            }

            QString path = item->data(0, Qt::UserRole).toString();

            QString text;
            QString errorMessage;

            // Чтение файла через DocumentReader

            QString extension = QFileInfo(path).suffix().toLower();
            bool success = false;

            if (extension == "txt") {
                success = DocumentReader::readTxt(path, text, errorMessage);
            }
            else if (extension == "docx") {
                success = DocumentReader::readDocx(path, text, errorMessage);
            }

            else if (extension == "doc") {
                success = DocumentReader::readDoc(path, text, errorMessage);
            }

            if (!success) {
                QMessageBox::warning(this, "Ошибка", errorMessage);
                return;
            }


            // Создаём окно просмотра
            QDialog dialog(this);
            dialog.setWindowTitle("Просмотр: " + QFileInfo(path).fileName());
            dialog.resize(700, 500);

            QVBoxLayout layout(&dialog);

            QPlainTextEdit textEdit;
            textEdit.setReadOnly(true);
            textEdit.setPlainText(text);

            layout.addWidget(&textEdit);

            dialog.exec();
        });




    // Выбор архива
    connect(ui.btnSelectArchive, &QPushButton::clicked,
        this, [this]() {

            QString path = QFileDialog::getOpenFileName( // открытие окна выбора файла
                this,
                "Выберите архив",
                QDir::homePath(), // начальная папка пользователя
                "ZIP-архивы (*.zip)"
            );

            // Если пользователь ничего не выбрал
            if (path.isEmpty()) {
                return;
            }

            // Запоминаем путь
            selectedArchivePath = path;

            // Показываем название выбранного архива
            ui.lblArchivePath->setText(QFileInfo(path).fileName());
            ui.lblArchivePath->setToolTip(path);

            // Разрешаем распаковку
            ui.btnExtractArchive->setEnabled(true);
        });

    // Распаковка выбранного архива
    connect(ui.btnExtractArchive, &QPushButton::clicked,
        this, [this]() {

            if (selectedArchivePath.isEmpty()) {
                return;
            }

            ArchiveExtractor extractor;
            std::string errorMessage;

            bool success = extractor.extract(
                std::filesystem::path(selectedArchivePath.toStdWString()),
                std::filesystem::path(participantsPath.toStdWString()),
                errorMessage
            );

            if (success) {
                refreshParticipants();

                QMessageBox::information(this, "Готово",
                    "Архив успешно распакован!");
            }
            else {
                QMessageBox::warning(this, "Ошибка распаковки",

                    QString::fromUtf8(errorMessage.c_str()));

            }
        });




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

    /* Открываем папку участников  */

    // Запоминаем путь к папке участников
    participantsPath = dir.filePath(folder);
    QDir participantsDir(participantsPath);

    // Загружаем участников при запуске
    refreshParticipants();

    // Обновляем список по нажатию кнопки
    connect(ui.btnRefreshParticipants, &QPushButton::clicked,
        this, [this]() {
            refreshParticipants();
        });


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


void MainWindow::refreshParticipants()
{
    // Очищаем старые списки
    ui.treeParticipants->clear();
    ui.treeFiles->clear();

    QDir dir(participantsPath);

    // Проверяем существование каталога
    if (!dir.exists()) {
        QMessageBox::warning(this, "Ошибка",
            "Папка участников не найдена");
        return;
    }

    // Получаем список папок
    QStringList folders = dir.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name
    );

    // Добавляем участников
    for (const QString& name : folders) {
        QTreeWidgetItem* item =
            new QTreeWidgetItem(ui.treeParticipants);

        item->setText(0, name);
    }

    statusBar()->showMessage("Список участников обновлён");
}


