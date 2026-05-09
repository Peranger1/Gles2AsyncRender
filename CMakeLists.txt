cmake_minimum_required(VERSION 3.16)

project(Gles2AsyncRender LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt5 REQUIRED COMPONENTS Widgets Gui OpenGL)

add_executable(Gles2AsyncRender
    src/angle_threading.cpp
    src/angle_threading.h
    src/async_gles_widget.cpp
    src/async_gles_widget.h
    src/main.cpp
    src/gles_thread_guard.cpp
    src/gles_thread_guard.h
    src/shared_texture_worker.cpp
    src/shared_texture_worker.h
)

target_link_libraries(Gles2AsyncRender PRIVATE
    Qt5::Gui
    Qt5::OpenGL
    Qt5::Widgets
)

if (WIN32)
    target_link_libraries(Gles2AsyncRender PRIVATE d3d11 dxgi)
endif()
