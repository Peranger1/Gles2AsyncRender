QT += core gui widgets opengl
QT += gui-private

CONFIG += c++17
TEMPLATE = app
TARGET = Gles2AsyncRender

INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtGui/5.15.2/QtGui
INCLUDEPATH += $$PWD/src

win32:LIBS += d3d11.lib dxgi.lib d3dcompiler.lib ole32.lib

SOURCES += \
    src/adapters/photo_editor/photo_editor_cpu_preview_processor.cpp \
    src/adapters/photo_editor/photo_editor_work_processor.cpp \
    src/adapters/photo_editor/photo_editor_render_session.cpp \
    src/app/async_render_main_window.cpp \
    src/app/async_task_facade.cpp \
    src/app/photo_editor_async_render_facade.cpp \
    src/angle_threading.cpp \
    src/framework/backend/win_angle_d3d11/angle_standalone_runtime.cpp \
    src/framework/backend/win_angle_d3d11/d3d11_frame_publisher.cpp \
    src/framework/backend/render_backend_factory.cpp \
    src/framework/backend/win_angle_d3d11/win_angle_render_backend.cpp \
    src/framework/backend/win_angle_d3d11/gles2_proc_table.cpp \
    src/framework/backend/win_angle_d3d11/gles2_shader_utils.cpp \
    src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.cpp \
    src/framework/core/async_job_controller.cpp \
    src/framework/core/replace_with_latest_coalescer.cpp \
    src/framework/core/serial_conflated_async_pipeline.cpp \
    src/framework/core/serial_conflated_work_scheduler.cpp \
    src/framework/qt/qopenglwidget_frame_view.cpp \
    src/framework/qt/qt_angle_display_presenter.cpp \
    src/adapters/photo_editor/photo_editor_gles2_backend.cpp \
    src/runtime_diagnostics.cpp \
    src/main.cpp \
    src/gles_thread_guard.cpp

HEADERS += \
    src/adapters/photo_editor/photo_editor_cpu_preview_payload.h \
    src/adapters/photo_editor/photo_editor_cpu_preview_processor.h \
    src/adapters/photo_editor/photo_editor_gles2_backend.h \
    src/adapters/photo_editor/photo_editor_work_processor.h \
    src/adapters/photo_editor/photo_editor_render_payload.h \
    src/adapters/photo_editor/photo_editor_render_session.h \
    src/app/async_render_main_window.h \
    src/app/async_task_facade.h \
    src/app/photo_editor_async_render_facade.h \
    src/angle_threading.h \
    src/framework/backend/win_angle_d3d11/angle_standalone_runtime.h \
    src/framework/backend/win_angle_d3d11/d3d11_frame_publisher.h \
    src/framework/backend/win_angle_d3d11/d3d11_shared_slot_pool.h \
    src/framework/backend/render_backend_factory.h \
    src/framework/backend/win_angle_d3d11/win_angle_render_backend.h \
    src/framework/backend/platform_render_backend.h \
    src/framework/backend/win_angle_d3d11/gles2_proc_table.h \
    src/framework/backend/win_angle_d3d11/gles2_shader_utils.h \
    src/framework/backend/win_angle_d3d11/qt_angle_egl_tools.h \
    src/framework/core/async_pipeline.h \
    src/framework/core/async_job_controller.h \
    src/framework/core/frame_presenter.h \
    src/framework/core/frame_publisher.h \
    src/framework/core/gl_display_target.h \
    src/framework/core/noop_work_runtime.h \
    src/framework/core/display_target.h \
    src/framework/core/request_coalescer.h \
    src/framework/core/replace_with_latest_coalescer.h \
    src/framework/core/serial_conflated_async_pipeline.h \
    src/framework/core/serial_conflated_work_scheduler.h \
    src/framework/core/shared_frame_slot_pool.h \
    src/framework/core/work_observer.h \
    src/framework/core/work_processor.h \
    src/framework/core/work_runtime.h \
    src/framework/core/work_scheduler.h \
    src/framework/core/work_types.h \
    src/framework/qt/qopenglwidget_frame_view.h \
    src/framework/qt/qt_angle_display_presenter.h \
    src/image_effect_types.h \
    src/gles_thread_guard.h \
    src/runtime_diagnostics.h
