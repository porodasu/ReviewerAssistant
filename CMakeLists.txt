cmake_minimum_required(VERSION 3.16)
project(ReviewerAssistant LANGUAGES CXX)

include(qt.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR}
    COMPONENTS
        Core
        Gui
        Widgets
)

find_package(LibArchive REQUIRED)
# find_package(libzip CONFIG REQUIRED) - УДАЛЕНО

qt_standard_project_setup()

set(PROJECT_SOURCES
    main.cpp
    mainwindow.ui
    mainwindow.h
    mainwindow.cpp

    archive/ArchiveExtractor.h
    archive/ArchiveExtractor.cpp
)

qt_add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

add_executable(ArchiveExtractorTest
    archive/ArchiveExtractorTest.cpp
    archive/ArchiveExtractor.cpp
)

target_link_libraries(ArchiveExtractorTest
    PRIVATE
        LibArchive::LibArchive
        # libzip::zip - УДАЛЕНО
)

set_target_properties(${PROJECT_NAME}
    PROPERTIES
        WIN32_EXECUTABLE TRUE
)

target_link_libraries(${PROJECT_NAME}
    PUBLIC
        Qt::Core
        Qt::Gui
        Qt::Widgets
        LibArchive::LibArchive
        # libzip::zip - УДАЛЕНО
)