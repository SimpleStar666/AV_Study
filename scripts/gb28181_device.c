/*
 * gb28181_device.c — GB28181 设备模拟器（C 版）
 * ============================================
 * 与 Python 版（gb28181_device.py）行为对齐的 C 实现，用于 6.4「aarch64 交叉编译」教学：
 *
 *   REGISTER(Digest qop=auth) → 200 OK → Keepalive 心跳
 *   → 目录上报（Catalog）/ 设备信息（DeviceInfo）
 *   → 实时点播 INVITE → 200 OK + SDP(y=SSRC) → RTP/PS 推流（15fps）
 *   → 收 BYE 停止推流
 *
 * 媒体数据源：
 *   - 默认：预生成的 H.264 裸流文件（testsrc.h264，AnnexB），整文件读入内存循环推送。
 *   - `-live <fifo>`：实时流模式 —— 从命名管道(FIFO)流式读 H.264（如摄像头经 ffmpeg
 *     v4l2→libx264 编码写入），滚动缓冲按 NAL 切帧，锚定到 SPS 保证首帧为 I 帧。
 *     （aarch64 目标环境无法调用 x86 ffmpeg 子进程，实时流由宿主侧 ffmpeg 编码，
 *      设备应用层只负责封装推流——与真实 IPC 的"ISP/编码器出流，应用层封装"一致。）
 *
 * 编译（aarch64 交叉编译，静态链接免目标环境库依赖）：
 *   aarch64-linux-gnu-gcc -static -O2 -o gb28181_device_aarch64 gb28181_device.c -pthread
 *
 * 运行（QEMU 用户态模拟：syscall 直通宿主内核，UDP 网络与真机一致）：
 *   qemu-aarch64 ./gb28181_device_aarch64
 *
 * 验证 ELF 架构：file gb28181_device_aarch64  →  ELF 64-bit LSB executable, ARM aarch64
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdint.h>

/* ================== 配置（与 Python 版一致） ================== */
#define SIP_SERVER_IP   "192.168.0.197"
#define SIP_SERVER_PORT 5060
#define SIP_LOCAL_PORT  5061
#define DEVICE_ID       "34020000001320000001"
#define DEVICE_PWD      "12345678"
#define PLATFORM_ID     "44030000002000000001"
#define DOMAIN          "4403000000"
#define CHANNEL_ID      "34020000001320000001"
#define DEVICE_NAME     "TS模拟摄像头"

#define RTP_PT   96
#define MTU      1400
#define H264_FILE "/home/ts/testsrc.h264"
#define FRAME_INTERVAL_MS 66   /* 15fps 自限速（与 Python 版 0.066s 一致） */
#define HEARTBEAT_S 60

/* ================== 全局状态 ================== */
static int sip_fd = -1;
static int cseq_no = 0;
static char call_id[64];
static char local_tag[64];
static volatile int streaming = 0;        /* 推流开关（BYE 置 0） */
static unsigned int rtp_ssrc = 0x12345678;
static unsigned int media_ip = 0;         /* INVITE SDP c= 行解析的推流目标 */
static int media_port = 0;                /* INVITE SDP m= 行解析的推流端口 */
static struct { const unsigned char *ptr; int len; } pending[16];
static int pending_cnt = 0;

/* ===== 实时流模式（-live FIFO）：摄像头经 ffmpeg 编码写入命名管道 ===== */
static int live_mode = 0;              /* 1 = FIFO 实时流 */
static const char *live_path = NULL;
static unsigned char *stream_buf = NULL;   /* 滚动缓冲 */
static size_t stream_used = 0;
#define STREAM_CAP (2u << 20)              /* 2 MB */
static unsigned char live_pending_buf[16][8192];   /* pending 独立拷贝（滚动不失效） */
static size_t live_pending_len[16];
static int live_pending_cnt = 0;

/* ================== MD5（RFC 1321 公共域实现） ================== */
typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    unsigned char buffer[64];
} MD5_CTX;

#define MD5_F(x,y,z) (((x)&(y)) | (~(x)&(z)))
#define MD5_G(x,y,z) (((x)&(z)) | ((y)&~(z)))
#define MD5_H(x,y,z) ((x)^(y)^(z))
#define MD5_I(x,y,z) ((y)^((x)|~(z)))
#define MD5_ROTL(x,n) (((x)<<(n)) | ((x)>>(32-(n))))
#define MD5_FF(a,b,c,d,x,s,ac) {(a)+=MD5_F(b,c,d)+(x)+(ac);(a)=MD5_ROTL((a),(s));(a)+=(b);}
#define MD5_GG(a,b,c,d,x,s,ac) {(a)+=MD5_G(b,c,d)+(x)+(ac);(a)=MD5_ROTL((a),(s));(a)+=(b);}
#define MD5_HH(a,b,c,d,x,s,ac) {(a)+=MD5_H(b,c,d)+(x)+(ac);(a)=MD5_ROTL((a),(s));(a)+=(b);}
#define MD5_II(a,b,c,d,x,s,ac) {(a)+=MD5_I(b,c,d)+(x)+(ac);(a)=MD5_ROTL((a),(s));(a)+=(b);}

static const unsigned char md5_padding[64] = {0x80,0};

