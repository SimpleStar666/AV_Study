#!/bin/bash
# 第5步：音画同步——视频(testsrc) + 音频(sine 440Hz) 双轨 RTSP 推流
# 用法：bash ~/step5_push.sh
ffmpeg -re -stream_loop -1 \
  -f lavfi -i "testsrc=size=640x360:rate=15" \
  -f lavfi -i "sine=frequency=440:sample_rate=8000" \
  -map 0:v -map 1:a \
  -c:v libx264 -preset veryfast -tune zerolatency -b:v 1M -g 50 -pix_fmt yuv420p \
  -c:a pcm_mulaw -ar 8000 -ac 1 \
  -f rtsp rtsp://localhost:8554/live
