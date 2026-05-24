QT += core gui widgets opengl
QT += gui-private

CONFIG += c++17
TEMPLATE = app
TARGET = Gles2AsyncRender

INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtGui/5.15.2/QtGui
INCLUDEPATH += $$PWD/src

win32:LIBS += d3d11.lib dxgi.lib d3dcompiler.lib ole32.lib

SOURCES += \
    src/app/photo_editor_app_session.cpp \
    src/app/photo_editor_handle_actor.cpp \
    src/app/photo_editor_runtime_service.cpp \
    src/app/texture_present_widget.cpp \
    src/app/async_render_main_window.cpp \
    src/framework/backend/win_angle_d3d11/gles2_proc_table.cpp \
    src/framework/backend/win_angle_d3d11/gles2_shader_utils.cpp \
    src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.cpp \
    src/framework/execution/qt_runtime_host.cpp \
    src/framework/execution/runtime_executor.cpp \
    src/framework/execution/runtime_scope.cpp \
    src/photo_editor/photo_editor_gles2_backend.cpp \
    src/runtime_diagnostics.cpp \
    src/main.cpp

HEADERS += \
    src/app/photo_editor_app_session.h \
    src/app/photo_editor_handle_actor.h \
    src/app/photo_editor_runtime_service.h \
    src/photo_editor/photo_editor_result_types.h \
    src/app/texture_present_widget.h \
    src/photo_editor/photo_editor_gles2_backend.h \
    src/app/async_render_main_window.h \
    src/framework/backend/win_angle_d3d11/gles2_proc_table.h \
    src/framework/backend/win_angle_d3d11/gles2_shader_utils.h \
    src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.h \
    src/framework/execution/async_lane.h \
    src/framework/execution/execution_common.h \
    src/framework/execution/qt_runtime_host.h \
    src/framework/execution/runtime_executor.h \
    src/framework/execution/runtime_host.h \
    src/framework/execution/runtime_scope.h \
    src/framework/execution/sync_lane.h \
    src/framework/platform/gl_types.h \
    src/framework/platform/platform_backend.h \
    src/framework/platform/platform_backend_factory.h \
    src/framework/platform/presentation_context.h \
    src/framework/platform/presentation_events.h \
    src/framework/platform/reader.h \
    src/framework/platform/runtime.h \
    src/framework/platform/texture_types.h \
    src/framework/platform/writer.h \
    src/image_effect_types.h \
    src/runtime_diagnostics.h

SOURCES += \
    src/framework/platform/platform_backend_factory.cpp

win32 {
    SOURCES += \
        src/framework/platform/win_angle_d3d11/win_angle_platform_backend.cpp \
        src/framework/platform/win_angle_d3d11/win_angle_runtime.cpp \
        src/framework/platform/win_angle_d3d11/win_angle_texture_reader.cpp \
        src/framework/platform/win_angle_d3d11/win_angle_texture_writer.cpp

    HEADERS += \
        src/framework/platform/win_angle_d3d11/d3d11_shared_texture_slots.h \
        src/framework/platform/win_angle_d3d11/win_angle_platform_backend.h \
        src/framework/platform/win_angle_d3d11/win_angle_runtime.h \
        src/framework/platform/win_angle_d3d11/win_angle_texture_reader.h \
        src/framework/platform/win_angle_d3d11/win_angle_texture_writer.h
}

macx {
    SOURCES += \
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_platform_backend.cpp \
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_runtime.cpp \
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_texture_reader.cpp \
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_texture_writer.cpp

    HEADERS += \
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_platform_backend.h \
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_runtime.h \
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_shared_state.h \
        src/framework/platform/mac_cocoa_gl/mac_iosurface_texture_slots.h \
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_texture_reader.h \
        src/framework/platform/mac_cocoa_gl/mac_cocoa_gl_texture_writer.h
}

macx:LIBS += -framework OpenGL -framework IOSurface -framework CoreFoundation