static void md5_init(MD5_CTX *ctx) {
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe; ctx->state[3] = 0x10325476;
}
static void md5_transform(MD5_CTX *ctx, const unsigned char block[64]) {
    uint32_t a=ctx->state[0], b=ctx->state[1], c=ctx->state[2], d=ctx->state[3], x[16];
    int i;
    for (i=0;i<16;i++) x[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1]<<8) |
                              ((uint32_t)block[i*4+2]<<16) | ((uint32_t)block[i*4+3]<<24);
    MD5_FF(a,b,c,d,x[ 0], 7,0xd76aa478); MD5_FF(d,a,b,c,x[ 1],12,0xe8c7b756);
    MD5_FF(c,d,a,b,x[ 2],17,0x242070db); MD5_FF(b,c,d,a,x[ 3],22,0xc1bdceee);
    MD5_FF(a,b,c,d,x[ 4], 7,0xf57c0faf); MD5_FF(d,a,b,c,x[ 5],12,0x4787c62a);
    MD5_FF(c,d,a,b,x[ 6],17,0xa8304613); MD5_FF(b,c,d,a,x[ 7],22,0xfd469501);
    MD5_FF(a,b,c,d,x[ 8], 7,0x698098d8); MD5_FF(d,a,b,c,x[ 9],12,0x8b44f7af);
    MD5_FF(c,d,a,b,x[10],17,0xffff5bb1); MD5_FF(b,c,d,a,x[11],22,0x895cd7be);
    MD5_FF(a,b,c,d,x[12], 7,0x6b901122); MD5_FF(d,a,b,c,x[13],12,0xfd987193);
    MD5_FF(c,d,a,b,x[14],17,0xa679438e); MD5_FF(b,c,d,a,x[15],22,0x49b40821);
    MD5_GG(a,b,c,d,x[ 1], 5,0xf61e2562); MD5_GG(d,a,b,c,x[ 6], 9,0xc040b340);
    MD5_GG(c,d,a,b,x[11],14,0x265e5a51); MD5_GG(b,c,d,a,x[ 0],20,0xe9b6c7aa);
    MD5_GG(a,b,c,d,x[ 5], 5,0xd62f105d); MD5_GG(d,a,b,c,x[10], 9,0x02441453);
    MD5_GG(c,d,a,b,x[15],14,0xd8a1e681); MD5_GG(b,c,d,a,x[ 4],20,0xe7d3fbc8);
    MD5_GG(a,b,c,d,x[ 9], 5,0x21e1cde6); MD5_GG(d,a,b,c,x[14], 9,0xc33707d6);
    MD5_GG(c,d,a,b,x[ 3],14,0xf4d50d87); MD5_GG(b,c,d,a,x[ 8],20,0x455a14ed);
    MD5_GG(a,b,c,d,x[13], 5,0xa9e3e905); MD5_GG(d,a,b,c,x[ 2], 9,0xfcefa3f8);
    MD5_GG(c,d,a,b,x[ 7],14,0x676f02d9); MD5_GG(b,c,d,a,x[12],20,0x8d2a4c8a);
    MD5_HH(a,b,c,d,x[ 5], 4,0xfffa3942); MD5_HH(d,a,b,c,x[ 8],11,0x8771f681);
    MD5_HH(c,d,a,b,x[11],16,0x6d9d6122); MD5_HH(b,c,d,a,x[14],23,0xfde5380c);
    MD5_HH(a,b,c,d,x[ 1], 4,0xa4beea44); MD5_HH(d,a,b,c,x[ 4],11,0x4bdecfa9);
    MD5_HH(c,d,a,b,x[ 7],16,0xf6bb4b60); MD5_HH(b,c,d,a,x[10],23,0xbebfbc70);
    MD5_HH(a,b,c,d,x[13], 4,0x289b7ec6); MD5_HH(d,a,b,c,x[ 0],11,0xeaa127fa);
    MD5_HH(c,d,a,b,x[ 3],16,0xd4ef3085); MD5_HH(b,c,d,a,x[ 6],23,0x04881d05);
    MD5_HH(a,b,c,d,x[ 9], 4,0xd9d4d039); MD5_HH(d,a,b,c,x[12],11,0xe6db99e5);
    MD5_HH(c,d,a,b,x[15],16,0x1fa27cf8); MD5_HH(b,c,d,a,x[ 2],23,0xc4ac5665);
    MD5_II(a,b,c,d,x[ 0], 6,0xf4292244); MD5_II(d,a,b,c,x[ 7],10,0x432aff97);
    MD5_II(c,d,a,b,x[14],15,0xab9423a7); MD5_II(b,c,d,a,x[ 5],21,0xfc93a039);
    MD5_II(a,b,c,d,x[12], 6,0x655b59c3); MD5_II(d,a,b,c,x[ 3],10,0x8f0ccc92);
    MD5_II(c,d,a,b,x[10],15,0xffeff47d); MD5_II(b,c,d,a,x[ 1],21,0x85845dd1);
    MD5_II(a,b,c,d,x[ 8], 6,0x6fa87e4f); MD5_II(d,a,b,c,x[15],10,0xfe2ce6e0);
    MD5_II(c,d,a,b,x[ 6],15,0xa3014314); MD5_II(b,c,d,a,x[13],21,0x4e0811a1);
    MD5_II(a,b,c,d,x[ 4], 6,0xf7537e82); MD5_II(d,a,b,c,x[11],10,0xbd3af235);
    MD5_II(c,d,a,b,x[ 2],15,0x2ad7d2bb); MD5_II(b,c,d,a,x[ 9],21,0xeb86d391);
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
}
static void md5_update(MD5_CTX *ctx, const unsigned char *input, size_t len) {
    size_t i, idx, part;
    idx = (size_t)((ctx->count[0] >> 3) & 0x3F);
    if ((ctx->count[0] += (uint32_t)len << 3) < ((uint32_t)len << 3)) ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);
    part = 64 - idx;
    if (len >= part) {
        memcpy(&ctx->buffer[idx], input, part);
        md5_transform(ctx, ctx->buffer);
        for (i = part; i + 63 < len; i += 64) md5_transform(ctx, &input[i]);
        idx = 0;
    } else i = 0;
    memcpy(&ctx->buffer[idx], &input[i], len - i);
}
static void md5_encode(unsigned char *output, uint32_t *input, unsigned int len);
static void md5_final(MD5_CTX *ctx, unsigned char digest[16]) {
    static unsigned char bits[8];
    unsigned int idx, padlen;
    unsigned char *p;
    idx = (unsigned int)((ctx->count[0] >> 3) & 0x3F);
    md5_encode(bits, ctx->count, 8);
    padlen = (idx < 56) ? (56 - idx) : (120 - idx);
    md5_update(ctx, md5_padding, padlen);
    md5_update(ctx, bits, 8);
    for (idx = 0; idx < 4; idx++) {
        p = digest + idx*4;
        p[0] = ctx->state[idx] & 0xFF; p[1] = (ctx->state[idx]>>8)&0xFF;
        p[2] = (ctx->state[idx]>>16)&0xFF; p[3] = (ctx->state[idx]>>24)&0xFF;
    }
}
static void md5_encode(unsigned char *output, uint32_t *input, unsigned int len) {
    unsigned int i, j;
    for (i = 0, j = 0; j < len; i++, j += 4) {
        output[j]   = input[i] & 0xFF;
        output[j+1] = (input[i] >> 8) & 0xFF;
        output[j+2] = (input[i] >> 16) & 0xFF;
        output[j+3] = (input[i] >> 24) & 0xFF;
    }
}
static void md5_hex(const char *in, char *out_hex) {
    MD5_CTX ctx;
    unsigned char digest[16];
    int i;
    md5_init(&ctx);
    md5_update(&ctx, (const unsigned char *)in, strlen(in));
    md5_final(&ctx, digest);
    for (i = 0; i < 16; i++) sprintf(out_hex + i*2, "%02x", digest[i]);
}

