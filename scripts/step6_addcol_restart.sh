#!/bin/bash
# 6.2b：补齐 wvp_device 缺失列（register_time/keepalive_time）并重启 wvp
mysql -u root -p12345678 wvp273 -e "ALTER TABLE wvp_device ADD register_time varchar(50) DEFAULT NULL; ALTER TABLE wvp_device ADD keepalive_time varchar(50) DEFAULT NULL;" 2>/dev/null
echo '--- 确认列 ---'
mysql -u root -p12345678 wvp273 -e "DESCRIBE wvp_device" 2>/dev/null | grep -E 'register_time|keepalive_time'
echo '--- 重启wvp ---'
systemctl restart wvp
echo WVP_RESTARTED
