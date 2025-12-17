#ifndef _XREDIS_H
#define _XREDIS_H

#include "xcoroutine.h"
#include "xpack.h"
#include <string>
#include <vector>

struct RedisConnConfig {
    std::string ip;
    int port;
    std::string password;  // 可选，为空表示不需要认证
    int db_index = 0;      // 默认数据库0
    bool use_resp3 = true; // 默认使用RESP3
};


// 初始化Redis连接池
int xredis_init(const RedisConnConfig& config, int max_conn);
int xredis_init(const std::string& ip, int port, int max_conn);
int xredis_status(int* total, int* idle, int* in_use, int* initializing);

// 销毁Redis连接池
void xredis_deinit();

// Redis字符串操作
xAwaiter xredis_set(const std::string& key, const std::string& value);
xAwaiter xredis_get(const std::string& key);

// Redis哈希操作
xAwaiter xredis_hset(const std::string& key, const std::string& field, const std::string& value);
xAwaiter xredis_hget(const std::string& key, const std::string& field);
xAwaiter xredis_hgetall(const std::string& key);

// Redis pub/sub
typedef void (*RedisSubscribeCallback)(const std::string& type, const std::string& channel, std::vector<VariantType>& resp);
xCoroTaskT<int> xredis_subscribe(const std::string& channel, RedisSubscribeCallback fnCallback);
xCoroTaskT<int> xredis_unsubscribe(const std::string& channel);
xAwaiter xredis_publish(const std::string& channel, const std::string& message);

// 通用命令接口
xAwaiter xredis_command(const std::vector<std::string>& args);

#endif // _XREDIS_H