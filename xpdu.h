#ifndef _XPDU_H
#define _XPDU_H

#include <stdint.h>
#include <stddef.h>
#include "xchannel.h"

// 协议枚举从0开始，用于数组下标
typedef enum xChannelProto {
    aeproto_blp2 = 0,  // 2字节长度字段
    aeproto_blp4 = 1,  // 4字节长度字段
    aeproto_max        // 协议数量
} xChannelProto;

// 包完整性检测结果
typedef enum {
    PACKET_INCOMPLETE = 0,  // 数据包不完整，需要继续接收
    PACKET_COMPLETE = 1,    // 数据包完整
    PACKET_INVALID = -1,    // 数据包无效
} PacketCheckResult;

// 包操作函数指针类型
typedef PacketCheckResult(*PacketCheckFunc)(xChannel* channel);
typedef size_t(*HeaderWriteFunc)(xChannel* channel);
typedef size_t(*HeaderReadFunc)(xChannel* channel);

// 包操作对象
typedef struct {
    PacketCheckFunc check_complete;  // 包完整性检测函数
    HeaderWriteFunc write_header;    // 写包头函数
    HeaderReadFunc read_header;      // 读包头函数
    size_t header_size;             // 包头大小
    const char* proto_name;         // 协议名称
} PacketOps;


// ==================== 全局操作对象数组 ====================
extern const PacketOps g_packet_ops[aeproto_max];

// ==================== 内联访问函数 ====================

/**
 * @brief 获取通道对应的包操作对象
 * @param channel 通道指针
 * @return 包操作对象指针
 */
static inline const PacketOps* channel_get_ops(xChannel* channel) {
    if (!channel || channel->pproto >= aeproto_max) {
        return NULL;
    }
    return &g_packet_ops[channel->pproto];
}

/**
 * @brief 检查通道接收缓冲区中的数据包是否完整
 * @param channel 通道指针
 * @return 包完整性检测结果
 */
static inline PacketCheckResult channel_check_complete(xChannel* channel) {
    const PacketOps* ops = channel_get_ops(channel);
    return ops && ops->check_complete ? ops->check_complete(channel) : PACKET_INVALID;
}

/**
 * @brief 将包头写入通道的发送缓冲区
 * @param channel 通道指针
 * @param data_len 数据部分长度
 * @return 写入的包头长度，0表示失败
 */
static inline size_t channel_write_header(xChannel* channel, size_t data_len) {
    const PacketOps* ops = channel_get_ops(channel);
    return ops && ops->write_header ? ops->write_header(channel, data_len) : 0;
}

/**
 * @brief 从通道的接收缓冲区读取包头
 * @param channel 通道指针
 * @param data_len [输出]数据部分长度
 * @return 读取的包头长度，0表示失败
 */
static inline size_t channel_read_header(xChannel* channel, size_t* data_len) {
    const PacketOps* ops = channel_get_ops(channel);
    return ops && ops->read_header ? ops->read_header(channel, data_len) : 0;
}

/**
 * @brief 获取当前通道协议的包头大小
 * @param channel 通道指针
 * @return 包头大小
 */
static inline size_t channel_get_header_size(xChannel* channel) {
    const PacketOps* ops = channel_get_ops(channel);
    return ops ? ops->header_size : 0;
}

/**
 * @brief 计算完整数据包的总长度（包头+数据）
 * @param channel 通道指针
 * @param data_len 数据部分长度
 * @return 完整包长度
 */
static inline size_t channel_calc_packet_size(xChannel* channel, size_t data_len) {
    return channel_get_header_size(channel) + data_len;
}

/**
 * @brief 获取协议名称
 * @param channel 通道指针
 * @return 协议名称字符串
 */
static inline const char* channel_get_proto_name(xChannel* channel) {
    const PacketOps* ops = channel_get_ops(channel);
    return ops ? ops->proto_name : "UNKNOWN";
}

#endif