/* ================== SIP 小工具 ================== */
static int next_cseq(void) { return ++cseq_no; }

/* 从消息中取某个头字段的值（含折叠查找简化版：仅单行） */
static void header_value(const char *text, const char *name, char *out, size_t outsz) {
    const char *p = strstr(text, name);
    size_t i;
    if (!p) { out[0] = 0; return; }
    p += strlen(name);
    while (*p == ' ') p++;
    for (i = 0; i < outsz - 1 && p[i] && p[i] != '\r' && p[i] != '\n'; i++) out[i] = p[i];
    out[i] = 0;
}
/* 从字符串中提取 key="value" 形式的值 */
static void kv_value(const char *text, const char *key, char *out, size_t outsz) {
    char pat[128];
    const char *p;
    size_t i;
    snprintf(pat, sizeof(pat), "%s=\"", key);
    p = strstr(text, pat);
    if (!p) { out[0] = 0; return; }
    p += strlen(pat);
    for (i = 0; i < outsz - 1 && p[i] && p[i] != '"'; i++) out[i] = p[i];
    out[i] = 0;
}

static void sip_send(const char *msg) {
    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SIP_SERVER_PORT);
    inet_pton(AF_INET, SIP_SERVER_IP, &sa.sin_addr);
    sendto(sip_fd, msg, strlen(msg), 0, (struct sockaddr *)&sa, sizeof(sa));
}

/* ================== SIP 消息构造 ================== */
static void build_register(char *buf, size_t bufsz, const char *auth_header) {
    char uri[128], branch[32], msg[1024];
    snprintf(uri, sizeof(uri), "sip:%s@%s", PLATFORM_ID, DOMAIN);
    snprintf(branch, sizeof(branch), "z9hG4bK%08lx", (unsigned long)random());
    if (auth_header) {
        snprintf(msg, sizeof(msg),
            "REGISTER %s SIP/2.0\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:%d;rport;branch=%s\r\n"
            "From: <sip:%s@%s>;tag=%s\r\n"
            "To: <sip:%s@%s>\r\n"
            "Call-ID: %s\r\n"
            "CSeq: %d REGISTER\r\n"
            "Contact: <sip:%s@127.0.0.1:%d>\r\n"
            "Max-Forwards: 70\r\n"
            "User-Agent: C-GB28181Sim\r\n"
            "Expires: 3600\r\n"
            "Authorization: %s\r\n"
            "Content-Length: 0\r\n\r\n",
            uri, SIP_LOCAL_PORT, branch, DEVICE_ID, DOMAIN, local_tag,
            PLATFORM_ID, DOMAIN, call_id, next_cseq(), DEVICE_ID, SIP_LOCAL_PORT, auth_header);
    } else {
        snprintf(msg, sizeof(msg),
            "REGISTER %s SIP/2.0\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:%d;rport;branch=%s\r\n"
            "From: <sip:%s@%s>;tag=%s\r\n"
            "To: <sip:%s@%s>\r\n"
            "Call-ID: %s\r\n"
            "CSeq: %d REGISTER\r\n"
            "Contact: <sip:%s@127.0.0.1:%d>\r\n"
            "Max-Forwards: 70\r\n"
            "User-Agent: C-GB28181Sim\r\n"
            "Expires: 3600\r\n"
            "Content-Length: 0\r\n\r\n",
            uri, SIP_LOCAL_PORT, branch, DEVICE_ID, DOMAIN, local_tag,
            PLATFORM_ID, DOMAIN, call_id, next_cseq(), DEVICE_ID, SIP_LOCAL_PORT);
    }
    snprintf(buf, bufsz, "%s", msg);
}

static void build_message(char *buf, size_t bufsz, const char *xml_body) {
    char uri[128], branch[32];
    snprintf(uri, sizeof(uri), "sip:%s@%s", PLATFORM_ID, DOMAIN);
    snprintf(branch, sizeof(branch), "z9hG4bK%08lx", (unsigned long)random());
    snprintf(buf, bufsz,
        "MESSAGE %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 127.0.0.1:%d;rport;branch=%s\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d MESSAGE\r\n"
        "Max-Forwards: 70\r\n"
        "User-Agent: C-GB28181Sim\r\n"
        "Content-Type: Application/MANSCDP+xml\r\n"
        "Content-Length: %lu\r\n\r\n%s",
        uri, SIP_LOCAL_PORT, branch, DEVICE_ID, DOMAIN, local_tag,
        PLATFORM_ID, DOMAIN, call_id, next_cseq(), (unsigned long)strlen(xml_body), xml_body);
}

