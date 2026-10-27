#!/bin/bash
# 第4步：WSL 推流（配合 Windows 侧 step4_capture_win.ps1 抓包）
# 用法：bash ~/step4_push.sh （Ctrl+C 停止）
ffmpeg -re -stream_loop -1 -f lavfi -i testsrc=size=640x360:rate=15 \
  -c:v libx264 -preset veryfast -tune zerolatency -b:v 1M -g 50 -pix_fmt yuv420p \
  -rtsp_transport udp -f rtsp rtsp://localhost:8554/live
