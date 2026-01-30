#ifndef XHANDLE_H
#define XHANDLE_H

#include <vector>
#include "xpack.h"
#include "xchannel.h"
#include "xerrno.h"

// 定义协议处理函数类型
typedef int (*ProtocolPostHandler)(xChannel* s, std::vector<VariantType>& args);
typedef XPackBuff (*ProtocolRPCHandler)(xChannel* s, std::vector<VariantType>& args);

// 注册函数
void xhandle_reg_post(int pt, ProtocolPostHandler h);
void xhandle_reg_rpc(int pt, ProtocolRPCHandler h);

// 包处理函数
int xhandle_on_pack(xChannel* s, char* buf, int len);

#endif // XHANDLE_H
