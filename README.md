# IPC 音视频开发学习项目（求职向）

目标：应对深圳「嵌入式音视频（IPC / 安防）」岗位招聘，采用**项目驱动**方式学习：每步「讲解 → 动手 → 出题验收 → 点评」，知识内嵌在每个实操步骤里。

## 最终交付项目

### 1. WebRTC 1v1 实时音视频通话系统
独立 repo：[SimpleStar666/rtc-1v1-audio](https://github.com/SimpleStar666/rtc-1v1-audio)
从零基于 libwebsockets / libnice / libsrtp2 / libopus / libvpx 自建 1v1 通话：SDP 协商、Trickle ICE、STUN 打洞、DTLS-SRTP、RTP 分片重组、NACK/PLI、GCC 拥塞控制。

### 2. GB28181 国标 IPC 智能摄像头模拟器（本 repo）
- 从 0 实现 GB/T 28181-2016 IPC 端协议栈（C 语言）
- 对接 wvp-pro + ZLMediaKit 开源平台全链路跑通
- 摄像头采集 → H.264 编码 → MPEG-PS 封装 → RTP/UDP 推流 → 平台建流 → RTSP/FLV 分发
- aarch64 交叉编译 + QEMU 用户态验证（无真机环境）
- 核心源码：`scripts/gb28181_device.c`

## 环境（WSL2 Ubuntu-20.04）

| 组件 | 版本/状态 | 说明 |
|---|---|---|
| ffmpeg / ffprobe / ffplay | 4.2.7 | 转换/分析/播放三件套 |
| x264 | ✓ | H.264 软件编码器 |
| v4l2-ctl | ✓ | 摄像头工具（v4l-utils，usbipd 透传 USB 摄像头） |
| ZLMediaKit | master (2026-10) | 流媒体服务器，RTSP/RTMP/HLS/GB28181 |
| wvp-pro | ✓ | GB28181 国标信令平台（Java/Spring Boot） |
| mediamtx | v1.21.3 | RTSP 测试用流媒体服务器 |
| aarch64 交叉编译链 | aarch64-linux-gnu-gcc | 静态交叉编译 ARM64 设备程序 |
| QEMU | qemu-aarch64 用户态 | 无真机时跑 ARM64 二进制 |

## 学习路线（7 步课表）

| 步 | 内容 | 状态 |
|---|---|---|
| 1 | 搭环境（ffmpeg 全家桶 + mediamtx + 测试视频） | ✅ 完成 |
| 2 | 摄像头挂进 WSL（usbipd + v4l2） | ✅ 完成 |
| 3 | H.264 编码 + RTSP 推流到 mediamtx | ✅ 完成 |
| 4 | ffplay 拉流 + ffprobe 分析 + Wireshark 抓包 | ✅ 完成 |
| 5 | 加 AAC 音频轨，验证音画同步（PTS/DTS） | ✅ 完成 |
| 6 | GB28181（wvp-pro + ZLMediaKit）+ aarch64 交叉编译 + 摄像头实时接入 | ✅ 完成 |
| 7 | 模拟面试 + 简历更新 | ✅ 完成 |

## 目录结构

- `README.md` 本文件
- `WebRTC音视频开发工程师-知识体系与补强手册.md` 知识手册
- `assets/` 测试视频等
- `scripts/gb28181_device.c` GB28181 IPC 设备端核心源码（SIP/PS/RTP）
- `scripts/step6_demo.sh` 全链验收脚本（彩条文件推流模式）
- `scripts/step6_cam.sh` 摄像头实时数据推流验收脚本
- `scripts/step4_*.sh / step5_*.sh` ffmpeg 编码推流脚本
- `docs/学习进度.md` 分步进度与验收记录
