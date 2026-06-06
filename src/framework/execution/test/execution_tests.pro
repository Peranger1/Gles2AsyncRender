TEMPLATE = app
TARGET = execution_tests
CONFIG += console c++17 testcase
CONFIG -= app_bundle
QT -= core gui

INCLUDEPATH += $$PWD/../../..

SOURCES += \
    execution_tests.cpp

HEADERS += \
    ../future/async.h \
    ../future/async_future.h \
    ../future/combinators.h \
    ../future/exceptions.h \
    ../future/executor.h \
    ../future/future.h \
    ../future/future_splitter.h \
    ../future/promise.h \
    ../future/shared_state.h \
    ../future/timeout.h \
    ../future/try.h \
    ../future/unit.h \
    ../execution.h \
    ../lane/lane_exceptions.h \
    ../lane/latest_lane.h \
    ../lane/merge_lane.h \
    ../lane/serial_lane.h \
    ../task_scheduler.h
