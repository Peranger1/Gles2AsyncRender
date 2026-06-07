TEMPLATE = app
TARGET = async_future_tests
CONFIG += console c++17 testcase
CONFIG -= app_bundle
QT -= core gui

INCLUDEPATH += $$PWD/../../../..

SOURCES += \
    future_combinator_tests.cpp \
    future_continuation_tests.cpp \
    future_executor_tests.cpp \
    future_lifecycle_tests.cpp \
    future_splitter_tests.cpp \
    future_timeout_tests.cpp \
    future_tests.cpp

HEADERS += \
    future_test_cases.h \
    ../../test/test_harness.h \
    ../async.h \
    ../async_future.h \
    ../combinators.h \
    ../exceptions.h \
    ../executor.h \
    ../future.h \
    ../future_splitter.h \
    ../promise.h \
    ../shared_state.h \
    ../timer_executor.h \
    ../timeout.h \
    ../try.h \
    ../unit.h
