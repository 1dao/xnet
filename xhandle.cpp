#include "xhandle.h"
#include <unordered_map>
#include <stdexcept>
#include "xlog.h"
#include "xpack.h"
#include "xrpc_template.h"

//定义一个全局变量，存储handle
std::unordered_map<int, ProtocolPostHandler> _handles_post;
std::unordered_map<int, ProtocolRPCHandler> _handles_rpc;

void xhandle_reg_post(int pt, ProtocolPostHandler handler) {
    // 判断是否已经注册过
    if (_handles_post.find(pt) != _handles_post.end()) {
        // 如果已经注册过，抛出异常
        throw std::runtime_error("Protocol already registered");
    }

    _handles_post[pt] = handler;
}

void xhandle_reg_rpc(int pt, ProtocolRPCHandler handler) {
    if (_handles_rpc.find(pt) != _handles_rpc.end()) {
        // 如果已经注册过，抛出异常
        throw std::runtime_error("Protocol already registered");
    }
    _handles_rpc[pt] = handler;
}

int xhandle_invoke_post(struct xChannel* s, int pt, std::vector<VariantType>& args) {
    auto it = _handles_post.find(pt);
    if (it == _handles_post.end()) {
        // 如果没有找到对应的handle，抛出异常
        throw std::runtime_error("Protocol not found");
    }

    // 调用handle
    return it->second(s, (args));
}

XPackBuff&& xhandle_invoke_rpc(struct xChannel* s, int pt, std::vector<VariantType>& args){
    auto it = _handles_rpc.find(pt);
    if (it == _handles_rpc.end()) {
        // 如果没有找到对应的handle，抛出异常
        throw std::runtime_error("Protocol not found");
    }

    // 调用handle
    return it->second(s, (args));
}

int xhandle_on_pack(xChannel* s, char* buf, int len) {
    // 解析包头
    uint16_t is_rpc = 1;
    uint32_t pkg_id = 0;
    int co_id = 0;
    uint16_t protocol = 0;
    char* cur = s->rbuf+_xchannel_header_size(s);
    is_rpc = ntohs(*(uint16_t*)cur);
    cur += sizeof(is_rpc);
    if (is_rpc == 0) {
        // 如果不是RPC包，直接返回
        protocol = ntohs(*(uint16_t*)cur);
        cur += sizeof(protocol);
        auto it = _handles_post.find(protocol);
        if (it == _handles_post.end()) {
            xlog_err("Protocol not found");
        } else {
            std::vector<VariantType> args = xpack_unpack(cur, len);
            it->second(s, args);
        }
        return len;
    } else if(is_rpc==2) {
        pkg_id = ntohl(*(uint32_t*)cur);
        cur += sizeof(pkg_id);
        co_id = ntohl(*(int*)cur);
        cur += sizeof(co_id);
        std::vector<VariantType> args = xpack_unpack(cur, len);
        coroutine_resume(co_id, &args);
    } else if(is_rpc==1) {
        pkg_id = ntohl(*(uint32_t*)cur);
        cur += sizeof(pkg_id);
        co_id = ntohl(*(int*)cur);
        cur += sizeof(co_id);
        protocol = ntohs(*(uint16_t*)cur);
        cur += sizeof(protocol);

        auto it = _handles_rpc.find(protocol);
        if (it == _handles_rpc.end()) {
            xlog_err("Protocol not found");
        } else {
            std::vector<VariantType> args = xpack_unpack(cur, len);
            XPackBuff res = it->second(s, args);
            xrpc_resp(s, co_id, pkg_id, res);
        }
    }
    return len;
}
