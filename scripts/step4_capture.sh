#!/bin/bash
# 第4步：抓包分析 RTSP 信令面 + RTP 数据面
# 用法：bash ~/step4_capture.sh
cd ~ || exit 1

# 0. 确保 mediamtx 在跑
if ! pgrep mediamtx >/dev/null; then
  nohup ./mediamtx >~/mediamtx.log 2>&1 &
  sleep 1
fi

CAP=~/proto.pcapng
rm -f "$CAP"

echo "==== [1/4] 开始抓包（lo 回环：TCP 8554 + UDP 全量）===="
sudo tshark -i lo -f "tcp port 8554 or udp" -w "$CAP" >/dev/null 2>&1 &
CAP_PID=$!
sleep 1

echo "==== [2/4] 推流 testsrc → rtsp://localhost:8554/proto（8秒）===="
ffmpeg -re -stream_loop -1 -f lavfi -i testsrc=size=640x360:rate=15 \
  -c:v libx264 -preset veryfast -tune zerolatency -b:v 1M -g 50 -pix_fmt yuv420p \
  -rtsp_transport udp -f rtsp rtsp://localhost:8554/proto >/tmp/push4.log 2>&1 &
PUSH=$!
sleep 8
kill "$PUSH" 2>/dev/null
sleep 1
sudo kill "$CAP_PID" 2>/dev/null

echo ""
echo "==== [3/4] 信令面：RTSP 会话（看四次握手）===="
sudo tshark -r "$CAP" -Y "rtsp.request or rtsp.response" -O rtsp 2>/dev/null | head -70

echo ""
echo "==== [4/4] 数据面：RTP 包（前5个 + 首个包字节 + 统计）===="
sudo tshark -r "$CAP" -Y "rtp" -c 5 -T fields \
  -e frame.number -e frame.len -e ip.src -e ip.dst \
  -e udp.srcport -e udp.dstport -e rtp.payload_type -e rtp.seq -e rtp.timestamp -e rtp.ssrc 2>/dev/null
echo "--- 首个 RTP 包原始字节 ---"
sudo tshark -r "$CAP" -Y "rtp" -c 1 -x 2>/dev/null | head -14
echo "--- 统计 ---"
sudo tshark -r "$CAP" -Y "rtp" -T fields -e frame.len 2>/dev/null | awk '{s+=$1; n++} END{printf "RTP 包总数=%d 总字节=%d 平均=%d B/包\n", n, s, n?int(s/n):0}'
echo "CAPTURE_DONE  ($CAP)"
