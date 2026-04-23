QT += core gui widgets opengl

CONFIG += c++17
TEMPLATE = app
TARGET = Gles2AsyncRender

INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtGui/5.15.2/QtGui

win32:LIBS += d3d11.lib dxgi.lib

SOURCES += \
    src/angle_threading.cpp \
    src/async_gles_widget.cpp \
    src/gles_thread_guard.cpp \
    src/main.cpp \
    src/shared_texture_worker.cpp

HEADERS += \
    src/angle_threading.h \
    src/async_gles_widget.h \
    src/gles_thread_guard.h \
    src/shared_texture_worker.h
