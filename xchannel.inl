#ifndef _XCHANNEL_INL_
#define _XCHANNEL_INL_
#include "xchannel.h"
// struct xChannel;

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

const PacketOps* _xchannel_get_ops(xChannel* channel);

/**
    * @brief 检查通道接收缓冲区中的数据包是否完整
    * @param channel 通道指针
    * @return 包完整性检测结果
    */
static inline xChannelErrCode _xchannel_check_complete(xChannel* channel) {
    const PacketOps* ops = _xchannel_get_ops(channel);
    return ops && ops->check_complete ? ops->check_complete(channel) : PACKET_SUCCESS; // default check when on pack
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

#endif // !_XCHANNEL_INL_
