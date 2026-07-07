#!/bin/bash

PC_IP="172.32.0.100/24"
TARGET_IP="172.32.0.93"

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
    echo "Запуск test_rtsp.sh"
    set_static_ip
    
    ffplay -fflags nobuffer -window_title "RTSP Stream" \
           -vf "drawtext=text='FPS: ':x=10:y=10:fontsize=24:fontcolor=white,showinfo" \
           rtsp://172.32.0.93/live/0 2>&1 | \
    grep --line-buffered "n:" | \
    awk '
        BEGIN { 
            frame_count=0; 
            start_time=0; 
        }
        {
            # Извлекаем номер кадра и время
            match($0, /n: ([0-9]+)/, n);
            match($0, /pts_time:([0-9.]+)/, t);
            
            if (n[1] != "" && t[1] != "") {
                frame_count++;
                current_time = t[1];
                
                if (frame_count == 1) {
                    start_time = current_time;
                }
                
                # Каждые 30 кадров вычисляем FPS
                if (frame_count % 30 == 0 && frame_count > 1) {
                    elapsed = current_time - start_time;
                    if (elapsed > 0) {
                        fps = frame_count / elapsed;
                        printf "\r\033[KFPS: %.2f (кадров: %d, время: %.2f с)   ", fps, frame_count, elapsed;
                        fflush();
                    }
                }
            }
        }
    '
}

main "$@"