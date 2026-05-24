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
    src/app/photo_editor_app_session.cpp
    src/app/photo_editor_app_session.h
    src/app/photo_editor_handle_actor.cpp
    src/app/photo_editor_handle_actor.h
    src/app/photo_editor_runtime_service.cpp
    src/app/photo_editor_runtime_service.h
    src/photo_editor/photo_editor_result_types.h
    src/app/texture_present_widget.cpp
    src/app/texture_present_widget.h
    src/photo_editor/photo_editor_gles2_backend.cpp
    src/photo_editor/photo_editor_gles2_backend.h
    src/app/async_render_main_window.cpp
    src/app/async_render_main_window.h
    src/framework/backend/win_angle_d3d11/gles2_proc_table.cpp
    src/framework/backend/win_angle_d3d11/gles2_proc_table.h
    src/framework/backend/win_angle_d3d11/gles2_shader_utils.cpp
    src/framework/backend/win_angle_d3d11/gles2_shader_utils.h
    src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.cpp
    src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.h
    src/framework/execution/async_lane.h
    src/framework/execution/execution_common.h
    src/framework/execution/qt_runtime_host.cpp
    src/framework/execution/qt_runtime_host.h
    src/framework/execution/runtime_executor.cpp
    src/framework/execution/runtime_executor.h
    src/framework/execution/runtime_host.h
    src/framework/execution/runtime_scope.cpp
    src/framework/execution/runtime_scope.h
    src/framework/execution/sync_lane.h
    src/framework/platform/platform_backend.h
    src/framework/platform/platform_backend_factory.cpp
    src/framework/platform/platform_backend_factory.h
    src/framework/platform/presentation_context.h
    src/framework/platform/presentation_events.h
    src/framework/platform/reader.h
    src/framework/platform/runtime.h
    src/framework/platform/gl_types.h
    src/framework/platform/texture_types.h
    src/framework/platform/writer.h
    src/image_effect_types.h
    src/main.cpp
    src/runtime_diagnostics.cpp
    src/runtime_diagnostics.h
)

if (WIN32)
    target_sources(Gles2AsyncRender PRIVATE
        src/framework/platform/win_angle_d3d11/d3d11_shared_texture_slots.h
        src/framework/platform/win_angle_d3d11/win_angle_platform_backend.cpp
        src/framework/platform/win_angle_d3d11/win_angle_platform_backend.h
        src/framework/platform/win_angle_d3d11/win_angle_runtime.cpp
        src/framework/platform/win_angle_d3d11/win_angle_runtime.h
        src/framework/platform/win_angle_d3d11/win_angle_texture_reader.cpp
        src/framework/platform/win_angle_d3d11/win_angle_texture_reader.h
        src/framework/platform/win_angle_d3d11/win_angle_texture_writer.cpp
        src/framework/platform/win_angle_d3d11/win_angle_texture_writer.h
    )
elseif(APPLE)
    target_sources(Gles2AsyncRender PRIVATE
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_platform_backend.cpp
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_platform_backend.h
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_runtime.cpp
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_runtime.h
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_shared_state.h
        src/framework/platform/mac_cocoa_gl/mac_iosurface_texture_slots.h
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_texture_reader.cpp
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_texture_reader.h
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_texture_writer.cpp
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_texture_writer.h
    )
endif()

target_include_directories(Gles2AsyncRender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(Gles2AsyncRender PRIVATE
    Qt5::Gui
    Qt5::GuiPrivate
    Qt5::OpenGL
    Qt5::Widgets
)

if (WIN32)
    target_link_libraries(Gles2AsyncRender PRIVATE d3d11 dxgi d3dcompiler ole32)
endif()

if (APPLE)
    target_link_libraries(Gles2AsyncRender PRIVATE
        "-framework OpenGL"
        "-framework IOSurface"
        "-framework CoreFoundation"
    )
endif()
