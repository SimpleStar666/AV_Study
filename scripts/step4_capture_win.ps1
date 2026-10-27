# 第4步：Windows 侧抓包分析（RTSP 信令 + RTP 数据面）v3
# 用法：
#   ① 运行本脚本（抓包立即开始，最多 60 秒）
#   ② 去 WSL 终端 2 运行：bash ~/step4_push.sh，看到 fps 跳动
#   ③ 回来按回车提前停止 → 自动分析 [A]握手 [B]RTP字段 [C]字节 [D]统计
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$tshark = 'C:\Program Files\Wireshark\tshark.exe'
$cap = "$env:USERPROFILE\step4.pcapng"

if (-not (Test-Path -LiteralPath $tshark)) { Write-Output 'tshark 未找到，确认 Wireshark 已安装'; exit 1 }

# 自动探测 Npcap 回环接口编号
$loopIdx = $null
foreach ($line in (& $tshark -D)) {
    if ($line -match 'loopback traffic capture') {
        $loopIdx = ($line -split '\. ')[0]
        break
    }
}
if (-not $loopIdx) { Write-Output '未找到回环抓包接口（Npcap loopback 未安装？）'; exit 1 }

# 关键：先删掉旧文件，防止读到历史数据
Remove-Item -LiteralPath $cap -Force -ErrorAction SilentlyContinue

Write-Output "==== 抓包已开始（接口 #$loopIdx 回环，最多 60 秒）===="
Write-Output '>>> 现在去 WSL 终端 2 运行：bash ~/step4_push.sh'
Write-Output '>>> 看到推流 fps 跳动后，回来按回车停止抓包（可提前按）'

# 注意：ArgumentList 数组按空格拼接、不会为含空格的项加引号，必须整体拼成带引号的单字符串
$tsharkArgs = "-i $loopIdx -f ""tcp port 8554 or udp portrange 8000-8001"" -a duration:60 -w ""$cap"""
$proc = Start-Process -FilePath $tshark -ArgumentList $tsharkArgs -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 2
if ($proc.HasExited) {
    Write-Output '!!! tshark 未能启动抓包，请检查 Npcap 驱动状态后重试'
    exit 1
}
if ($env:STEP4_AUTO -eq '1') { Start-Sleep -Seconds 12 } else { Read-Host '按回车停止抓包...' }
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# 空抓包检查
if (-not (Test-Path -LiteralPath $cap) -or (Get-Item -LiteralPath $cap).Length -lt 200) {
    Write-Output ''
    Write-Output '!!! 抓包结果为空：可能回车按得太快（tshark 还没就绪）或推流没启动。请重跑一次，确保先看到推流 fps 跳动再按回车。'
    exit 1
}

Write-Output ''
Write-Output '==== [A] RTSP 信令面：四次握手（OPTIONS/DESCRIBE/SETUP/PLAY）===='
& $tshark -r $cap -Y 'rtsp.request or rtsp.response' -O rtsp 2>$null | Select-Object -First 70

Write-Output ''
Write-Output '==== [B] RTP 数据面：前 5 个包 ===='
& $tshark -r $cap -d 'udp.port==8000,rtp' -d 'udp.port==8001,rtcp' -Y 'rtp' -T fields -e frame.number -e frame.len -e ip.src -e ip.dst -e udp.srcport -e udp.dstport -e rtp.p_type -e rtp.seq -e rtp.timestamp -e rtp.ssrc 2>$null | Select-Object -First 5

Write-Output ''
Write-Output '==== [C] 首个 RTP 包原始字节（IP头20 + UDP头8 + RTP头12 + 载荷）===='
& $tshark -r $cap -d 'udp.port==8000,rtp' -Y 'rtp' -x 2>$null | Select-Object -First 13

Write-Output ''
Write-Output '==== [B2] 双轨统计（第5步音画同步）===='
$pts = @(& $tshark -r $cap -d 'udp.port==8000,rtp' -Y 'rtp' -T fields -e rtp.p_type 2>$null)
if ($pts.Count -gt 0) {
    $grp = $pts | Group-Object | Sort-Object Count -Descending
    Write-Output "按 PT 分组（96=H.264视频 0=PCMU音频 其他=动态类型）: $($grp | ForEach-Object { "$($_.Name)x$($_.Count)" })"
    Write-Output ''
    Write-Output '--- 音频 RTP 前 4 包（PT/seq/timestamp，8000Hz 时钟，每包增量=采样数）---'
    & $tshark -r $cap -d 'udp.port==8000,rtp' -Y 'rtp.p_type == 0' -T fields -e frame.number -e frame.len -e rtp.seq -e rtp.timestamp 2>$null | Select-Object -First 4
    Write-Output '--- 视频 RTP 前 4 包（同帧多包 ts 相同 = 分片）---'
    & $tshark -r $cap -d 'udp.port==8000,rtp' -Y 'rtp.p_type == 96' -T fields -e frame.number -e frame.len -e rtp.seq -e rtp.timestamp 2>$null | Select-Object -First 4
} else {
    Write-Output 'RTP 包数=0（检查推流是否真的在跑）'
}

Write-Output ''
Write-Output '==== [D] 统计 ===='
$pkts = @(& $tshark -r $cap -d 'udp.port==8000,rtp' -Y 'rtp' -T fields -e frame.len 2>$null)
$n = $pkts.Count
if ($n -gt 0) {
    $s = ($pkts | Measure-Object -Sum).Sum
    Write-Output "RTP 包数=$n 总字节=$s 平均=$([math]::Round($s/$n,1)) B/包"
} else {
    Write-Output 'RTP 包数=0（检查推流是否真的在跑）'
}
Write-Output ''
Write-Output "PCAP 文件：$cap —— 用 Wireshark GUI 打开它，输入 filter：rtsp 或 rtp 慢慢看！"