static void build_200(char *buf, size_t bufsz, const char *req,
                      const char *extra_headers, const char *body) {
    char via[512], from[512], to[512], cseq[128];
    header_value(req, "Via:", via, sizeof(via));
    header_value(req, "From:", from, sizeof(from));
    header_value(req, "To:", to, sizeof(to));
    header_value(req, "CSeq:", cseq, sizeof(cseq));
    /* 注意：header_value 会把 "To:" 匹配到 "From:" 里的 To 段，这里做修正：CSeq 用首行方法 */
    if (strncmp(req, "INVITE", 6) == 0 && !cseq[0]) snprintf(cseq, sizeof(cseq), "%d INVITE", next_cseq());
    if (!via[0]) snprintf(via, sizeof(via), "SIP/2.0/UDP 127.0.0.1:%d", SIP_LOCAL_PORT);
    if (body) {
        snprintf(buf, bufsz,
            "SIP/2.0 200 OK\r\n"
            "Via: %s\r\n"
            "From: %s\r\n"
            "To: %s;tag=%08lx\r\n"
            "Call-ID: %s\r\n"
            "CSeq: %s\r\n"
            "User-Agent: C-GB28181Sim\r\n"
            "%s"
            "Content-Type: application/sdp\r\n"
            "Content-Length: %lu\r\n\r\n%s",
            via, from, to, (unsigned long)random(), call_id, cseq,
            extra_headers ? extra_headers : "", (unsigned long)strlen(body), body);
    } else {
        snprintf(buf, bufsz,
            "SIP/2.0 200 OK\r\n"
            "Via: %s\r\n"
            "From: %s\r\n"
            "To: %s;tag=%08lx\r\n"
            "Call-ID: %s\r\n"
            "CSeq: %s\r\n"
            "User-Agent: C-GB28181Sim\r\n"
            "%s"
            "Content-Length: 0\r\n\r\n",
            via, from, to, (unsigned long)random(), call_id, cseq,
            extra_headers ? extra_headers : "");
    }
}

/* ================== XML（MANSCDP） ================== */
static void xml_keepalive(char *buf, size_t bufsz, long sn) {
    snprintf(buf, bufsz,
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\n"
        "<Notify>\n<CmdType>Keepalive</CmdType>\n<SN>%ld</SN>\n"
        "<DeviceID>%s</DeviceID>\n<Status>OK</Status>\n</Notify>\n",
        sn, DEVICE_ID);
}
static void xml_catalog_response(char *buf, size_t bufsz, long sn) {
    snprintf(buf, bufsz,
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\n"
        "<Response>\n<CmdType>Catalog</CmdType>\n<SN>%ld</SN>\n"
        "<DeviceID>%s</DeviceID>\n<SumNum>1</SumNum>\n<DeviceList>\n"
        "<Item>\n"
        "<DeviceID>%s</DeviceID>\n"
        "<Name>%s</Name>\n"
        "<Manufacturer>TS</Manufacturer>\n"
        "<Model>SIM-IPC</Model>\n"
        "<Owner>ts</Owner>\n"
        "<CivilCode>440300</CivilCode>\n"
        "<Address>WSL</Address>\n"
        "<Parental>0</Parental>\n"
        "<SafetyWay>0</SafetyWay>\n"
        "<RegisterWay>1</RegisterWay>\n"
        "<Secrecy>0</Secrecy>\n"
        "<Status>ON</Status>\n"
        "<PTZType>0</PTZType>\n"
        "</Item>\n</DeviceList>\n</Response>\n",
        sn, DEVICE_ID, CHANNEL_ID, DEVICE_NAME);
}
static void xml_device_info(char *buf, size_t bufsz, long sn) {
    snprintf(buf, bufsz,
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\n"
        "<Response>\n<CmdType>DeviceInfo</CmdType>\n<SN>%ld</SN>\n"
        "<DeviceID>%s</DeviceID>\n<Result>OK</Result>\n"
        "<DeviceName>%s</DeviceName>\n"
        "<Manufacturer>TS</Manufacturer>\n"
        "<Model>SIM-IPC</Model>\n"
        "<Firmware>V1.0</Firmware>\n"
        "<Channel>1</Channel>\n"
        "</Response>\n",
        sn, DEVICE_ID, DEVICE_NAME);
}

/* ================== PS → RTP 打包（字节与 Python 版一致） ================== */
static const unsigned char PACK_HEADER[]   = {0x00,0x00,0x01,0xba,0x44,0x00,0x04,0x00,0x04,0x01,0x86,0x66,0xcf,0xf8};
static const unsigned char SYSTEM_HEADER[] = {0x00,0x00,0x01,0xbb,0x00,0x09,0xc3,0x33,0x67,0x00,0x21,0xff,0xe0,0xe0,0xe6,0x00};
static const unsigned char PSM[]           = {0x00,0x00,0x01,0xbc,0x00,0x0e,0x01,0x01,0x00,0x00,0x00,0x04,0x1b,0xe0,0x00,0x00,0x00,0x00,0x00,0x00}; /* 20B：PSM 含 4B CRC，与 Python 版一致 */

/* MPEG PTS 33bit → 5 字节（与 Python encode_pts 一致） */
static void encode_pts(unsigned char out[5], unsigned long long pts) {
    pts &= 0x1FFFFFFFFULL;
    out[0] = 0x21 | ((pts >> 29) & 0x0E);
    out[1] = (pts >> 22) & 0xFF;
    out[2] = 0x01 | ((pts >> 15) & 0xFE);
    out[3] = (pts >> 7) & 0xFF;
    out[4] = 0x01 | ((pts & 0x7F) << 1);
}

