#!/bin/bash
# ============================================================
# step6_cam.sh — GB28181 实时流接入验证（-live FIFO 模式）
# 用法: bash step6_cam.sh [x86|aarch64] [testsrc|webcam]
#   x86     : 原生二进制（调试用）
#   aarch64 : qemu-aarch64 交叉编译二进制（默认）
#   testsrc : ffmpeg 测试源（验证 live 链路逻辑，默认）
#   webcam  : 真实摄像头 /dev/video0 → libx264 → FIFO
# ============================================================
MODE=${1:-aarch64}
SRC=${2:-testsrc}
FIFO=/tmp/cam.h264.fifo
LOG=/tmp/step6_cam.log
DUMP=/tmp/cam_frame.jpg
STREAM=34020000001320000001_34020000001320000001

if [ "$MODE" = "x86" ]; then
  BIN="/home/ts/gb28181_device_x86"
else
  BIN="qemu-aarch64 /home/ts/gb28181_device_aarch64"
fi

FFLOG=/tmp/ffmpeg_cam.log
WVP_HDR=/tmp/wvp_hdr.txt

echo "== [1/6] 清场 =="
pkill -9 -x qemu-aarch64 2>/dev/null
pkill -9 -x gb28181_device_x86 2>/dev/null
pkill -9 -x gb28181_device_aarch64 2>/dev/null
pkill -9 -x ffmpeg 2>/dev/null
rm -f "$FIFO" "$LOG" "$DUMP" "$FFLOG" "$WVP_HDR" /tmp/cam_frame2.jpg /tmp/wc_a.jpg /tmp/wc_b.jpg
mkfifo "$FIFO"

echo "== [2/6] 启动编码源（$SRC）→ FIFO =="
if [ "$SRC" = "webcam" ]; then
  ffmpeg -y -v error -f v4l2 -input_format mjpeg -video_size 640x480 -framerate 15 -i /dev/video0 \
    -c:v libx264 -preset ultrafast -tune zerolatency -b:v 800k -g 30 -threads 1 \
    -x264-params repeat-headers=1:slices=1 -f h264 "$FIFO" > "$FFLOG" 2>&1 &
else
  ffmpeg -y -v error -f lavfi -i "testsrc2=size=320x180:rate=15" -c:v libx264 -preset ultrafast \
    -tune zerolatency -b:v 500k -g 30 -threads 1 -x264-params repeat-headers=1:slices=1 \
    -f h264 "$FIFO" > "$FFLOG" 2>&1 &
fi
FFPID=$!
echo "  ffmpeg pid=$FFPID"

echo "== [3/6] 启动模拟器（$MODE, -live）=="
$BIN -live "$FIFO" > "$LOG" 2>&1 &
SIMPID=$!
echo "  模拟器 pid=$SIMPID"
sleep 5

echo "== [4/6] 登录 wvp 并点播 =="
echo "  等待 wvp 就绪（HTTP 200）..."
for i in $(seq 1 30); do
  CODE=$(curl -s -o /dev/null -w "%{http_code}" \
    "http://127.0.0.1:18080/api/user/login?username=admin&password=21232f297a57a5a743894a0e4a801fc3" 2>/dev/null)
  [ "$CODE" = "200" ] && { echo "  wvp 就绪（${i}0s 内）"; break; }
  sleep 10
done
TOK=$(curl -s -D "$WVP_HDR" -o /dev/null \
  "http://127.0.0.1:18080/api/user/login?username=admin&password=21232f297a57a5a743894a0e4a801fc3" \
  && grep -i "^access-token:" "$WVP_HDR" | head -1 | awk '{print $2}' | tr -d '\r')
echo "  token 长度: ${#TOK}"
curl -s -H "access-token: $TOK" \
  "http://127.0.0.1:18080/api/common/channel/play?deviceId=34020000001320000001&channelId=1" | head -c 300
echo
sleep 6

echo "== [5/6] FLV 拉流抓帧（两帧验证画面在动）=="
ffmpeg -y -v error -t 5 -rtsp_transport tcp -i "rtsp://127.0.0.1:554/rtp/$STREAM" -frames:v 1 "$DUMP" 2>&1 | tail -2
ls -la "$DUMP" 2>/dev/null || echo "!! 抓帧失败"
sleep 2
ffmpeg -y -v error -rtsp_transport tcp -i "rtsp://127.0.0.1:554/rtp/$STREAM" -frames:v 1 /tmp/cam_frame2.jpg 2>/dev/null
echo "--- 两帧 md5（不同 = 实时画面在动） ---"
md5sum "$DUMP" /tmp/cam_frame2.jpg 2>/dev/null
# 立即拷到 Windows 侧（WSL 实例可能随后回收 /tmp）
cp "$DUMP" /mnt/c/Users/小陶/Doubao/chats/2026-10-26/new-chat-3/scripts/webcam_frame_a.jpg 2>/dev/null
cp /tmp/cam_frame2.jpg /mnt/c/Users/小陶/Doubao/chats/2026-10-26/new-chat-3/scripts/webcam_frame_b.jpg 2>/dev/null
ls -la /mnt/c/Users/小陶/Doubao/chats/2026-10-26/new-chat-3/scripts/webcam_frame_*.jpg 2>/dev/null

echo "== [6/6] 证据汇总 =="
echo "--- 编码源日志 ---"
tail -3 "$FFLOG" 2>/dev/null
echo "--- 模拟器日志 ---"
grep -E "注册|INVITE|FIFO|已推|推流|收流|ERROR|error" "$LOG" | tail -12

kill $SIMPID $FFPID 2>/dev/null
echo "DEMO_DONE"
