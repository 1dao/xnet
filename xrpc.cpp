#include "xrpc.h"
#include "ae.h"
#include "xcoroutine.h"
#include <iostream>
#include "xpack.h"
#include "xchannel.inl"

int _xrpc_resp(xChannel* s, int co_id, uint32_t wait_id, int retcode, XPackBuff& res) {
    uint16_t is_rpc = 2;
    int remain = (int)s->wlen - (int)(s->wpos - s->wbuf);
    int hlen = (int)_xchannel_header_size(s);
    // 增加 sizeof(retcode)
    int plen = sizeof(is_rpc) + sizeof(wait_id) + sizeof(co_id) + sizeof(retcode) + res.len;

    if (remain < hlen + plen) {
        std::cout << "xrpc_resp: Buffer overflow" << std::endl;
        return XNET_BUFF_LIMIT;
    }

    // 写包头
    _xchannel_write_header(s, plen);

    // 写协议头
    *(uint16_t*)s->wpos = htons(is_rpc);
    s->wpos += sizeof(is_rpc);
    *(uint32_t*)s->wpos = htonl(wait_id);
    s->wpos += sizeof(wait_id);
    *(int*)s->wpos = htonl(co_id);
    s->wpos += sizeof(co_id);

    // 写执行结果
    *(int*)s->wpos = htonl(retcode);
    s->wpos += sizeof(retcode);

    // 写数据包
    return xchannel_rawsend(s, res.get(), res.len);
}