static void send_rtp_packet(const unsigned char *ps, int ps_len, unsigned int *seq,
                            unsigned int ts) {
    struct sockaddr_in sa;
    unsigned char rtp[12 + MTU];
    int off = 0, piece;
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)media_port);
    sa.sin_addr.s_addr = media_ip;   /* inet_addr 已是网络字节序 */
    while (off < ps_len) {
        piece = (ps_len - off > MTU) ? MTU : ps_len - off;
        int last = (off + piece >= ps_len);
        rtp[0] = 0x80;
        rtp[1] = RTP_PT | (last ? 0x80 : 0);
        rtp[2] = (*seq >> 8) & 0xFF;  rtp[3] = *seq & 0xFF;
        rtp[4] = (ts >> 24) & 0xFF;   rtp[5] = (ts >> 16) & 0xFF;
        rtp[6] = (ts >> 8) & 0xFF;    rtp[7] = ts & 0xFF;
        rtp[8] = (rtp_ssrc >> 24) & 0xFF; rtp[9] = (rtp_ssrc >> 16) & 0xFF;
        rtp[10] = (rtp_ssrc >> 8) & 0xFF; rtp[11] = rtp_ssrc & 0xFF;
        memcpy(rtp + 12, ps + off, piece);
        sendto(sip_fd, rtp, 12 + piece, 0, (struct sockaddr *)&sa, sizeof(sa));
        (*seq)++;
        off += piece;
    }
}

/* 推流线程：循环读 H.264 内存缓冲，切 NAL，组帧 → PS → RTP */
static unsigned char *h264_buf = NULL;
static size_t h264_len = 0;

static int find_sc(const unsigned char *d, size_t len, size_t pos, size_t *sc_len) {
    size_t j = pos;
    while (j + 3 <= len) {
        if (d[j]==0 && d[j+1]==0 && d[j+2]==1) { *sc_len = 3; return (int)j; }
        if (j + 4 <= len && d[j]==0 && d[j+1]==0 && d[j+2]==0 && d[j+3]==1) { *sc_len = 4; return (int)j; }
        j++;
    }
    return -1;
}

static void *push_loop(void *arg) {
    unsigned int seq = (unsigned int)random() & 0xFFFF;
    unsigned long long pts = 0;
    size_t pos = 0;
    int frames = 0, sent = 0;
    static int dbg_cnt = 0;
    printf("[媒体] 推流线程启动（目标 %u.%u.%u.%u:%d，SSRC=%08x）\n",
           (media_ip>>24)&0xFF,(media_ip>>16)&0xFF,(media_ip>>8)&0xFF,media_ip&0xFF,
           media_port, rtp_ssrc);
    while (streaming) {
        size_t f1_len = 3, f2_len = 3;
        int f1 = find_sc(h264_buf, h264_len, pos, &f1_len);
        if (f1 < 0) { pos = 0; continue; }   /* 已到文件尾，回绕 */
        int f2 = find_sc(h264_buf, h264_len, f1 + f1_len, &f2_len);
        if (f2 < 0) { pos = 0; continue; }   /* 文件尾，回绕 */
        const unsigned char *nal = h264_buf + f1;
        int nal_len = f2 - f1;
        int nt = (nal[2]==0) ? (nal[4] & 0x1F) : (nal[3] & 0x1F);
        if (dbg_cnt++ % 20 == 0)
            printf("[DBG] pos=%zu f1=%d sc1=%zu f2=%d sc2=%zu nt=%d nal_len=%d h264_len=%zu\n",
                   pos, f1, f1_len, f2, f2_len, nt, nal_len, h264_len);
        if (nt == 1 || nt == 5) {            /* slice → 一帧边界 */
            /* 组 PS：pack + system_header + PSM + PES(pending+slice) */
            unsigned char ps[131072];
            int ps_len = 0;
            unsigned char pes_hdr[14];
            int i;
            memcpy(ps + ps_len, PACK_HEADER, sizeof(PACK_HEADER)); ps_len += sizeof(PACK_HEADER);
            memcpy(ps + ps_len, SYSTEM_HEADER, sizeof(SYSTEM_HEADER)); ps_len += sizeof(SYSTEM_HEADER);
            memcpy(ps + ps_len, PSM, sizeof(PSM)); ps_len += sizeof(PSM);
            /* PES 头：start_code(4) + length(2) + flags(3) + PTS(5) = 14 字节 */
            pes_hdr[0]=0x00; pes_hdr[1]=0x00; pes_hdr[2]=0x01; pes_hdr[3]=0xe0;
            encode_pts(pes_hdr + 9, pts);
            int payload_len = 0, k;
            for (k = 0; k < pending_cnt; k++) payload_len += pending[k].len;
            payload_len += nal_len;
            unsigned int pes_len = (unsigned int)payload_len + 8;
            if (pes_len > 0xFFFF) pes_len = 0;
            pes_hdr[4] = (pes_len >> 8) & 0xFF; pes_hdr[5] = pes_len & 0xFF;
            pes_hdr[6]=0x80; pes_hdr[7]=0x80; pes_hdr[8]=0x05;
            memcpy(ps + ps_len, pes_hdr, 14); ps_len += 14;
            for (k = 0; k < pending_cnt; k++) { memcpy(ps + ps_len, pending[k].ptr, pending[k].len); ps_len += pending[k].len; }
            memcpy(ps + ps_len, nal, nal_len); ps_len += nal_len;
            pending_cnt = 0;
            send_rtp_packet(ps, ps_len, &seq, (unsigned int)(pts & 0xFFFFFFFF));
            if (dbg_cnt % 20 == 1)
                printf("[DBG] SEND frame#%d seq=%u ts=%u ps_len=%d\n", frames, seq, (unsigned int)(pts & 0xFFFFFFFF), ps_len);
            pts = (pts + 6000) & 0x1FFFFFFFFULL;   /* 90000Hz/15fps */
            sent++;
            frames++;
            if (frames % 10 == 0) printf("[媒体] 已推 %d 帧 %d 个 RTP 包，seq=%u\n", frames, sent, seq);
            usleep(FRAME_INTERVAL_MS * 1000);
        } else {
            /* 非 slice（SPS/PPS/SEI/AUD）→ pending */
            if (pending_cnt < 16) { pending[pending_cnt].ptr = nal; pending[pending_cnt].len = nal_len; pending_cnt++; }
        }
        pos = f2;
    }
    printf("[媒体] 推流结束（BYE/停止）\n");
    return NULL;
}

