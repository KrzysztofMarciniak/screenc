#!/bin/sh

OS="$(uname -s)"
CC=cc
SRC="main.c"
TARGET="screenc"

CFLAGS="-std=c99 -Wall -O2"
LDFLAGS="-lpng -lX11"

if [ "$OS" = "OpenBSD" ]; then
    CFLAGS="$CFLAGS -I/usr/X11R6/include -I/usr/local/include"
    LDFLAGS="-L/usr/X11R6/lib -L/usr/local/lib -lpng -lX11"
fi

usage() {
    echo "Usage: $0 [build|clean|install]"
    exit 1
}

case "$1" in
    build|"")
        echo "Building for $OS..."
        echo "CC flags: $CFLAGS"
        echo "LD flags: $LDFLAGS"
        $CC $CFLAGS -o $TARGET $SRC $LDFLAGS
        if [ $? -eq 0 ]; then
            echo "Build successful: ./$TARGET"
        else
            echo "Build failed"
            exit 1
        fi
        ;;
    clean)
        echo "Cleaning..."
        rm -f $TARGET
        ;;
    install)
        PREFIX="/usr/local/bin"
        if [ ! -x $TARGET ]; then
            echo "Error: $TARGET not built yet. Run '$0 build' first."
            exit 1
        fi
        echo "Installing $TARGET to $PREFIX..."
        install -m 755 $TARGET $PREFIX
        ;;
    *)
        usage
        ;;
esac
