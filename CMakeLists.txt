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
    src/d3d11_import_widget.cpp
    src/d3d11_import_widget.h
    src/d3d11_native_demo_window.cpp
    src/d3d11_native_demo_window.h
    src/d3d11_native_slot_pool.h
    src/d3d11_native_worker.cpp
    src/d3d11_native_worker.h
    src/gles_thread_guard.cpp
    src/gles_thread_guard.h
    src/image_effect_types.h
    src/main.cpp
    src/qt_angle_egl_tools.cpp
    src/qt_angle_egl_tools.h
)

target_link_libraries(Gles2AsyncRender PRIVATE
    Qt5::Gui
    Qt5::OpenGL
    Qt5::Widgets
)

if (WIN32)
    target_link_libraries(Gles2AsyncRender PRIVATE d3d11 dxgi d3dcompiler ole32)
endif()
