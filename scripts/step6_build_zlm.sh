#!/bin/bash
# 第6步 6.2a：ZLMediaKit 编译（源码已在 /home/ts/ZLMediaKit）
# 教训记录：-j32 OOM（WSL 7.8G 内存装不下 32 个编译器进程）→ 用 -j4
set -u
LOG=/home/ts/step6_zlm_build.log
echo "== 编译启动 $(date) ==" > "$LOG"
cd /home/ts/ZLMediaKit || { echo "NO_SRC_DIR" >> "$LOG"; exit 1; }
rm -rf build
mkdir -p build && cd build || exit 1
echo "== cmake ==" >> "$LOG"
cmake .. -DCMAKE_BUILD_TYPE=Release >> "$LOG" 2>&1 || { echo "CMAKE_FAILED" >> "$LOG"; exit 1; }
echo "== make -j4（内存友好） ==" >> "$LOG"
make -j4 >> "$LOG" 2>&1 || { echo "MAKE_FAILED" >> "$LOG"; exit 1; }
echo "BUILD_OK $(date)" >> "$LOG"
ls -lh ../release/linux/Release/ 2>/dev/null >> "$LOG"
