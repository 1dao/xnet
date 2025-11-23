#include <string.h>
#include "xchannel.h"
#include "xchannel.inl"

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
    if (channel->wlen - (channel->wpos - channel->wbuf) < data_len + 2) {
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

    if (len < pkg_len + 4) {
        return PACKET_INCOMPLETE;
    }

    return PACKET_SUCCESS;
}

static int blp4_write_header(xChannel* channel, size_t data_len) {
    if (!channel || !channel->wbuf) {
        return PACKET_FD_INVALD;
    }

    // 检查缓冲区空间
    if (channel->wlen - (channel->wpos - channel->wbuf) < data_len + 4) {
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

    if (len < pkg_len + 4) {
        return PACKET_INCOMPLETE;
    }

    // 数据长度 = 总长度 - 包头长度
    *data_len = pkg_len;
    return 4;
}

// ==================== 全局操作对象数组 ====================

const PacketOps _g_pack_ops[aeproto_max] = {
    // aeproto_blp2 = 0
    {
        blp2_check_complete,    // check_complete
        blp2_write_header,      // write_header
        blp2_read_header,       // read_header
        2,                      // header_size
        "BLP2"                  // proto_name
    },
    // aeproto_blp4 = 1
    {
        blp4_check_complete,    // check_complete
        blp4_write_header,      // write_header
        blp4_read_header,       // read_header
        4,                      // header_size
        "BLP4"                  // proto_name
    }
};

const PacketOps* _xchannel_get_ops(xChannel* channel) {
    if (!channel || channel->pproto >= aeproto_max) {
        return NULL;
    }
    return &_g_pack_ops[channel->pproto];
}