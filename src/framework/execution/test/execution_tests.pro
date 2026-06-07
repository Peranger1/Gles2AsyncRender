TEMPLATE = app
TARGET = execution_tests
CONFIG += console c++17 testcase
CONFIG -= app_bundle
QT -= core gui

INCLUDEPATH += $$PWD/../../..

SOURCES += \
    execution_tests.cpp \
    task_scheduler_tests.cpp

HEADERS += \
    execution_test_cases.h \
    test_harness.h \
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
    ../execution_common.h \
    ../execution.h \
    ../task_scheduler.h
