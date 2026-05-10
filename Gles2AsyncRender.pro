QT += core gui widgets opengl
QT += gui-private

CONFIG += c++17
TEMPLATE = app
TARGET = Gles2AsyncRender

INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtGui/5.15.2/QtGui

win32:LIBS += d3d11.lib dxgi.lib d3dcompiler.lib ole32.lib

SOURCES += \
    src/angle_standalone_runtime.cpp \
    src/angle_threading.cpp \
    src/d3d11_cpu_publish_bridge.cpp \
    src/d3d11_import_widget.cpp \
    src/d3d11_native_demo_window.cpp \
    src/d3d11_native_worker.cpp \
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
    src/angle_standalone_runtime.h \
    src/angle_threading.h \
    src/d3d11_cpu_publish_bridge.h \
    src/d3d11_import_widget.h \
    src/d3d11_native_demo_window.h \
    src/d3d11_native_slot_pool.h \
    src/d3d11_native_worker.h \
    src/gles2_proc_table.h \
    src/gles2_shader_utils.h \
    src/image_effect_types.h \
    src/gles_thread_guard.h \
    src/photo_editor_gles2_simulator.h \
    src/photo_editor_library_host.h \
    src/photo_editor_session.h \
    src/runtime_diagnostics.h \
    src/qt_angle_egl_tools.h
