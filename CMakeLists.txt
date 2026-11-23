cmake_minimum_required(VERSION 3.16)
project(ReviewerAssistant LANGUAGES CXX)

include(qt.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED
    COMPONENTS
        Core
        Gui
        Widgets
        AxContainer
        Xml
)

find_package(LibArchive REQUIRED)

qt_standard_project_setup()

# Исходники главного приложения
set(PROJECT_SOURCES
    main.cpp
    mainwindow.ui
    mainwindow.h
    mainwindow.cpp

    DocumentReader.h
    DocumentReader.cpp
    ReviewManager.h
    ReviewManager.cpp

    archive/ArchiveExtractor.h
    archive/ArchiveExtractor.cpp
)

# Создаем главное приложение
qt_add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

set_target_properties(${PROJECT_NAME}
    PROPERTIES
        WIN32_EXECUTABLE TRUE
)

# Линкуем библиотеки для главного приложения
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Xml
        Qt6::AxContainer
        LibArchive::LibArchive

)

# ==========================================
# БЛОК ДЛЯ ТЕСТОВ (Опционально)
# ==========================================
# Убедитесь, что файл archive/ArchiveExtractorTest.cpp существует!
# Если тесты не нужны, просто закомментируйте или удалите блок ниже.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/archive/ArchiveExtractorTest.cpp")
    add_executable(ArchiveExtractorTest
        archive/ArchiveExtractorTest.cpp
        archive/ArchiveExtractor.cpp
    )

    target_link_libraries(ArchiveExtractorTest
        PRIVATE
            Qt::Core
            LibArchive::LibArchive
    )
endif()