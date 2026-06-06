TEMPLATE = app
TARGET = async_future_tests
CONFIG += console c++17 testcase
CONFIG -= app_bundle
QT -= core gui

INCLUDEPATH += $$PWD/../../../..

SOURCES += \
    future_tests.cpp

HEADERS += \
    ../async.h \
    ../async_future.h \
    ../combinators.h \
    ../exceptions.h \
    ../executor.h \
    ../future.h \
    ../future_splitter.h \
    ../promise.h \
    ../shared_state.h \
    ../timeout.h \
    ../try.h \
    ../unit.h
