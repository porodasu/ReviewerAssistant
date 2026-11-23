#include "mainwindow.h"
#include "DocumentReader.h"
#include "ReviewManager.h"
#include "SubmissionManager.h"
#include <QDir>
#include <QInputDialog>
#include <QDate>
#include <QMap>
#include <QRegularExpression>
#include <QProcess> // позволяет запускать другие программы
#include <QDebug>
#include <QDateTime>
#include <QSettings>
#include "archive/ArchiveExtractor.h" // распоковщик Никиты
#include <filesystem> // для пути
#include <QSaveFile>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QFormLayout>
#include <QDialog> // отдельное диалоговое окно
#include <QDialogButtonBox>
#include <QPushButton>
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

        // Пропускаем резервные копии
        if (file.isFile() &&
            file.fileName().endsWith(".bak", Qt::CaseInsensitive)) {
            continue;
        }

        // Пропускаем служебные файлы блокировки LibreOffice
        if (file.isFile() &&
            file.fileName().startsWith(".~lock.", Qt::CaseInsensitive)) {
            continue;
        }

        QTreeWidgetItem* item = new QTreeWidgetItem(parent);

        item->setText(0, file.fileName()); // хранение ФИО

        item->setData(0, Qt::UserRole, file.absoluteFilePath()); // хранение полного пути

        // Если это папка, открываем её содержимое
        if (file.isDir() && !file.isSymLink()) { // isSymLink - для убирания символических ссылок
            addFolderItems(item, file.absoluteFilePath());
        }
    }
}


MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);

    ui.btnPreviewDocument->setEnabled(false);
    ui.btnEditRemarks->setEnabled(false);
    ui.btnExtractArchive->setEnabled(false);
    ui.btnCreateReview->setEnabled(false);

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


    // Реакция на выбор документа в дереве файлов
    connect(ui.treeFiles, &QTreeWidget::currentItemChanged,
        this, [this](QTreeWidgetItem* current, QTreeWidgetItem*) {

            // Если элемент не выбран
            if (current == nullptr) {
                ui.btnPreviewDocument->setEnabled(false);
                ui.btnEditRemarks->setEnabled(false);
                ui.btnCreateReview->setEnabled(false);
                return;
            }

            // Получаем путь выбранного элемента
            QString path = current->data(0, Qt::UserRole).toString();

            // Разрешаем создание рецензии при выборе папки
            QFileInfo selectedInfo(path);

            ui.btnCreateReview->setEnabled(selectedInfo.isDir());

            QFileInfo reviewFile(path);

            bool canEditRemarks =
                reviewFile.isFile() &&
                reviewFile.suffix().compare("docx", Qt::CaseInsensitive) == 0 &&
                reviewFile.fileName().contains("Рецензия", Qt::CaseInsensitive);

            ui.btnEditRemarks->setEnabled(canEditRemarks);
            QFileInfo file(path);

            // Проверяем, что это существующий файл .docx
            QString extension = file.suffix().toLower();

            bool canPreview = file.isFile() &&
                (extension == "txt" ||
                    extension == "docx" ||
                    extension == "doc");

            ui.btnPreviewDocument->setEnabled(canPreview);
        });

    // Открываем окно с замечаниями рецензии
    connect(ui.btnEditRemarks, &QPushButton::clicked,
        this, [this]() {

            QTreeWidgetItem* item = ui.treeFiles->currentItem();

            if (item == nullptr) {
                return;
            }

            QString path = item->data(0, Qt::UserRole).toString();

            QString documentText;
            QString errorMessage;

            // Читаем содержимое DOCX
            if (!ReviewManager::readDocxText(
                path, documentText, errorMessage, true)) {

                QMessageBox::warning(this, "Ошибка", errorMessage);
                return;
            }

            // Запоминаем состояние исходного файла
            QFileInfo openedFile(path);

            QDateTime openedModified = openedFile.lastModified();
            qint64 openedSize = openedFile.size();

            // Разбиваем текст документа на строки
            QStringList lines = documentText.split('\n');

            int headingIndex = -1;

            // Ищем раздел "Замечания"
            for (int i = 0; i < lines.size(); ++i) {

                if (lines[i].trimmed() == "Замечания") {
                    headingIndex = i;
                    break;
                }
            }

            if (headingIndex == -1) {
                QMessageBox::warning(this, "Ошибка",
                    "Раздел «Замечания» не найден");
                return;
            }

            // Получаем текст после заголовка
            QString remarks = lines.mid(headingIndex + 1)
                .join("\n").trimmed();

            // Создаём диалоговое окно
            QDialog dialog(this);
            dialog.setWindowTitle("Замечания: " + QFileInfo(path).fileName());
            dialog.resize(700, 500);

            QVBoxLayout layout(&dialog);

            QPlainTextEdit textEdit;
            textEdit.setPlainText(remarks);
            textEdit.setReadOnly(false);

            layout.addWidget(&textEdit);

            QDialogButtonBox buttons(QDialogButtonBox::Close);

            QPushButton* saveButton = buttons.addButton(
                "Сохранить",
                QDialogButtonBox::ActionRole
            );

            saveButton->setEnabled(true);

            // Сохраняем изменённые замечания
            connect(saveButton, &QPushButton::clicked,
                &dialog, [this, &dialog, &textEdit, path,
                openedModified, openedSize]() {


                    QString input = textEdit.toPlainText().trimmed();

                    QStringList newRemarks;

                    // Если пользователь оставил хотя бы одно замечание
                    if (!input.isEmpty()) {

                        newRemarks = input.split('\n', Qt::KeepEmptyParts);

                        for (QString& remark : newRemarks) {

                            remark = remark.trimmed();

                            if (remark.isEmpty()) {
                                QMessageBox::warning(this, "Ошибка",
                                    "Между замечаниями не должно быть пустых строк.");
                                return;
                            }
                        }
                    }
                    else {

                        // Подтверждение удаления всех замечаний
                        QMessageBox::StandardButton answer =
                            QMessageBox::question(
                                this,
                                "Удаление замечаний",
                                "Вы действительно хотите удалить все замечания?",
                                QMessageBox::Yes | QMessageBox::No,
                                QMessageBox::No
                            );

                        if (answer != QMessageBox::Yes) {
                            return;
                        }
                    }


                    // Проверяем исходный документ перед сохранением
                    QFileInfo fileInfo(path);

                    // Проверяем, не открыт ли документ в LibreOffice
                    QString lockPath = QDir(fileInfo.absolutePath()).filePath(
                        ".~lock." + fileInfo.fileName() + "#"
                    );

                    if (QFile::exists(lockPath)) {
                        QMessageBox::warning(this, "Документ открыт",
                            "Рецензия, возможно, открыта в LibreOffice.\n"
                            "Закройте её в LibreOffice и повторите сохранение.");
                        return;
                    }

                    // Проверяем, не изменился ли исходный документ
                    if (!fileInfo.isFile() ||
                        fileInfo.lastModified() != openedModified ||
                        fileInfo.size() != openedSize) {

                        QMessageBox::warning(this, "Файл изменён",
                            "Документ был изменён после открытия редактора.\n"
                            "Закройте окно замечаний и откройте рецензию заново.");

                        return;
                    }

                    // Создаём резервную копию исходной рецензии
                    QString timestamp = QDateTime::currentDateTime()
                        .toString("yyyyMMdd_HHmmss_zzz");

                    QString backupPath = QDir(fileInfo.absolutePath()).filePath(
                        fileInfo.fileName() + "." + timestamp + ".bak"
                    );

                    // Проверяем создание резервной копии
                    if (!QFile::copy(path, backupPath)) {
                        QMessageBox::warning(this, "Ошибка",
                            "Не удалось создать резервную копию рецензии.");
                        return;
                    }

                    qDebug() << "Резервная копия создана:" << backupPath;

                    // Создаём временный DOCX рядом с исходным файлом
                    QString tempPath = QDir(fileInfo.absolutePath()).filePath(
                        fileInfo.completeBaseName() + "_временный_" +
                        timestamp + ".docx"
                    );

                    QString errorMessage;

                    // Сначала формируем полностью готовую рецензию
                    if (!ReviewManager::fillTemplate(
                        path,
                        tempPath,
                        {},
                        errorMessage,
                        newRemarks,
                        false,
                        true)) {

                        QFile::remove(tempPath);

                        QMessageBox::warning(this, "Ошибка сохранения",
                            errorMessage);
                        return;
                    }

                    // Читаем готовый временный DOCX
                    QFile tempFile(tempPath);

                    if (!tempFile.open(QIODevice::ReadOnly)) {
                        QFile::remove(tempPath);

                        QMessageBox::warning(this, "Ошибка",
                            "Не удалось прочитать временный документ.");
                        return;
                    }

                    QByteArray data = tempFile.readAll();

                    bool readSuccess = (data.size() == tempFile.size());
                    tempFile.close();

                    if (!readSuccess) {
                        QFile::remove(tempPath);

                        QMessageBox::warning(this, "Ошибка",
                            "Не удалось полностью прочитать временный документ.");
                        return;
                    }

                    // Безопасно записываем данные в исходную рецензию
                    QSaveFile saveFile(path);

                    if (!saveFile.open(QIODevice::WriteOnly)) {
                        QFile::remove(tempPath);

                        QMessageBox::warning(this, "Ошибка",
                            "Не удалось открыть исходный файл для сохранения:\n" +
                            saveFile.errorString());
                        return;
                    }

                    if (saveFile.write(data) != data.size()) {
                        saveFile.cancelWriting();
                        QFile::remove(tempPath);

                        QMessageBox::warning(this, "Ошибка",
                            "Не удалось записать документ.");
                        return;
                    }

                    // Завершаем запись и заменяем исходный файл
                    if (!saveFile.commit()) {
                        QFile::remove(tempPath);

                        QMessageBox::warning(this, "Ошибка",
                            "Не удалось завершить сохранение:\n" +
                            saveFile.errorString());
                        return;
                    }

                    // Удаляем больше не нужный временный файл
                    QFile::remove(tempPath);

                    QMessageBox::information(this, "Готово",
                        "Изменения сохранены в исходную рецензию.\n"
                        "Резервная копия создана:\n" + backupPath);

                    dialog.accept();
                });

            QPushButton* openButton = buttons.addButton(
                "Открыть весь документ в LibreOffice",
                QDialogButtonBox::ActionRole
            );

            connect(openButton, &QPushButton::clicked,
                &dialog, [this, path]() {

                    QString libreOffice =
                        "C:/Program Files/LibreOffice/program/soffice.exe";

                    if (!QFileInfo::exists(libreOffice)) {
                        QMessageBox::warning(this, "Ошибка",
                            "LibreOffice не найден");
                        return;
                    }

                    if (!QProcess::startDetached(libreOffice, QStringList{ path })) {
                        QMessageBox::warning(this, "Ошибка",
                            "Не удалось запустить LibreOffice");
                    }
                });

            connect(&buttons, &QDialogButtonBox::rejected,
                &dialog, &QDialog::reject);

            layout.addWidget(&buttons);

            dialog.exec();
        });

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

            if (selectedArchivePath.isEmpty())
                return;

            // Выбираем тип работы
            QStringList workOptions = {
                "Лабораторная работа №1",
                "Лабораторная работа №2",
                "Курсовой проект"
            };

            bool ok = false;

            QString selection = QInputDialog::getItem(
                this,
                "Импорт работ",
                "Выберите тип работы:",
                workOptions,
                0,
                false,
                &ok
            );

            if (!ok)
                return;

            WorkType workType = WorkType::Lab1;

            if (selection == workOptions[1])
                workType = WorkType::Lab2;
            else if (selection == workOptions[2])
                workType = WorkType::CourseProject;

            // Корневой каталог приложения
            QString documents = QStandardPaths::writableLocation(
                QStandardPaths::DocumentsLocation);

            QString rootPath = QDir(documents).filePath(
                "ReviewerAssistant");

            // Общий шаблон рецензии
            QString templatePath = QDir(rootPath).filePath(
                "Шаблоны/Рецензия_шаблон.docx");

            if (!QFileInfo(templatePath).isFile()) {
                QMessageBox::warning(
                    this,
                    "Ошибка",
                    "Шаблон рецензии не найден:\n" + templatePath
                );
                return;
            }

            QList<SubmissionInfo> results;
            QString errorMessage;

            // Импортируем архив через модуль напарника
            bool success = SubmissionManager::importArchive(
                selectedArchivePath,
                rootPath,
                workType,
                templatePath,
                results,
                errorMessage
            );

            // Обновляем список даже при частичной ошибке
            refreshParticipants();

            if (!success) {
                QMessageBox::warning(
                    this,
                    "Ошибка импорта",
                    errorMessage +
                    "\n\nУспешно импортировано работ: " +
                    QString::number(results.size())
                );
                return;
            }

            // Защита от повторного импорта тем же нажатием
            selectedArchivePath.clear();
            ui.lblArchivePath->setText("Архив не выбран");
            ui.lblArchivePath->setToolTip(QString());
            ui.btnExtractArchive->setEnabled(false);

            QMessageBox::information(
                this,
                "Готово",
                "Импорт успешно завершён.\n"
                "Количество работ: " +
                QString::number(results.size())
            );
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



    connect(ui.btnCreateReview, &QPushButton::clicked,
        this, [this]() {

            // Получаем выбранного студента и папку
            QTreeWidgetItem* student = ui.treeParticipants->currentItem();
            QTreeWidgetItem* folder = ui.treeFiles->currentItem();

            if (student == nullptr || folder == nullptr) {
                QMessageBox::warning(this, "Ошибка",
                    "Выберите студента и папку работы.");
                return;
            }

            QString studentName = student->text(0);
            QString folderPath = folder->data(0, Qt::UserRole).toString();

            if (!QFileInfo(folderPath).isDir()) {
                QMessageBox::warning(this, "Ошибка",
                    "Необходимо выбрать папку работы.");
                return;
            }

            // Разделяем ФИО
            QStringList nameParts = studentName.simplified().split(
                ' ', Qt::SkipEmptyParts
            );

            if (nameParts.size() != 3) {
                QMessageBox::warning(this, "Ошибка",
                    "ФИО студента должно содержать "
                    "фамилию, имя и отчество.");
                return;
            }

            // Формируем фамилию и инициалы
            QString shortName =
                nameParts[0] + " " +
                nameParts[1].left(1) + "." +
                nameParts[2].left(1) + ".";

            // Путь к рабочей папке
            QString documents = QStandardPaths::writableLocation(
                QStandardPaths::DocumentsLocation
            );

            QDir workDir(QDir(documents).filePath("ReviewerAssistant"));

            // Путь к шаблону
            QString templatePath = workDir.filePath(
                "Шаблоны/Рецензия_шаблон.docx"
            );

            if (!QFileInfo::exists(templatePath)) {
                QMessageBox::warning(this, "Ошибка",
                    "Шаблон рецензии не найден:\n" + templatePath);
                return;
            }

            // Загружаем сохранённые сведения о студенте
            QString settingsPath = workDir.filePath("student_data.ini");

            QSettings settings(settingsPath, QSettings::IniFormat);
            settings.beginGroup(studentName);

            QString savedSpec = settings.value(
                "specialty", ""
            ).toString();

            QString savedLogin = settings.value(
                "login", ""
            ).toString();

            QString savedCity = settings.value(
                "city", ""
            ).toString();

            QString savedForm = settings.value(
                "studentForm", "Студент"
            ).toString();

            settings.endGroup();

            // Создаём одно окно со всеми параметрами
            QDialog dataDialog(this);
            dataDialog.setWindowTitle("Создание рецензии");
            dataDialog.setMinimumWidth(520);

            QFormLayout form(&dataDialog);

            // Номер лабораторной работы
            QSpinBox labEdit(&dataDialog);
            labEdit.setRange(1, 2);
            labEdit.setValue(1);

            // Получаем название выбранной папки
            QString folderName = QFileInfo(folderPath).fileName();

            // Проверяем формат: номер ЛР + номер попытки
            QRegularExpression pattern("^([12])([1-9][0-9]*)$");
            QRegularExpressionMatch match = pattern.match(folderName);

            if (match.hasMatch()) {

                // Первая цифра — номер лабораторной
                int detectedLab = match.captured(1).toInt();

                labEdit.setValue(detectedLab);

                // Запрещаем менять определённый номер вручную
                labEdit.setEnabled(false);
            }

            // Форма обращения
            QComboBox studentFormEdit(&dataDialog);
            studentFormEdit.addItems({
                "Студент",
                "Студентка"
                });

            int formIndex = studentFormEdit.findText(savedForm);
            if (formIndex >= 0) {
                studentFormEdit.setCurrentIndex(formIndex);
            }

            // Дополнительные сведения
            QLineEdit specEdit(&dataDialog);
            QLineEdit loginEdit(&dataDialog);
            QLineEdit cityEdit(&dataDialog);

            specEdit.setText(savedSpec);
            loginEdit.setText(savedLogin);
            cityEdit.setText(savedCity);

            specEdit.setMinimumWidth(280);
            loginEdit.setMinimumWidth(280);
            cityEdit.setMinimumWidth(280);

            // Добавляем элементы в форму
            form.addRow("Номер лабораторной:", &labEdit);
            form.addRow("Форма обращения:", &studentFormEdit);
            form.addRow("Специальность:", &specEdit);
            form.addRow("Шифр студента:", &loginEdit);
            form.addRow("Представительство:", &cityEdit);

            // Кнопки подтверждения и отмены
            QDialogButtonBox buttons(
                QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                &dataDialog
            );

            form.addRow(&buttons);

            connect(&buttons, &QDialogButtonBox::accepted,
                &dataDialog, &QDialog::accept);

            connect(&buttons, &QDialogButtonBox::rejected,
                &dataDialog, &QDialog::reject);

            // Если пользователь нажал Cancel
            if (dataDialog.exec() != QDialog::Accepted) {
                return;
            }

            // Получаем значения из окна
            int labNumber = labEdit.value();
            QString studentForm = studentFormEdit.currentText();

            // Формируем имя рецензии
            QString fileName = QString(
                "%1_Рецензия_ЛР%2.docx"
            ).arg(shortName).arg(labNumber);

            QString outputPath = QDir(folderPath).filePath(fileName);

            // Не перезаписываем существующий документ
            if (QFileInfo::exists(outputPath)) {
                QMessageBox::warning(this, "Ошибка",
                    "Рецензия уже существует:\n" + outputPath);
                return;
            }

            // Ищем рецензию из предыдущих попыток
            QString previousReviewPath;

            if (match.hasMatch()) {

                // Номер текущей попытки
                int currentAttempt = match.captured(2).toInt();

                // Папка студента
                QDir studentDir = QFileInfo(folderPath).dir();

                // Проверяем предыдущие попытки от новой к старой
                for (int attempt = currentAttempt - 1; attempt >= 1; --attempt) {

                    QString previousFolderName =
                        QString("%1%2").arg(labNumber).arg(attempt);

                    QString previousFolderPath =
                        studentDir.filePath(previousFolderName);

                    QString candidatePath =
                        QDir(previousFolderPath).filePath(fileName);

                    if (QFileInfo(candidatePath).isFile()) {
                        previousReviewPath = candidatePath;
                        break;
                    }
                }
            }

            // Временная проверка поиска
            if (!previousReviewPath.isEmpty()) {
                QMessageBox::information(this,
                    "Предыдущая рецензия найдена",
                    previousReviewPath);
            }
            else {
                QMessageBox::information(this,
                    "Предыдущая рецензия",
                    "Предыдущая рецензия не найдена. "
                    "Будет использоваться общий шаблон.");
            }

            // Заполнители шаблона
            QMap<QString, QString> replacements;

            replacements.insert(
                "{ID}", QString::number(labNumber)
            );

            replacements.insert(
                "{DISCIPLINE}",
                "Объектно-ориентированное программирование - " +
                QString::number(labNumber)
            );

            replacements.insert("{FIO}", shortName);

            replacements.insert(
                "{GEN}",
                studentForm == "Студентка" ? "ка" : ""
            );

            replacements.insert(
                "{DD.MM.YYYY}",
                QDate::currentDate().toString("dd.MM.yyyy")
            );

            // Дополнительные сведения
            if (!specEdit.text().trimmed().isEmpty()) {
                replacements.insert(
                    "{SPEC}", specEdit.text().trimmed()
                );
            }

            if (!loginEdit.text().trimmed().isEmpty()) {
                replacements.insert(
                    "{LOGIN}", loginEdit.text().trimmed()
                );
            }

            if (!cityEdit.text().trimmed().isEmpty()) {
                replacements.insert(
                    "{CITY}", cityEdit.text().trimmed()
                );
            }

            // Создаём DOCX


            QString errorMessage;
            bool created = false;

            if (!previousReviewPath.isEmpty()) {

                // Новая попытка: используем предыдущую рецензию
                // и обновляем известные реквизиты.
                created = ReviewManager::fillTemplate(
                    previousReviewPath,
                    outputPath,
                    replacements,
                    errorMessage,
                    {},
                    true
                );
            }
            else {

                // Первая попытка: используем общий шаблон.
                created = ReviewManager::fillTemplate(
                    templatePath,
                    outputPath,
                    replacements,
                    errorMessage
                );
            }

            // Проверяем успешность создания
            if (!created) {
                QMessageBox::warning(
                    this,
                    "Ошибка создания рецензии",
                    errorMessage
                );
                return;
            }


            // Сохраняем данные студента после успешного создания
            settings.beginGroup(studentName);

            settings.setValue(
                "specialty", specEdit.text().trimmed()
            );

            settings.setValue(
                "login", loginEdit.text().trimmed()
            );

            settings.setValue(
                "city", cityEdit.text().trimmed()
            );

            settings.setValue("studentForm", studentForm);

            settings.endGroup();
            settings.sync();

            if (settings.status() != QSettings::NoError) {
                QMessageBox::warning(this,
                    "Предупреждение",
                    "Рецензия создана, но сведения о студенте "
                    "не удалось сохранить.");
            }

            // Обновляем дерево файлов выбранной папки
            while (folder->childCount() > 0) {
                delete folder->takeChild(0);
            }

            addFolderItems(folder, folderPath);
            folder->setExpanded(true);

            QMessageBox::information(this, "Готово",
                "Рецензия успешно создана:\n" + outputPath);
        });



}

MainWindow::~MainWindow()
{
}


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


