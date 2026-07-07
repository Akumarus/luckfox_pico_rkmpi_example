#!/bin/bash

TARGET_DIR="/oem/usr/bin"
TARGET_BIN="get_jpeg"
RKSTOP_BIN="RkLunch-stop.sh"

main() {
    echo "Запуск test_jpeg.sh"
    echo "Запуск $TARGET_DIR/$TARGET_BIN"
    adb shell ls -la /tmp | grep jp
    adb shell $TARGET_DIR/$RKSTOP_BIN
    adb shell $TARGET_DIR/$TARGET_BIN
    adb shell ls -la /tmp | grep jp
    adb pull /tmp/image.jpg
}

main "$@"