QT += core gui widgets opengl
QT += gui-private

CONFIG += c++17
TEMPLATE = app
TARGET = Gles2AsyncRender

INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtGui/5.15.2/QtGui
INCLUDEPATH += $$PWD/src

win32:LIBS += d3d11.lib dxgi.lib d3dcompiler.lib ole32.lib

SOURCES += \
    src/adapters/photo_editor/photo_editor_work_processor.cpp \
    src/adapters/photo_editor/photo_editor_render_session.cpp \
    src/app/async_render_main_window.cpp \
    src/app/photo_editor_async_render_facade.cpp \
    src/angle_threading.cpp \
    src/framework/backend/win_angle_d3d11/angle_standalone_runtime.cpp \
    src/framework/backend/win_angle_d3d11/d3d11_frame_publisher.cpp \
    src/framework/backend/win_angle_d3d11/gles2_proc_table.cpp \
    src/framework/backend/win_angle_d3d11/gles2_shader_utils.cpp \
    src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.cpp \
    src/framework/core/async_pipeline.cpp \
    src/framework/core/latest_only_async_pipeline.cpp \
    src/framework/core/latest_only_work_scheduler.cpp \
    src/framework/core/simple_artifact_builder.cpp \
    src/framework/qt/qopenglwidget_frame_view.cpp \
    src/framework/qt/qt_angle_display_presenter.cpp \
    src/photo_editor_gles2_simulator.cpp \
    src/photo_editor_library_host.cpp \
    src/photo_editor_session.cpp \
    src/runtime_diagnostics.cpp \
    src/main.cpp \
    src/gles_thread_guard.cpp

HEADERS += \
    src/adapters/photo_editor/photo_editor_work_processor.h \
    src/adapters/photo_editor/photo_editor_render_payload.h \
    src/adapters/photo_editor/photo_editor_render_session.h \
    src/app/async_render_main_window.h \
    src/app/photo_editor_async_render_facade.h \
    src/angle_threading.h \
    src/framework/backend/win_angle_d3d11/angle_standalone_runtime.h \
    src/framework/backend/win_angle_d3d11/d3d11_frame_publisher.h \
    src/framework/backend/win_angle_d3d11/d3d11_shared_slot_pool.h \
    src/framework/backend/win_angle_d3d11/gles2_proc_table.h \
    src/framework/backend/win_angle_d3d11/gles2_shader_utils.h \
    src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.h \
    src/framework/core/async_pipeline.h \
    src/framework/core/artifact_builder.h \
    src/framework/core/artifact_presenter.h \
    src/framework/core/artifact_publisher.h \
    src/framework/core/gl_presentation_target.h \
    src/framework/core/latest_only_async_pipeline.h \
    src/framework/core/latest_only_work_scheduler.h \
    src/framework/core/presentation_target.h \
    src/framework/core/shared_frame_slot_pool.h \
    src/framework/core/simple_artifact_builder.h \
    src/framework/core/work_observer.h \
    src/framework/core/work_processor.h \
    src/framework/core/work_runtime.h \
    src/framework/core/work_scheduler.h \
    src/framework/core/work_types.h \
    src/framework/qt/qopenglwidget_frame_view.h \
    src/framework/qt/qt_angle_display_presenter.h \
    src/framework/qt/qopenglwidget_display_host.h \
    src/image_effect_types.h \
    src/gles_thread_guard.h \
    src/photo_editor_gles2_simulator.h \
    src/photo_editor_library_host.h \
    src/photo_editor_session.h \
    src/runtime_diagnostics.h
