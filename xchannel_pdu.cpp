#include <string.h>
#include "xchannel.h"
#include "xchannel.inl"
#include "xpack_redis.h"

//*********************************
// 包操作对象定义与实现
// 包格式：
//  1. BLP2协议：2字节长度字段（大端序）- BLP4协议：4字节长度字段（大端序）
//  2. IS_RPC:1字节是否RPC 0:非RPC 1:RPC-REQ 2:RPC-REP
//  3. CO_ID: rpc-4字节协程ID nrpc-0字节
//  4. PK_ID: rpc-4字节包ID nrpc-0字节
//  5. PT：4字节协议号， resp-0字节
//  6. args：根据调用传参数量定义
// ==================== BLP2协议实现 ====================

static xChannelErrCode blp2_check_complete(xChannel* channel) {
    if (!channel) {
        return PACKET_FD_INVALD;
    }
    int len = (int)(channel->rpos - channel->rbuf);
    if (len < 2) {
        return PACKET_INVALID;  // 长度字段值太小
    }

    // 读取2字节长度字段(大端序)
    uint16_t pkg_len = (channel->rbuf[0] << 8) | channel->rbuf[1];
    if (len < pkg_len + 2) {
        return PACKET_INCOMPLETE;
    }

    return PACKET_SUCCESS;
}

static int blp2_write_header(xChannel* channel, size_t data_len) {
    if (!channel || !channel->wbuf) {
        return PACKET_FD_INVALD;
    }
    // 检查缓冲区空间
    if (channel->wlen - (int)(channel->wpos - channel->wbuf) < (int)data_len + 2) {
        return PACKET_BUF_LEAK;
    }

    // 以大端序写入
    uint8_t* b = (uint8_t*)channel->wpos;
    b[0] = (data_len >> 8) & 0xFF;
    b[1] = data_len & 0xFF;
    channel->wpos += 2;

    return 2;
}

static int blp2_read_header(xChannel* channel, size_t* data_len) {
    if (!channel || !channel->rbuf || !channel->rpos || !data_len) {
        return PACKET_FD_INVALD;
    }

    int len = (int)(channel->rpos - channel->rbuf);
    if (len < 2) {
        return PACKET_INCOMPLETE;  // 长度字段值太小
    }

    // 读取2字节长度字段(大端序)
    uint8_t* b = (uint8_t*)channel->rbuf;
    uint16_t pkg_len = (b[0] << 8) | b[1];
    if (len < pkg_len + 2) {
        return PACKET_INCOMPLETE;
    }
    // 数据长度 = 总长度 - 包头长度
    *data_len = pkg_len;
    return 2;
}

// ==================== BLP4协议实现 ====================

static xChannelErrCode blp4_check_complete(xChannel* channel) {
    if (!channel) {
        return PACKET_FD_INVALD;
    }

    int len = (int)(channel->rpos - channel->rbuf);
    if (len < 4) {
        return PACKET_INCOMPLETE;  // 数据不够读取包头
    }

    // 读取4字节长度字段(大端序)
    uint8_t* b = (uint8_t*)channel->rbuf;
    uint32_t pkg_len = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];

    if (len < (int)pkg_len + 4) {
        return PACKET_INCOMPLETE;
    }

    return PACKET_SUCCESS;
}

static int blp4_write_header(xChannel* channel, size_t data_len) {
    if (!channel || !channel->wbuf) {
        return PACKET_FD_INVALD;
    }

    // 检查缓冲区空间
    if (channel->wlen - (channel->wpos - channel->wbuf) < (int)data_len + 4) {
        return PACKET_BUF_LEAK;
    }

    // 以大端序写入
    uint8_t* b = (uint8_t*)channel->wpos;
    b[0] = (data_len >> 24) & 0xFF;
    b[1] = (data_len >> 16) & 0xFF;
    b[2] = (data_len >> 8) & 0xFF;
    b[3] = data_len & 0xFF;
    channel->wpos += 4;
    return 4;
}

static int blp4_read_header(xChannel* channel, size_t* data_len) {
    if (!channel || !channel->rbuf || !channel->rpos || !data_len) {
        return PACKET_FD_INVALD;
    }

    int len = (int)(channel->rpos - channel->rbuf);
    if (len < 4) {
        return PACKET_INCOMPLETE;  // 数据不够读取包头
    }

    // 读取4字节长度字段(大端序)
    uint8_t* b = (uint8_t*)channel->rbuf;
    uint32_t pkg_len = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];

    if (len < (int)pkg_len + 4) {
        return PACKET_INCOMPLETE;
    }

    // 数据长度 = 总长度 - 包头长度
    *data_len = pkg_len;
    return 4;
}

// ==================== RESP2协议实现 ====================

static xChannelErrCode resp2_check_complete(xChannel* channel) {
    if (!channel)
        return PACKET_FD_INVALD;
    if (redis::redis_check_complete(channel->rbuf, (int)(channel->rpos-channel->rbuf), RedisProtocol::RESP2))
        return PACKET_SUCCESS;
    return PACKET_INCOMPLETE;
}

static int resp2_write_header(xChannel* channel, size_t data_len) {
    if (!channel || !channel->wbuf) {
        return PACKET_FD_INVALD;
    }

    // 检查缓冲区空间
    if (channel->wlen - (channel->wpos - channel->wbuf) < (int)data_len + 4) {
        return PACKET_BUF_LEAK;
    }
    return 0;
}

static int resp2_read_header(xChannel* channel, size_t* data_len) {
    if (!channel || !channel->rbuf || !channel->rpos || !data_len) {
        return PACKET_FD_INVALD;
    }
    *data_len = (int)(channel->rpos - channel->rbuf);
    return 0;
}

