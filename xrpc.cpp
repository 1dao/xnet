#include "xrpc.h"
#include "ae.h"
#include "xcoroutine.h"
#include <iostream>
#include "xpack.h"
#include "xchannel.inl"

bool xrpc_resp_blp4(xChannel* channel) {
    // 检查是否有足够数据读取包长度
    if (static_cast<int>(channel->rpos - channel->rbuf) < 4)
        return false;

    uint32_t net_pkg_len = *reinterpret_cast<uint32_t*>(channel->rbuf);
    uint32_t pkg_len = ntohl(net_pkg_len);

    // 检查是否收到完整包
    if (static_cast<int>(channel->rpos - channel->rbuf) < static_cast<int>(pkg_len + 4))
        return false;

    // 解析协议头
    uint16_t net_protocol = *reinterpret_cast<uint16_t*>(channel->rbuf + 4);
    uint16_t protocol = ntohs(net_protocol);

    uint16_t net_request = *reinterpret_cast<uint16_t*>(channel->rbuf + 6);
    uint16_t request = ntohs(net_request);

    uint32_t net_pkg_id = *reinterpret_cast<uint32_t*>(channel->rbuf + 8);
    uint32_t pkg_id = ntohl(net_pkg_id);

    // 处理响应包
    if (request == 0 && pkg_id > 0) {
        // 提取响应数据（跳过头部：4字节长度 + 8字节协议头）
        char* response_data = channel->rbuf + 12; // 4(len) + 2(protocol) + 2(request) + 4(pkg_id)
        int response_len = static_cast<int>(pkg_len) - 8; // 减去协议头长度

        if (response_len < 0) {
            std::cout << "xrpc_check_resp: Invalid response length: " << response_len << std::endl;
            return false;
        }

        //// 创建 XPackBuff
        //XPackBuff result(response_data, response_len);

        //// --- 新: 直接交给协程管理器，让对应协程 resume ---
        //xcoroutine_rpc::resume_rpc(pkg_id, std::move(result));

        // 移动缓冲区
        int total_packet_size = static_cast<int>(pkg_len) + 4;
        int data_remaining = static_cast<int>(channel->rpos - channel->rbuf) - total_packet_size;

        if (data_remaining > 0) {
            std::memmove(channel->rbuf, channel->rbuf + total_packet_size, data_remaining);
        }
        channel->rpos = channel->rbuf + data_remaining;

        std::cout << "xrpc_check_resp: Completed RPC, pkg_id: " << pkg_id
            << ", data_len: " << response_len << std::endl;
        return true;
    }
    return false;
}

int xrpc_resp(xChannel* s, int co_id, uint32_t pkg_id, XPackBuff& res) {
    uint16_t is_rpc = 2;
    char* wpos = s->wpos;
    int remain = (int)s->wlen - (int)(s->wpos - s->wbuf);
    int hlen = (int)_xchannel_header_size(s);
    int plen = (res.len + sizeof(pkg_id) + sizeof(co_id) + sizeof(is_rpc));
    if (remain < hlen + plen) {
        // 如果缓冲区空间不足，直接返回一个错误 awaiter
        std::cout << "xrpc_resp: Buffer overflow" << std::endl;
        return XNET_BUFF_LIMIT;
    }

    // 写头
    _xchannel_write_header(s, plen);
    *(uint16_t*)s->wpos = (uint16_t)htons(is_rpc);
    s->wpos += sizeof(is_rpc);
    *(uint32_t*)s->wpos = (uint32_t)htonl(pkg_id);
    s->wpos += sizeof(pkg_id);
    *(int*)s->wpos = (int)htonl(co_id);
    s->wpos += sizeof(co_id);
    return xchannel_rawsend(s, res.get(), res.len);
}
