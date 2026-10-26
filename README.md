# IPC 音视频开发学习项目（求职向）

> 目标：应对深圳「嵌入式音视频（IPC / 安防）」岗位招聘，采用**项目驱动**方式学习：
> 每步「我讲 → 你动手 → 出题验收 → 点评」，知识内嵌在每个实操步骤里。

## 环境（WSL2 Ubuntu-20.04）

| 组件 | 版本/状态 | 说明 |
| --- | --- | --- |
| ffmpeg / ffprobe / ffplay | 4.2.7 | 转换/分析/播放三件套 |
| x264 | ✓ | H.264 软件编码器（`-c:v libx264`） |
| v4l2-ctl | ✓ | 摄像头工具（v4l-utils） |
| mediamtx | v1.21.3 | RTSP/RTMP/HLS/WebRTC 流媒体服务器，监听 `:8554` |
| 测试视频 | assets/test.mp4 | 640×360@15fps、5s、H.264（testsrc 生成） |

## 目录结构

```
├── README.md                      本文件
├── WebRTC音视频开发工程师-知识体系与补强手册.md   知识手册（字典，随进度查阅）
├── assets/
│   └── test.mp4                   测试视频（testsrc 生成）
├── scripts/
│   └── dl_mediamtx.sh             mediamtx 下载脚本（gh-proxy 镜像，可复用）
└── docs/
    └── 学习进度.md                分步进度与验收记录
```

## 学习路线（7 步课表）

| 步 | 内容 | 状态 |
| --- | --- | --- |
| 1 | 搭环境（ffmpeg 全家桶 + mediamtx + 测试视频） | ✅ 完成 |
| 2 | 摄像头挂进 WSL（usbipd + v4l2） | ✅ 完成 |
| 3 | H.264 编码 + RTSP 推流到 mediamtx | ⏳ 下一步 |
| 4 | ffplay 拉流 + ffprobe 分析 + Wireshark 抓包 | 待办 |
| 5 | 加 AAC 音频轨，验证音画同步（PTS/DTS） | 待办 |
| 6 | GB28181（ZLMediaKit/WVP）+ aarch64 交叉编译 | 待办 |
| 7 | 模拟面试 + 简历更新 | 待办 |

## 常用命令

```bash
# 启动 RTSP 服务器（保持终端开启）
cd ~ && mediamtx

# 查看摄像头
v4l2-ctl --list-devices

# 抓拍一帧（MJPG）
ffmpeg -y -f v4l2 -input_format mjpeg -video_size 640x480 -i /dev/video0 -frames:v 1 ~/mycap.jpg

# 查看视频信息
ffprobe ~/test.mp4
```
