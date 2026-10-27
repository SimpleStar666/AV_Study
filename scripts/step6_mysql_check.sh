#!/bin/bash
# 6.2b：WSL 重启后恢复验证（停 wvp 循环、起 mysql、查表）
systemctl stop wvp 2>/dev/null || true
systemctl disable wvp 2>/dev/null || true
echo '--- 服务状态 ---'
systemctl is-active zlm redis-server 2>&1 || true
echo '--- 启动mysql ---'
sudo service mysql start
sleep 3
echo '--- 验证wvp库表 ---'
mysql -u root -p12345678 wvp273 -e 'SHOW TABLES' 2>&1 | head -14
echo CHECK_DONE
