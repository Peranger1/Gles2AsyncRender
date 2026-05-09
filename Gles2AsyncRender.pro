QT += core gui widgets opengl
QT += gui-private

CONFIG += c++17
TEMPLATE = app
TARGET = Gles2AsyncRender

INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtGui/5.15.2/QtGui

win32:LIBS += d3d11.lib dxgi.lib

SOURCES += \
    src/angle_threading.cpp \
    src/async_gles_widget.cpp \
    src/image_processing_pipeline.cpp \
    src/main.cpp \
    src/main_window.cpp \
    src/gles_thread_guard.cpp \
    src/shared_gl_context_handle.cpp \
    src/shared_gl_environment.cpp \
    src/shared_texture_worker.cpp

HEADERS += \
    src/angle_threading.h \
    src/async_gles_widget.h \
    src/image_effect_types.h \
    src/image_processing_pipeline.h \
    src/main_window.h \
    src/gles_thread_guard.h \
    src/shared_gl_context_handle.h \
    src/shared_gl_environment.h \
    src/shared_texture_frame_pool.h \
    src/shared_texture_worker.h
