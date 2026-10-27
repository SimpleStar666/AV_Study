#!/bin/bash
# 第6步 6.2b：MySQL 初始化 wvp 数据库（WSL 内执行）
set -e
sudo service mysql start || true
echo '--- 设置root密码+建库 ---'
sudo mysql -e "ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY '12345678'; CREATE DATABASE IF NOT EXISTS wvp273 DEFAULT CHARACTER SET utf8mb4;"
echo '--- 导入官方初始化SQL ---'
cp '/mnt/c/Users/小陶/Tools/wvp-pro/db/mysql-274.sql' /home/ts/wvp/
mysql -u root -p12345678 wvp273 < /home/ts/wvp/mysql-274.sql
echo '--- 验证表 ---'
mysql -u root -p12345678 wvp273 -e 'SHOW TABLES' | head -10
echo MYSQL_INIT_DONE
