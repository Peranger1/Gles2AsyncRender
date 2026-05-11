QT += core gui widgets opengl
QT += gui-private

CONFIG += c++17
TEMPLATE = app
TARGET = Gles2AsyncRender

INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtGui/5.15.2/QtGui

win32:LIBS += d3d11.lib dxgi.lib d3dcompiler.lib ole32.lib

SOURCES += \
    src/adapters/photo_editor/photo_editor_render_session.cpp \
    src/angle_standalone_runtime.cpp \
    src/angle_threading.cpp \
    src/d3d11_import_widget.cpp \
    src/d3d11_native_demo_window.cpp \
    src/d3d11_native_worker.cpp \
    src/d3d11_standalone_publish_bridge.cpp \
    src/framework/core/async_render_executor.cpp \
    src/framework/qt/qt_angle_display_presenter.cpp \
    src/gles2_proc_table.cpp \
    src/gles2_shader_utils.cpp \
    src/photo_editor_gles2_simulator.cpp \
    src/photo_editor_library_host.cpp \
    src/photo_editor_session.cpp \
    src/runtime_diagnostics.cpp \
    src/main.cpp \
    src/gles_thread_guard.cpp \
    src/qt_angle_egl_tools.cpp

HEADERS += \
    src/adapters/photo_editor/photo_editor_render_payload.h \
    src/adapters/photo_editor/photo_editor_render_session.h \
    src/angle_standalone_runtime.h \
    src/angle_threading.h \
    src/d3d11_import_widget.h \
    src/d3d11_native_demo_window.h \
    src/d3d11_native_slot_pool.h \
    src/d3d11_native_worker.h \
    src/d3d11_standalone_publish_bridge.h \
    src/framework/core/async_render_executor.h \
    src/framework/core/async_render_session.h \
    src/framework/core/async_render_types.h \
    src/framework/core/display_presenter.h \
    src/framework/core/frame_publisher.h \
    src/framework/core/render_runtime.h \
    src/framework/core/shared_frame_slot_pool.h \
    src/framework/qt/qt_angle_display_presenter.h \
    src/framework/qt/qopenglwidget_display_host.h \
    src/gles2_proc_table.h \
    src/gles2_shader_utils.h \
    src/image_effect_types.h \
    src/gles_thread_guard.h \
    src/photo_editor_gles2_simulator.h \
    src/photo_editor_library_host.h \
    src/photo_editor_session.h \
    src/runtime_diagnostics.h \
    src/qt_angle_egl_tools.h
