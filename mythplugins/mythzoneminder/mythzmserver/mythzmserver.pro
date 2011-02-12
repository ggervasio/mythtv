include ( ../../mythconfig.mak )
include (../../settings.pro )

TEMPLATE = app

CONFIG -= qt

TARGET = mythzmserver
target.path = $${PREFIX}/bin

INSTALLS = target

macx {
    CONFIG += qt
    QT += sql
}
LIBS = $$system(mysql_config --libs)

linux: DEFINES += linux

# Input
HEADERS += zmserver.h

SOURCES += main.cpp zmserver.cpp
