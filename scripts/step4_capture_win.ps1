# 第4步：Windows 侧抓包分析（RTSP 信令 + RTP 数据面）v2
# 用法：
#   ① 先运行本脚本（抓包立即开始）
#   ② 去 WSL 终端 2 运行：bash ~/step4_push.sh
#   ③ 看到推流在跑后，回到本窗口按回车停止抓包并自动分析
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

Write-Output "==== 抓包已开始（接口 #$loopIdx 回环）===="
Write-Output '>>> 现在去 WSL 终端 2 运行：bash ~/step4_push.sh'
Write-Output '>>> 看到推流在跑（fps 跳动）后，回来按回车停止抓包'

$proc = Start-Process -FilePath $tshark -ArgumentList '-i', $loopIdx, '-f', 'tcp port 8554 or udp portrange 8000-8001', '-w', $cap -WindowStyle Hidden -PassThru
if ($env:STEP4_AUTO -eq '1') { Start-Sleep -Seconds 12 } else { Read-Host '按回车停止抓包...' }
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Output ''
Write-Output '==== [A] RTSP 信令面：四次握手（OPTIONS/DESCRIBE/SETUP/PLAY）===='
& $tshark -r $cap -Y 'rtsp.request or rtsp.response' -O rtsp 2>$null | Select-Object -First 65

Write-Output ''
Write-Output '==== [B] RTP 数据面：前 5 个包 ===='
& $tshark -r $cap -d 'udp.port==8000,rtp' -d 'udp.port==8001,rtcp' -Y 'rtp' -c 5 -T fields -e frame.number -e frame.len -e ip.src -e ip.dst -e udp.srcport -e udp.dstport -e rtp.p_type -e rtp.seq -e rtp.timestamp -e rtp.ssrc 2>$null

Write-Output ''
Write-Output '==== [C] 首个 RTP 包原始字节（IP头20 + UDP头8 + RTP头12 + 载荷）===='
& $tshark -r $cap -d 'udp.port==8000,rtp' -Y 'rtp' -c 1 -x 2>$null | Select-Object -First 13

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
