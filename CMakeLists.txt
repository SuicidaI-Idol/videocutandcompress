cmake_minimum_required(VERSION 3.21)

project(VideoCutCompressQtCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

qt_add_executable(videocut
    src/main.cpp
    src/resources.qrc
)

target_link_libraries(videocut PRIVATE
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
)

if (WIN32)
    set_target_properties(videocut PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(videocut PRIVATE src/app.rc)
endif()
