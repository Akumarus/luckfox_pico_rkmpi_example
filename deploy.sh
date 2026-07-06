#!/bin/bash
PC_IP="172.32.0.100/24"
BOARD_IP="172.32.0.93"
TARGET_DIR="/oem/usr/bin"

set_static_ip() {
    INTERFACE=$(ip a | grep -E "^[0-9]+: enx" | grep -v "lo:" | awk -F': ' '{print $2}' | head -1)
    echo "RDIS interface: $INTERFACE"

    if [ -n "$INTERFACE" ]; then
        if ip a show "$INTERFACE" | grep -q "$PC_IP"; then
            echo "IP $PC_IP Уже настроен на $INTERFACE"
        else
            echo "Настраиваем IP $PC_IP на интерфейс $INTERFACE"
            sudo ip link set "$INTERFACE" up
            sudo ip addr add 172.32.0.100/24 dev "$INTERFACE"
            echo "Luckfox Pico настроен на интерфейс $INTERFACE с IP 172.32.0.100"
        fi
    else
        echo "Luckfox Pico не найдена"
    fi
}

main() {
    local FILE_TO_DEPLOY="./build/luckfox_pico_jpeg"

    echo "Запуск deploy.sh"
    set_static_ip
    
    echo "Board IP = $BOARD_IP"
    
    # Копируем через ADB
    echo "Copying to /oem/usr/bin/..."
    adb push "$FILE_TO_DEPLOY" "$TARGET_DIR/"
    adb shell "chmod +x $TARGET_DIR/$(basename "$FILE_TO_DEPLOY")"
    adb shell "rm -f /tmp/*.jpeg 2>/dev/null"
    adb shell "rm -f /tmp/*.jpg 2>/dev/null"
    echo "Deployed to $TARGET_DIR/$(basename "$FILE_TO_DEPLOY")"

}

main "$@"