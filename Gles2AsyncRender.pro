QT += core gui widgets opengl
QT += gui-private

CONFIG += c++17
TEMPLATE = app
TARGET = Gles2AsyncRender

INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtGui/5.15.2/QtGui

win32:LIBS += d3d11.lib dxgi.lib d3dcompiler.lib ole32.lib

SOURCES += \
    src/angle_threading.cpp \
    src/d3d11_import_widget.cpp \
    src/d3d11_native_demo_window.cpp \
    src/d3d11_native_worker.cpp \
    src/main.cpp \
    src/gles_thread_guard.cpp \
    src/qt_angle_egl_tools.cpp

HEADERS += \
    src/angle_threading.h \
    src/d3d11_import_widget.h \
    src/d3d11_native_demo_window.h \
    src/d3d11_native_slot_pool.h \
    src/d3d11_native_worker.h \
    src/image_effect_types.h \
    src/gles_thread_guard.h \
    src/qt_angle_egl_tools.h