/* 实时流推流线程：从 FIFO 流式读 H.264（AnnexB），滚动缓冲 + 按 NAL 切帧，组 PS 推 RTP。
 * 与文件模式共用相同的 PS 字节布局（PSM 20B / PES 头 14B / pts+6000 / 15fps 自限速）。
 * 锚定策略：丢弃 SPS(7) 之前的垃圾数据，从 SPS 开始累积，保证首帧 = SPS/PPS/SEI/IDR。 */
static void *push_loop_live(void *arg) {
    unsigned int seq = (unsigned int)random() & 0xFFFF;
    unsigned long long pts = 0;
    int frames = 0, sent = 0, got_anchor = 0;
    FILE *fp = NULL;
    stream_buf = malloc(STREAM_CAP);
    if (!stream_buf) { fprintf(stderr, "malloc stream_buf 失败\n"); return NULL; }
    printf("[媒体] 实时流模式启动（读 %s，等待 ffmpeg 写入端）\n", live_path);
    for (;;) {   /* 打开 FIFO：阻塞直到写入端出现 */
        fp = fopen(live_path, "rb");
        if (fp) break;
        if (!streaming) { free(stream_buf); stream_buf = NULL; return NULL; }
        usleep(200000);
    }
    setvbuf(fp, NULL, _IONBF, 0);
    printf("[媒体] FIFO 已连接，流式切帧推流（目标 %u.%u.%u.%u:%d，SSRC=%08x）\n",
           (media_ip>>24)&0xFF,(media_ip>>16)&0xFF,(media_ip>>8)&0xFF,media_ip&0xFF,
           media_port, rtp_ssrc);
    while (streaming) {
        size_t n = fread(stream_buf + stream_used, 1, 65536, fp);
        if (n == 0) {   /* 写端暂歇/关闭 → 重开 FIFO 等待 */
            if (!streaming) break;
            fclose(fp); fp = NULL;
            for (;;) {
                if (!streaming) break;
                fp = fopen(live_path, "rb");
                if (fp) { setvbuf(fp, NULL, _IONBF, 0); break; }
                usleep(200000);
            }
            continue;
        }
        stream_used += n;
        size_t pos = 0;
        while (1) {
            size_t sc_len = 3;
            int f1 = find_sc(stream_buf, stream_used, pos, &sc_len);
            if (f1 < 0) { pos = 0; break; }                       /* 无 start code，等更多 */
            size_t f2_sc = 3;
            int f2 = find_sc(stream_buf, stream_used, f1 + (int)sc_len, &f2_sc);
            if (f2 < 0) { pos = (size_t)f1; break; }              /* 当前 NAL 不完整，等更多 */
            const unsigned char *nal = stream_buf + f1;
            int nal_len = f2 - f1;
            int nt = (nal[2] == 0) ? (nal[4] & 0x1F) : (nal[3] & 0x1F);
            if (!got_anchor) {                                    /* 锚定前全部丢弃 */
                if (nt == 7) {
                    got_anchor = 1;
                    if (live_pending_cnt < 16) {
                        int cp = nal_len > 8191 ? 8191 : nal_len;
                        memcpy(live_pending_buf[live_pending_cnt], nal, (size_t)cp);
                        live_pending_len[live_pending_cnt] = (size_t)cp;
                        live_pending_cnt++;
                    }
                }
                pos = (size_t)f2;
                continue;
            }
            if (nt == 1 || nt == 5) {                             /* slice → 帧边界，组 PS */
                unsigned char ps[131072];
                int ps_len = 0;
                unsigned char pes_hdr[14];
                int k;
                memcpy(ps + ps_len, PACK_HEADER, sizeof(PACK_HEADER)); ps_len += sizeof(PACK_HEADER);
                memcpy(ps + ps_len, SYSTEM_HEADER, sizeof(SYSTEM_HEADER)); ps_len += sizeof(SYSTEM_HEADER);
                memcpy(ps + ps_len, PSM, sizeof(PSM)); ps_len += sizeof(PSM);
                pes_hdr[0]=0x00; pes_hdr[1]=0x00; pes_hdr[2]=0x01; pes_hdr[3]=0xe0;
                encode_pts(pes_hdr + 9, pts);
                int payload_len = 0;
                for (k = 0; k < live_pending_cnt; k++) payload_len += (int)live_pending_len[k];
                payload_len += nal_len;
                unsigned int pes_len = (unsigned int)payload_len + 8;
                if (pes_len > 0xFFFF) pes_len = 0;
                pes_hdr[4] = (pes_len >> 8) & 0xFF; pes_hdr[5] = pes_len & 0xFF;
                pes_hdr[6]=0x80; pes_hdr[7]=0x80; pes_hdr[8]=0x05;
                memcpy(ps + ps_len, pes_hdr, 14); ps_len += 14;
                for (k = 0; k < live_pending_cnt; k++) { memcpy(ps + ps_len, live_pending_buf[k], live_pending_len[k]); ps_len += (int)live_pending_len[k]; }
                memcpy(ps + ps_len, nal, (size_t)nal_len); ps_len += nal_len;
                live_pending_cnt = 0;
                send_rtp_packet(ps, ps_len, &seq, (unsigned int)(pts & 0xFFFFFFFF));
                pts = (pts + 6000) & 0x1FFFFFFFFULL;
                sent++; frames++;
                if (frames % 10 == 0)
                    printf("[媒体] 实时流已推 %d 帧 %d 包 seq=%u\n", frames, sent, seq);
                usleep(FRAME_INTERVAL_MS * 1000);
            } else {                                               /* SPS/PPS/SEI/AUD → pending 拷贝 */
                if (live_pending_cnt < 16) {
                    int cp = nal_len > 8191 ? 8191 : nal_len;
                    memcpy(live_pending_buf[live_pending_cnt], nal, (size_t)cp);
                    live_pending_len[live_pending_cnt] = (size_t)cp;
                    live_pending_cnt++;
                }
            }
            pos = (size_t)f2;
        }
        if (pos > 0) {                                          /* 滚动保留未消费尾部（含 pos==stream_used 清零） */
            memmove(stream_buf, stream_buf + pos, stream_used - pos);
            stream_used -= pos;
        }
    }
    if (fp) fclose(fp);
    free(stream_buf); stream_buf = NULL;
    printf("[媒体] 实时流推流结束\n");
    return NULL;
}

