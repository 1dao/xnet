// ae_channel.h
#ifndef _AE_CHANNEL_H
#define _AE_CHANNEL_H
#include "ae.h"

#include <stdint.h>
#include <stdbool.h>

typedef enum xProto {
    aeproto_blp2 = 0,  // 2字节长度字段
    aeproto_blp4 = 1,  // 4字节长度字段
    aeproto_max        // 协议数量
} xProto;

typedef struct xChannel {
    xSocket fd;
    int     wlen;
    char* wbuf;
    char* wpos;

    int	    rlen;
    char* rbuf;
    char* rpos;

    aeFileEvent* ev;
    xProto pproto;   // 通道协议类型
    void* userdata;         // 用户数据指针
} xChannel;

typedef int xchannel_proc(struct xChannel* s, char* buf, int len);

// 函数声明 - 添加协议类型参数
xChannel*   xchannel_conn(char* addr, int port, xchannel_proc* on_pack, xchannel_proc* on_close, void* userdata, xProto proto = xProto::aeproto_blp4);
int         xchannel_listen(int port, char* bindaddr, xchannel_proc* proc, xchannel_proc* on_close, void* userdata, xProto proto = xProto::aeproto_blp4);
int         xchannel_send(struct xChannel* s, const char* buf, int len);
int         xchannel_rawsend(struct xChannel* s, const char* buf, int len);
int         xchannel_rpc(struct xChannel* s, char* buf, int len);
int         xchannel_close(struct xChannel* s);

////////////////////////////////// xchannel pdu
//

// 包完整性检测结果
typedef enum {
    PACKET_SUCCESS = 0,    // 数据包完整
    PACKET_INCOMPLETE = -1,  // 数据包不完整，需要继续接收
    PACKET_INVALID = -2,    // 数据包无效
    PACKET_BUF_LEAK = -3,   // 数据包无效
    PACKET_FD_INVALD = -4,   // 
} xChannelErrCode;

// 包操作函数指针类型
typedef xChannelErrCode(*PacketCheckFunc)(xChannel* channel);
typedef int (*HeaderWriteFunc)(xChannel* channel, size_t data_len);
typedef int (*HeaderReadFunc)(xChannel* channel, size_t* head_len);

// 包操作对象
typedef struct {
    PacketCheckFunc check_complete;  // 包完整性检测函数
    HeaderWriteFunc write_header;    // 写包头函数
    HeaderReadFunc read_header;      // 读包头函数
    size_t header_size;             // 包头大小
    const char* proto_name;         // 协议名称
} PacketOps;

// ==================== 全局操作对象数组 ====================
extern const PacketOps _g_pack_ops[aeproto_max];

// ==================== 内联访问函数 ====================

/**
    * @brief 获取通道对应的包操作对象
    * @param channel 通道指针
    * @return 包操作对象指针
    */
static inline const PacketOps* _xchannel_get_ops(xChannel* channel) {
    if (!channel || channel->pproto >= aeproto_max) {
        return NULL;
    }
    return &_g_pack_ops[channel->pproto];
}

/**
    * @brief 检查通道接收缓冲区中的数据包是否完整
    * @param channel 通道指针
    * @return 包完整性检测结果
    */
static inline xChannelErrCode _xchannel_check_complete(xChannel* channel) {
    const PacketOps* ops = _xchannel_get_ops(channel);
    return ops && ops->check_complete ? ops->check_complete(channel) : PACKET_INVALID;
}

/**
    * @brief 将包头写入通道的发送缓冲区
    * @param channel 通道指针
    * @param data_len 数据部分长度
    * @return 写入的包头长度，0表示失败
    */
static inline int _xchannel_write_header(xChannel* channel, size_t data_len) {
    const PacketOps* ops = _xchannel_get_ops(channel);
    return ops && ops->write_header ? ops->write_header(channel, data_len) : 0;
}

/**
    * @brief 从通道的接收缓冲区读取包头
    * @param channel 通道指针
    * @param data_len [输出]数据部分长度
    * @return 读取的包头长度，0表示失败
    */
static inline int _xchannel_read_header(xChannel* channel, size_t* data_len) {
    const PacketOps* ops = _xchannel_get_ops(channel);
    return ops && ops->read_header ? ops->read_header(channel, data_len) : 0;
}

/**
    * @brief 获取当前通道协议的包头大小
    * @param channel 通道指针
    * @return 包头大小
    */
static inline size_t _xchannel_header_size(xChannel* channel) {
    const PacketOps* ops = _xchannel_get_ops(channel);
    return ops ? ops->header_size : 0;
}

/**
    * @brief 计算完整数据包的总长度（包头+数据）
    * @param channel 通道指针
    * @param data_len 数据部分长度
    * @return 完整包长度
    */
static inline size_t _xchannel_packet_size(xChannel* channel, size_t data_len) {
    return _xchannel_header_size(channel) + data_len;
}

/**
    * @brief 获取协议名称
    * @param channel 通道指针
    * @return 协议名称字符串
    */
static inline const char* _xchannel_get_proto_name(xChannel* channel) {
    const PacketOps* ops = _xchannel_get_ops(channel);
    return ops ? ops->proto_name : "UNKNOWN";
}

#endif