// ==================== RESP3协议实现 ====================

static xChannelErrCode resp3_check_complete(xChannel* channel) {
    if (!channel)
        return PACKET_FD_INVALD;
    if (redis::redis_check_complete(channel->rbuf, (int)(channel->rpos - channel->rbuf), RedisProtocol::RESP3))
        return PACKET_SUCCESS;
    return PACKET_INCOMPLETE;
}

static int resp3_write_header(xChannel* channel, size_t data_len) {
    if (!channel || !channel->wbuf) {
        return PACKET_FD_INVALD;
    }

    // 检查缓冲区空间
    if (channel->wlen - (channel->wpos - channel->wbuf) < (int)data_len + 4) {
        return PACKET_BUF_LEAK;
    }
    return 0;
}

static int resp3_read_header(xChannel* channel, size_t* data_len) {
    if (!channel || !channel->rbuf || !channel->rpos || !data_len) {
        return PACKET_FD_INVALD;
    }
    *data_len = (int)(channel->rpos - channel->rbuf);
    return 0;
}

// ==================== NATS协议实现 (行基文本协议) ====================

static xChannelErrCode nats_check_complete(xChannel* channel) {
    if (!channel)
        return PACKET_FD_INVALD;

    int len = (int)(channel->rpos - channel->rbuf);
    if (len < 2) return PACKET_INCOMPLETE;

    // 查找第一个 \r\n 作为命令结束
    int cmd_end = -1;
    for (int i = 0; i < len - 1; i++) {
        if (channel->rbuf[i] == '\r' && channel->rbuf[i + 1] == '\n') {
            cmd_end = i;
            break;
        }
    }
    if (cmd_end < 0) return PACKET_INCOMPLETE;

    // 检查是否是 MSG 命令，MSG 后面还有消息体
    // MSG <subject> <sid> [reply-to] <#bytes>\r\n<payload>\r\n
    if (len >= 3 && strncasecmp(channel->rbuf, "MSG", 3) == 0) {
        // 解析字节数
        int i = 3;
        while (i < cmd_end && isspace(channel->rbuf[i])) i++;
        // 跳过 subject
        while (i < cmd_end && !isspace(channel->rbuf[i])) i++;
        while (i < cmd_end && isspace(channel->rbuf[i])) i++;
        // 跳过 sid
        while (i < cmd_end && !isspace(channel->rbuf[i])) i++;
        while (i < cmd_end && isspace(channel->rbuf[i])) i++;
        // 可能是 reply-to 或者是字节数
        int start = i;
        while (i < cmd_end && !isspace(channel->rbuf[i])) i++;
        int next = i;
        while (i < cmd_end && isspace(channel->rbuf[i])) i++;
        // 如果还有内容，说明前面是 reply-to，后面是字节数
        int num_start = start;
        if (i < cmd_end) {
            num_start = i;
            while (i < cmd_end && !isspace(channel->rbuf[i])) i++;
        }

        // 解析字节数
        int payload_len = 0;
        for (int j = num_start; j < i && isdigit(channel->rbuf[j]); j++) {
            payload_len = payload_len * 10 + (channel->rbuf[j] - '0');
        }

        // 需要 cmd_end + 2(\r\n) + payload_len + 2(\r\n)
        int total_needed = cmd_end + 2 + payload_len + 2;
        if (len < total_needed) return PACKET_INCOMPLETE;
    }

    return PACKET_SUCCESS;
}

static int nats_write_header(xChannel* channel, size_t data_len) {
    if (!channel || !channel->wbuf) {
        return PACKET_FD_INVALD;
    }
    // NATS 协议不需要包头，直接返回 0
    (void)data_len;
    return 0;
}

static int nats_read_header(xChannel* channel, size_t* data_len) {
    if (!channel || !channel->rbuf || !channel->rpos || !data_len) {
        return PACKET_FD_INVALD;
    }
    // 返回当前缓冲区中的所有数据
    *data_len = (int)(channel->rpos - channel->rbuf);
    return 0;
}

// ==================== 全局操作对象数组 ====================

const PacketOps _g_pack_ops[xproto_max] = {
    // xproto_blp2 = 0
    {
        blp2_check_complete,    // check_complete
        blp2_write_header,      // write_header
        blp2_read_header,       // read_header
        2,                      // header_size
        "BLP2"                  // proto_name
    },
    // xproto_blp4 = 1
    {
        blp4_check_complete,    // check_complete
        blp4_write_header,      // write_header
        blp4_read_header,       // read_header
        4,                      // header_size
        "BLP4"                  // proto_name
    },
    // xproto_crlf_resp2 = 2
    {
        resp2_check_complete,    // check_complete
        resp2_write_header,      // write_header
        resp2_read_header,       // read_header
        0,                       // header_size
        "RESP2"                  // proto_name
    },
    // xproto_crlf_resp3 = 3
    {
        resp3_check_complete,    // check_complete
        resp3_write_header,      // write_header
        resp3_read_header,       // read_header
        0,                       // header_size
        "RESP3"                  // proto_name
    },
    // xrpoto_crlf_http1 = 4 (used for NATS)
    {
        nats_check_complete,    // check_complete
        nats_write_header,      // write_header
        nats_read_header,       // read_header
        0,                      // header_size
        "NATS"                  // proto_name
    },
};

const PacketOps* _xchannel_get_ops(xChannel* channel) {
    if (!channel || channel->pproto >= xproto_max) {
        return NULL;
    }
    return &_g_pack_ops[channel->pproto];
}