/* ================== 注册流程（同步） ================== */
static int do_register(void) {
    char uri[128], auth_header[512];
    char buf[8192];
    struct timeval tv;
    fd_set rset;
    int ok = 0, tries;
    snprintf(uri, sizeof(uri), "sip:%s@%s", PLATFORM_ID, DOMAIN);
    auth_header[0] = 0;
    printf("[SIP] 开始注册 %s ...\n", uri);
    for (tries = 0; tries < 30 && !ok; tries++) {
        char req[1024];
        build_register(req, sizeof(req), auth_header[0] ? auth_header : NULL);
        sip_send(req);
        printf("[SIP] REGISTER 已发送%s\n", auth_header[0] ? "（带 Digest 认证）" : "");
        /* 等 2 秒内的响应 */
        for (;;) {
            tv.tv_sec = 2; tv.tv_usec = 0;
            FD_ZERO(&rset); FD_SET(sip_fd, &rset);
            int r = select(sip_fd + 1, &rset, NULL, NULL, &tv);
            if (r <= 0) break;
            int n = (int)recv(sip_fd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = 0;
            if (strncmp(buf, "SIP/2.0 401", 11) == 0) {
                char realm[128], nonce[128], qop[32];
                char ha1[33], ha2[33], resp[33], to_hash[512];
                kv_value(buf, "realm", realm, sizeof(realm));
                kv_value(buf, "nonce", nonce, sizeof(nonce));
                kv_value(buf, "qop", qop, sizeof(qop));
                snprintf(to_hash, sizeof(to_hash), "%s:%s:%s", DEVICE_ID, realm, DEVICE_PWD);
                md5_hex(to_hash, ha1);
                snprintf(to_hash, sizeof(to_hash), "%s:%s", "REGISTER", uri);
                md5_hex(to_hash, ha2);
                if (qop[0]) {
                    const char *cnonce = "0a4f113b";
                    snprintf(to_hash, sizeof(to_hash), "%s:%s:00000001:%s:%s:%s", ha1, nonce, cnonce, qop, ha2);
                    md5_hex(to_hash, resp);
                    snprintf(auth_header, sizeof(auth_header),
                        "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                        "response=\"%s\", algorithm=MD5, qop=%s, nc=00000001, cnonce=\"%s\"",
                        DEVICE_ID, realm, nonce, uri, resp, qop, cnonce);
                } else {
                    snprintf(to_hash, sizeof(to_hash), "%s:%s:%s", ha1, nonce, ha2);
                    md5_hex(to_hash, resp);
                    snprintf(auth_header, sizeof(auth_header),
                        "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                        "response=\"%s\", algorithm=MD5", DEVICE_ID, realm, nonce, uri, resp);
                }
                printf("[SIP] 收到 401（qop=%s），已计算 Digest response\n", qop[0] ? qop : "无");
            } else if (strncmp(buf, "SIP/2.0 200", 11) == 0) {
                printf("[SIP] 注册成功（200 OK）\n");
                ok = 1;
                break;
            }
        }
    }
    return ok;
}

/* ================== 心跳线程 ================== */
static void *heartbeat_loop(void *arg) {
    char xml[512], msg[1024];
    long sn = 1;
    for (;;) {
        sleep(HEARTBEAT_S);
        xml_keepalive(xml, sizeof(xml), sn++);
        build_message(msg, sizeof(msg), xml);
        sip_send(msg);
        printf("[SIP] 心跳 Keepalive 已发送\n");
    }
    return NULL;
}

/* ================== 请求处理 ================== */
static void handle_request(const char *text) {
    char buf[8192], xml[1024], msg[2048];
    const char *body;
    const char *sep = strstr(text, "\r\n\r\n");
    body = sep ? sep + 4 : "";
    if (strncmp(text, "INVITE ", 7) == 0) {
        char ip[64], ssrc_s[32], sdp[512], resp[2048];
        long port;
        const char *p = strstr(body, "c=IN IP4 ");
        const char *m = strstr(body, "m=video ");
        const char *y = strstr(body, "y=");
        if (!p || !m) { printf("[SIP] INVITE SDP 解析失败\n"); return; }
        sscanf(p + 9, "%63s", ip);
        sscanf(m + 8, "%ld", &port);
        media_port = (int)port;
        media_ip = (unsigned int)inet_addr(ip);
        if (y) {
            sscanf(y + 2, "%31s", ssrc_s);
            rtp_ssrc = (unsigned int)strtoul(ssrc_s, NULL, 10) & 0xFFFFFFFFu;
            printf("[SIP] 收到 INVITE 点播 → 推流目标 %s:%ld SSRC=%08x\n", ip, port, rtp_ssrc);
        } else {
            printf("[SIP] 收到 INVITE 点播 → 推流目标 %s:%ld（无 y=，用默认 SSRC）\n", ip, port);
        }
        snprintf(sdp, sizeof(sdp),
            "v=0\n"
            "o=%s 0 0 IN IP4 127.0.0.1\n"
            "s=Play\n"
            "c=IN IP4 127.0.0.1\n"
            "t=0 0\n"
            "m=video 0 RTP/AVP 96\n"
            "a=rtpmap:96 PS/90000\n"
            "a=sendonly\n"
            "y=%08x\n", DEVICE_ID, rtp_ssrc);
        build_200(resp, sizeof(resp), text, NULL, sdp);
        sip_send(resp);
        printf("[SIP] 已回 200 OK + SDP(sendonly)，启动推流\n");
        if (!streaming) {
            streaming = 1;
            pthread_t th;
            pthread_create(&th, NULL, live_mode ? push_loop_live : push_loop, NULL);
        }
    } else if (strncmp(text, "MESSAGE ", 8) == 0) {
        char sn_s[32], cmd[64], resp[1024];
        sn_s[0] = 0; cmd[0] = 0;
        const char *snp = strstr(body, "<SN>");
        const char *cmdp = strstr(body, "<CmdType>");
        if (snp) sscanf(snp + 4, "%31[^<]", sn_s);
        if (cmdp) sscanf(cmdp + 9, "%63[^<]", cmd);
        printf("[SIP] 收到 MESSAGE 查询: %s\n", cmd);
        build_200(resp, sizeof(resp), text, NULL, NULL);
        sip_send(resp);
        if (strcmp(cmd, "Catalog") == 0) {
            xml_catalog_response(xml, sizeof(xml), sn_s[0] ? atol(sn_s) : 1);
            build_message(msg, sizeof(msg), xml);
            sip_send(msg);
            printf("[SIP] 已上报目录（通道 %s）\n", CHANNEL_ID);
        } else if (strcmp(cmd, "DeviceInfo") == 0) {
            xml_device_info(xml, sizeof(xml), sn_s[0] ? atol(sn_s) : 1);
            build_message(msg, sizeof(msg), xml);
            sip_send(msg);
            printf("[SIP] 已上报设备信息\n");
        }
    } else if (strncmp(text, "BYE ", 4) == 0) {
        char resp[1024];
        printf("[SIP] 收到 BYE（停止点播）\n");
        build_200(resp, sizeof(resp), text, NULL, NULL);
        sip_send(resp);
        streaming = 0;
    } else if (strncmp(text, "OPTIONS ", 8) == 0) {
        char resp[1024];
        build_200(resp, sizeof(resp), text, NULL, NULL);
        sip_send(resp);
    }
}

/* ================== main ================== */
int main(int argc, char *argv[]) {
    struct sockaddr_in sa;
    char buf[8192];
    FILE *fp;
    pthread_t hb;
    /* 参数：[-live <fifo路径>] — 实时流模式（摄像头经 ffmpeg 写入 FIFO）；默认文件循环模式 */
    if (argc >= 3 && strcmp(argv[1], "-live") == 0) {
        live_mode = 1;
        live_path = argv[2];
    }
    setvbuf(stdout, NULL, _IOLBF, 0);   /* 行缓冲：日志实时可见（qemu/重定向下避免全缓冲） */
    /* UDP socket（SO_REUSEADDR 便于快速重启） */
    sip_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sip_fd < 0) { perror("socket"); return 1; }
    int reuse = 1;
    setsockopt(sip_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SIP_LOCAL_PORT);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(sip_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    /* 媒体数据源：live 模式 → FIFO（INVITE 后由 push_loop_live 打开）；默认 → 预生成文件 */
    if (live_mode) {
        printf("实时流模式：媒体源 = %s（FIFO，等待 ffmpeg 写入端）\n", live_path);
    } else {
        fp = fopen(H264_FILE, "rb");
        if (!fp) { fprintf(stderr, "无法打开 %s\n", H264_FILE); return 1; }
        fseek(fp, 0, SEEK_END);
        h264_len = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        h264_buf = malloc(h264_len);
        if (fread(h264_buf, 1, h264_len, fp) != h264_len) { fprintf(stderr, "读文件失败\n"); return 1; }
        fclose(fp);
        printf("H.264 码流: %zu 字节（循环推送）\n", h264_len);
    }
    /* 随机 Call-ID / tag */
    srandom((unsigned int)time(NULL));
    snprintf(call_id, sizeof(call_id), "%08lx@127.0.0.1", (unsigned long)random());
    snprintf(local_tag, sizeof(local_tag), "%08lx", (unsigned long)random());
    printf("====================================\n");
    printf("GB28181 设备模拟器（C 版 / aarch64）\n");
    printf("设备ID  : %s\n", DEVICE_ID);
    printf("平台SIP : %s:%d\n", SIP_SERVER_IP, SIP_SERVER_PORT);
    printf("本地SIP : 127.0.0.1:%d\n", SIP_LOCAL_PORT);
    printf("====================================\n");
    if (!do_register()) { fprintf(stderr, "注册失败\n"); return 1; }
    pthread_create(&hb, NULL, heartbeat_loop, NULL);
    /* 主循环 */
    while (1) {
        int n = (int)recv(sip_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        buf[n] = 0;
        handle_request(buf);
    }
    return 0;
}
