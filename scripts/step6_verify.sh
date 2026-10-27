#!/bin/bash
# 6.2b：wvp 启动结果验证（输出到文件）
sleep 35
{
echo '=== 服务状态 ==='
systemctl is-active wvp zlm redis-server mysql
echo '=== 关键端口 ==='
ss -tlnp 2>/dev/null | grep -E ':(5060|18080|8090|11000|554|3306)' | grep -oE ':(5060|18080|8090|11000|554|3306) .*(MediaServer|java|mysqld|users:)?.*' | head -8
echo '=== wvp 启动关键日志 ==='
journalctl -u wvp --no-pager -n 300 2>&1 | grep -iE 'Started VManageBootstrap|SIP监听|ZLM-连接成功|媒体节点.*上线|Application run failed|Unknown column|not found|ERROR ---' | tail -8
echo '=== 列确认 ==='
mysql -u root -p12345678 wvp273 -e "DESCRIBE wvp_device" 2>/dev/null | grep -E 'register_time|keepalive_time'
echo VERIFY_DONE
} > /home/ts/wvp_verify.txt 2>&1
echo SAVED
