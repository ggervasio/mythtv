include ( ../settings.pro )

QMAKE_STRIP = echo

TEMPLATE = app
CONFIG -= moc qt

QMAKE_COPY_DIR = sh ./cpsvndir
win32:QMAKE_COPY_DIR = sh ./cpsimple

html.path = $${PREFIX}/share/mythtv/html/
html.files = index.html info.html
html.files += css images js

INSTALLS += html

# Input
SOURCES += dummy.c
