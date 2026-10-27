#!/bin/bash
# 第6步 6.2a：ZLMediaKit 源码拉取 + 编译（WSL /home/ts 下执行）
# 策略：GitHub 被墙，用 gh-proxy 系列代理拉主仓库；子模块通过仓库内 insteadOf 改写（只影响本仓库，不污染全局）
set -u

PROXIES=("https://gh-proxy.com" "https://ghproxy.net" "https://gh.llkk.cc")
REPO="https://github.com/ZLMediaKit/ZLMediaKit.git"
LOG=~/step6_zlm_build.log
echo "== ZLMediaKit 安装脚本启动 $(date) ==" > "$LOG"

# 1) 拉主仓库
clone_ok=0
if [ -d ~/ZLMediaKit/.git ]; then
  echo "ZLMediaKit 已存在，跳过 clone" >> "$LOG"
  clone_ok=1
else
  for p in "${PROXIES[@]}"; do
    echo "== clone 尝试 $p ==" >> "$LOG"
    if git clone --depth 1 "$p/$REPO" ~/ZLMediaKit >> "$LOG" 2>&1 && [ -d ~/ZLMediaKit/.git ]; then
      clone_ok=1
      echo "clone OK via $p" >> "$LOG"
      break
    fi
    rm -rf ~/ZLMediaKit
  done
fi
if [ $clone_ok -eq 0 ]; then
  echo "CLONE_FAILED 所有代理失败，请检查网络后重跑" >> "$LOG"
  exit 1
fi

# 2) 子模块（zltoolkit/media-server 等），仓库内改写 URL 走代理
cd ~/ZLMediaKit || exit 1
sub_ok=0
for p in "${PROXIES[@]}"; do
  echo "== 子模块尝试 $p ==" >> "$LOG"
  git config url."$p/https://github.com/".insteadOf "https://github.com/"
  git submodule update --init --recursive >> "$LOG" 2>&1
  if git submodule status | grep -q '^-'; then
    echo "子模块不完整，换下一个代理" >> "$LOG"
    git config --unset-all url."$p/https://github.com/".insteadOf || true
    continue
  fi
  sub_ok=1
  echo "SUB_MODULE_OK via $p" >> "$LOG"
  break
done
if [ $sub_ok -eq 0 ]; then
  echo "SUB_MODULE_FAILED" >> "$LOG"
  exit 1
fi

# 3) 编译
echo "== cmake ==" >> "$LOG"
mkdir -p build
cd build || exit 1
cmake .. -DCMAKE_BUILD_TYPE=Release >> "$LOG" 2>&1 || { echo "CMAKE_FAILED" >> "$LOG"; exit 1; }
echo "== make -j32 ==" >> "$LOG"
make -j32 >> "$LOG" 2>&1 || { echo "MAKE_FAILED" >> "$LOG"; exit 1; }

echo "BUILD_OK $(date)" >> "$LOG"
ls -lh ../release/linux/Release/ 2>/dev/null >> "$LOG"
