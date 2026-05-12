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
    src/adapters/photo_editor/photo_editor_render_payload.h
    src/adapters/photo_editor/photo_editor_render_session.cpp
    src/adapters/photo_editor/photo_editor_render_session.h
    src/app/photo_editor_async_render_facade.cpp
    src/app/photo_editor_async_render_facade.h
    src/angle_threading.cpp
    src/angle_threading.h
    src/app/async_render_main_window.cpp
    src/app/async_render_main_window.h
    src/framework/backend/win_angle_d3d11/angle_standalone_runtime.cpp
    src/framework/backend/win_angle_d3d11/angle_standalone_runtime.h
    src/framework/backend/win_angle_d3d11/d3d11_frame_publisher.cpp
    src/framework/backend/win_angle_d3d11/d3d11_frame_publisher.h
    src/framework/backend/win_angle_d3d11/d3d11_shared_slot_pool.h
    src/framework/backend/win_angle_d3d11/gles2_proc_table.cpp
    src/framework/backend/win_angle_d3d11/gles2_proc_table.h
    src/framework/backend/win_angle_d3d11/gles2_shader_utils.cpp
    src/framework/backend/win_angle_d3d11/gles2_shader_utils.h
    src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.cpp
    src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.h
    src/framework/core/async_render_executor.cpp
    src/framework/core/async_render_executor.h
    src/framework/core/async_render_session.h
    src/framework/core/async_render_types.h
    src/framework/core/async_render_worker.cpp
    src/framework/core/async_render_worker.h
    src/framework/core/display_presenter.h
    src/framework/core/frame_publisher.h
    src/framework/core/render_runtime.h
    src/framework/core/shared_frame_slot_pool.h
    src/framework/qt/qopenglwidget_frame_view.cpp
    src/framework/qt/qopenglwidget_frame_view.h
    src/framework/qt/qt_angle_display_presenter.cpp
    src/framework/qt/qt_angle_display_presenter.h
    src/framework/qt/qopenglwidget_display_host.h
    src/gles_thread_guard.cpp
    src/gles_thread_guard.h
    src/image_effect_types.h
    src/main.cpp
    src/photo_editor_gles2_simulator.cpp
    src/photo_editor_gles2_simulator.h
    src/photo_editor_library_host.cpp
    src/photo_editor_library_host.h
    src/photo_editor_session.cpp
    src/photo_editor_session.h
    src/runtime_diagnostics.cpp
    src/runtime_diagnostics.h
)

target_include_directories(Gles2AsyncRender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(Gles2AsyncRender PRIVATE
    Qt5::Gui
    Qt5::OpenGL
    Qt5::Widgets
)

if (WIN32)
    target_link_libraries(Gles2AsyncRender PRIVATE d3d11 dxgi d3dcompiler ole32)
endif()
