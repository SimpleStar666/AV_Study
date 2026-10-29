#!/bin/bash
# ============================================================
# 6.3 GB28181 全链演示（用户本机一键跑）
# 跑完会打印拉流地址，然后 ffplay 自动弹窗出画面
# 用法：bash /home/ts/step6_demo.sh
# ============================================================
nohup bash -c 'while true; do curl -s -o /dev/null http://127.0.0.1:18080/ 2>/dev/null; sleep 2; done' >/dev/null 2>&1 &
BASE='http://127.0.0.1:18080'
S='v5ZdYOptO4cklpY48S3mviCT5p8fQDYC'
echo '== [1/5] 清理旧模拟器 =='
pkill -9 -f 'python3 -u /home/ts/gb28181_device' 2>/dev/null
for i in $(seq 1 10); do
  if ! ss -ulnp 2>/dev/null | grep -q ':5061 '; then break; fi
  sleep 1
done
sleep 1
echo '== [2/5] 启动模拟器（注册+目录上报+等待点播）=='
nohup python3 -u /home/ts/gb28181_device.py > /home/ts/device_demo.log 2>&1 &
for i in $(seq 1 25); do grep -q '注册成功' /home/ts/device_demo.log && break; sleep 1; done
if ! grep -q '注册成功' /home/ts/device_demo.log; then
  echo '[错误] 模拟器注册失败，日志末尾：'
  tail -5 /home/ts/device_demo.log
  exit 1
fi
grep -E '注册成功|上报目录' /home/ts/device_demo.log
sleep 1
echo '== [3/5] 登录 wvp =='
TOKEN=$(curl -s "$BASE/api/user/login?username=admin&password=21232f297a57a5a743894a0e4a801fc3" | sed 's/.*"accessToken":"\([^"]*\)".*/\1/')
echo "token 长度: ${#TOKEN}"
echo '== [4/5] 点播（INVITE→200OK→推流→ZLM收流）=='
RES=$(curl -s "$BASE/api/common/channel/play?deviceId=34020000001320000001&channelId=1" -H "access-token: $TOKEN")
echo "$RES" | head -c 500
echo
STREAM=$(echo "$RES" | sed 's/.*"stream":"\([^"]*\)".*/\1/')
echo '== [5/5] ffplay 拉流（RTSP）=='
RTSP="rtsp://127.0.0.1:554/rtp/$STREAM"
echo "拉流地址: $RTSP"
sleep 2
ffplay -autoexit -t 20 -rtsp_transport tcp -window_title 'GB28181-DEMO' "$RTSP"
echo DEMO_DONE
