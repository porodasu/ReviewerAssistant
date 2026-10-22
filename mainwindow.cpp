#include "mainwindow.h"
#include <QDir>
#include "archive/ArchiveExtractor.h"
#include <filesystem>
#include <QStandardPaths>
#include <QMessageBox>
#include <QStatusBar>
#include <QTreeWidgetItem>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>
#include <QFileDialog>
#include <QFileIconProvider>


void addFolderItems(QTreeWidgetItem* parent, const QString& path)
{
    QDir dir(path);
    QFileInfoList files = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::Name
    );

    static QFileIconProvider iconProvider; // Иконки папок и файлов

    for (const QFileInfo& file : files) {
        QTreeWidgetItem* item = new QTreeWidgetItem(parent);
        item->setIcon(0, iconProvider.icon(file));
        item->setText(0, file.fileName());
        item->setData(0, Qt::UserRole, file.absoluteFilePath());

        if (file.isDir() && !file.isSymLink()) {
            addFolderItems(item, file.absoluteFilePath());
        }
    }
}


MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);

    // Выбор архива
    connect(ui.btnSelectArchive, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "Выберите архив", QDir::homePath(), "ZIP-архивы (*.zip)");
        if (path.isEmpty()) return;

        selectedArchivePath = path;
        ui.lblArchivePath->setText(QFileInfo(path).fileName());
        ui.lblArchivePath->setToolTip(path);
        ui.btnExtractArchive->setEnabled(true);
        });

    // Распаковка архива
    connect(ui.btnExtractArchive, &QPushButton::clicked, this, [this]() {
        if (selectedArchivePath.isEmpty()) return;

        ArchiveExtractor extractor;
        std::string errorMessage;

        bool success = extractor.extract(
            std::filesystem::path(selectedArchivePath.toStdWString()),
            std::filesystem::path(participantsPath.toStdWString()),
            errorMessage
        );

        if (success) {
            refreshParticipants(); // Автоматически обновляем интерфейс
            QMessageBox::information(this, "Готово", "Архив успешно распакован!");
        }
        else {
            QMessageBox::warning(this, "Ошибка распаковки", QString::fromUtf8(errorMessage.c_str()));
        }
        });

    // Находим папку "Документы"
    QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty()) {
        QMessageBox::critical(this, "Ошибка", "Не удалось найти папку Документы");
        return;
    }

    QDir dir(documents);
    QString folder = "ReviewerAssistant/Участники";

    if (!dir.mkpath(folder)) {
        QMessageBox::critical(this, "Ошибка", "Не удалось создать рабочий каталог");
        return;
    }

    participantsPath = dir.filePath(folder);
    QDir participantsDir(participantsPath);

    refreshParticipants();

    // Обновление списка по кнопке
    connect(ui.btnRefreshParticipants, &QPushButton::clicked, this, [this]() {
        refreshParticipants();
        });

    // ИСПОЛЬЗУЕМ itemSelectionChanged ВМЕСТО itemClicked (надежнее)
    connect(ui.treeParticipants, &QTreeWidget::itemSelectionChanged, this, [this, participantsDir]() {
        auto selected = ui.treeParticipants->selectedItems();
        if (selected.isEmpty()) {
            ui.treeFiles->clear();
            return;
        }
        QTreeWidgetItem* item = selected.first();
        ui.treeFiles->clear();
        QString path = participantsDir.filePath(item->text(0));
        addFolderItems(ui.treeFiles->invisibleRootItem(), path);
        });

    // Открытие файла
    connect(ui.treeFiles, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        QString path = item->data(0, Qt::UserRole).toString();
        QFileInfo file(path);

        if (!file.exists() || !file.isFile()) return;

        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
            QMessageBox::warning(this, "Ошибка", "Не удалось открыть выбранный файл");
        }
        });
}

MainWindow::~MainWindow()
{
}

void MainWindow::refreshParticipants()
{
    // 1. Запоминаем, кто был выбран
    QString currentParticipant;
    auto selectedItems = ui.treeParticipants->selectedItems();
    if (!selectedItems.isEmpty()) {
        currentParticipant = selectedItems.first()->text(0);
    }

    // 2. Блокируем сигналы (чтобы не было двойных срабатываний и дубликатов папок)
    ui.treeParticipants->blockSignals(true);
    ui.treeParticipants->clear();
    ui.treeFiles->clear();

    QDir dir(participantsPath);
    if (!dir.exists()) {
        ui.treeParticipants->blockSignals(false);
        return;
    }

    // 3. Заполняем участников
    QStringList folders = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    QTreeWidgetItem* itemToSelect = nullptr;

    for (const QString& name : folders) {
        QTreeWidgetItem* item = new QTreeWidgetItem(ui.treeParticipants);
        item->setText(0, name);
        if (name == currentParticipant) {
            itemToSelect = item;
        }
    }

    // 4. Восстанавливаем выделение
    if (itemToSelect) {
        ui.treeParticipants->setCurrentItem(itemToSelect);
    }
    else if (ui.treeParticipants->topLevelItemCount() > 0) {
        ui.treeParticipants->setCurrentItem(ui.treeParticipants->topLevelItem(0));
    }

    // 5. Разблокируем сигналы
    ui.treeParticipants->blockSignals(false);

    // 6. ВРУЧНУЮ загружаем файлы для выбранного участника
    if (ui.treeParticipants->currentItem()) {
        QString path = dir.filePath(ui.treeParticipants->currentItem()->text(0));
        addFolderItems(ui.treeFiles->invisibleRootItem(), path);
    }

    statusBar()->showMessage("Список участников и файлов обновлён");
}