QT += core

TEMPLATE = app
TARGET = runtime_executor_tests
CONFIG += console c++17 testcase
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/../../..

SOURCES += \
    runtime_executor_tests.cpp \
    ../runtime_executor.cpp

HEADERS += \
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
    ../runtime_executor.h \
    ../../platform/runtime.h
