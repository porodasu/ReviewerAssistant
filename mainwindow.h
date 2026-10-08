#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_mainwindow.h"
#include <QString>

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    Ui::MainWindowClass ui;

    QString selectedArchivePath;

    void refreshParticipants(); // функция обновления
    QString participantsPath; // хранение пути участников
};

