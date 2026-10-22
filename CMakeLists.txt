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
)


find_package(LibArchive REQUIRED)

qt_standard_project_setup()

set(PROJECT_SOURCES
    main.cpp
    mainwindow.ui
    mainwindow.h
    mainwindow.cpp

    DocumentReader.h
    DocumentReader.cpp

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
        Qt6::AxContainer
        LibArchive::LibArchive
)

