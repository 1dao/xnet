#include "xredis.h"
#include "xchannel.h"
#include "xpack_redis.h"
#include "xthread.h"
#include "ae.h"
#include "xlog.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <atomic>
#include <list>
#include <memory>
#include "xpack.h"
#include "xqueue.h"

struct RedisConn {
    xChannel* channel = nullptr;
    RedisConnConfig config;
    bool ready = false;
    bool in_use = false;
    void* pool = nullptr;
    xCircleQueue queue;

    RedisConn(const RedisConnConfig& cfg) : config(cfg) {}
    ~RedisConn() {
        if (channel) {
            xchannel_close(channel);
            channel = nullptr;
            xqueue_circle_uninit(&queue);
        }
    }

    int handle_packet(char* buf, int len);
};

struct RedisPool {
    RedisConnConfig config;
    int max_conn = 0;
    std::list<RedisConn*> free_conns;
    std::list<RedisConn*> busy_conns;
    std::condition_variable cv;
    bool initialized = false;
    int total_created = 0;
    int initializing = 0;

    std::unordered_map<std::string, RedisConn*> subscribe_conns;
    std::unordered_map<std::string, RedisSubscribeCallback> callbacks;
    std::unordered_set<std::string> pending_unsubscribe;
};

#ifdef _WIN32
static _declspec(thread) RedisPool* _pool = NULL;
#else
static __thread RedisPool*          _pool = NULL;
#endif

static VariantType conv_value(const RedisObject& val);

// 判断是否为模式订阅（包含通配符）
inline bool is_pattern(const std::string& str) {
    return str.find('*') != std::string::npos ||
        str.find('?') != std::string::npos ||
        str.find('[') != std::string::npos;
}

// 简单的通配符匹配函数
static bool pattern_match(const std::string& channel, const std::string& pattern) {
    // 完全匹配
    if (pattern == "*") return true;

    // 没有通配符，直接比较
    if (pattern.find('*') == std::string::npos &&
        pattern.find('?') == std::string::npos &&
        pattern.find('[') == std::string::npos) {
        return channel == pattern;
    }

    // 简化实现，只处理*通配符
    if (pattern.find('*') == std::string::npos) {
        return channel == pattern;
    }

    // 处理前缀*后缀模式
    if (pattern.front() == '*' && pattern.back() == '*') {
        std::string middle = pattern.substr(1, pattern.length() - 2);
        return channel.find(middle) != std::string::npos;
    }
    // 处理前缀*模式
    else if (pattern.front() == '*') {
        std::string suffix = pattern.substr(1);
        if (channel.length() >= suffix.length()) {
            return channel.substr(channel.length() - suffix.length()) == suffix;
        }
    }
    // 处理*后缀模式
    else if (pattern.back() == '*') {
        std::string prefix = pattern.substr(0, pattern.length() - 1);
        return channel.length() >= prefix.length() &&
            channel.substr(0, prefix.length()) == prefix;
    }
    // 处理中间*模式
    else {
        size_t star_pos = pattern.find('*');
        std::string prefix = pattern.substr(0, star_pos);
        std::string suffix = pattern.substr(star_pos + 1);

        if (channel.length() < prefix.length() + suffix.length()) {
            return false;
        }

        return channel.substr(0, prefix.length()) == prefix &&
            channel.substr(channel.length() - suffix.length()) == suffix;
    }

    return false;
}

// 订阅关系枚举
enum SubscriptionRelation {
    NONE,           // 无覆盖关系
    A_COVERS_B,     // A覆盖B
    B_COVERS_A,     // B覆盖A
    IDENTICAL       // 完全相同
};

// 检查两个订阅是否有覆盖关系
static SubscriptionRelation check_subscription_relation(const std::string& a, const std::string& b) {
    bool a_is_pattern = is_pattern(a);
    bool b_is_pattern = is_pattern(b);

    // 完全相同
    if (a == b) {
        return SubscriptionRelation::IDENTICAL;
    }

    // 都是频道或都是模式，无覆盖关系
    if (a_is_pattern == b_is_pattern) {
        return SubscriptionRelation::NONE;
    }

    // 一个是频道，一个是模式
    std::string pattern = a_is_pattern ? a : b;
    std::string channel = a_is_pattern ? b : a;

    // 检查模式是否匹配频道
    if (pattern_match(channel, pattern)) {
        return a_is_pattern ? SubscriptionRelation::A_COVERS_B : SubscriptionRelation::B_COVERS_A;
    }

    return SubscriptionRelation::NONE;
}

static void release_connection(RedisConn* conn);
void handle_unsubscribe_confirm(const std::string& channel_or_pattern) {
    // 检查是否在待取消列表中
    auto pending_it = _pool->pending_unsubscribe.find(channel_or_pattern);
    if (pending_it == _pool->pending_unsubscribe.end()) {
        return;  // 不是我们发起的取消订阅
    }

    _pool->pending_unsubscribe.erase(pending_it);

    // 清理服务器订阅映射
    auto conn_it = _pool->subscribe_conns.find(channel_or_pattern);
    if (conn_it != _pool->subscribe_conns.end()) {
        RedisConn* conn = conn_it->second;
        _pool->subscribe_conns.erase(conn_it);

        // 检查连接是否还有其他订阅
        bool has_other_subscriptions = false;
        for (const auto& [key, c] : _pool->subscribe_conns) {
            if (c == conn) {
                has_other_subscriptions = true;
                break;
            }
        }

        // 如果没有其他订阅，释放连接
        if (!has_other_subscriptions) {
            conn->ready = true;
            release_connection(conn);
        }
    }

    xlog_info("Unsubscribe confirmed for: %s", channel_or_pattern.c_str());
}

int RedisConn::handle_packet(char* buf, int len) {
    xlog_debug("RedisConn::handle_packet:%.*s", len, buf);

    try {
        std::vector<RedisObject> objs = redis::redis_unpack(buf, len,
            config.use_resp3 ? RedisProtocol::RESP3 : RedisProtocol::RESP2);
        if (objs.size() == 0) {
            xlog_err("xRedis invalid response:%s", std::string(buf, len).c_str());
            return len;
        }
        
        if (!objs[0].is_push()) {
            if (!ready) { // auth select hello
                std::vector<VariantType> result;
                std::map<std::string, std::string> server_infos;
                for (const auto& obj : objs) {
                    result.push_back(conv_value(obj));
                }
                uint32_t wait_id = 0;
                if (xqueue_circle_dequeue(&queue, &wait_id) == 0) {
                    xlog_err("xRedis invalid response:%s", std::string(buf, len).c_str());
                    return len;
                }
                coroutine_resume(wait_id, std::move(result));
                return len;
            }

            uint32_t wait_id = 0;
            if (xqueue_circle_dequeue(&queue, &wait_id) == 0) {
                xlog_err("xRedis invalid response:%s", std::string(buf, len).c_str());
                return len;
            }
            std::vector<VariantType> result;
            result.push_back(0);
            for (const auto& obj : objs) {
                if (obj.is_array()) {
                    const auto& arr = obj.get_array();
                    for (size_t i = 1; i < arr.size(); ++i) {
                        result.push_back(conv_value(arr[i]));
                    }
                }
                else {
                    result.push_back(conv_value(obj));
                }
            }
            xlog_debug("Redis on recv conn(%d) wait_id(%d) command: %s", (int)channel->fd, wait_id, std::string(buf, len).c_str());
            coroutine_resume(wait_id, std::move(result));
        } else { 
            const auto& push_data = objs[0].get_array();
            if (push_data.size() >= 3) {
                std::string type = push_data[0].get_string();
                std::string channel = push_data[1].get_string();

                if(type == "subscribe" || type == "psubscribe") {
                    uint32_t wait_id = 0;
                    if (xqueue_circle_dequeue(&queue, &wait_id) == 0) {
                        xlog_err("xRedis invalid response:%s", std::string(buf, len).c_str());
                        return len;
                    }
                    std::vector<VariantType> result;
                    result.push_back(0);
                    result.push_back(conv_value(push_data[2]));
                    xlog_debug("subscribe confirmed for: waiter=%d, channel=%s", wait_id, channel.c_str());
                    coroutine_resume(wait_id, std::move(result));
                }  else if (type == "unsubscribe" || type == "punsubscribe") {
                    handle_unsubscribe_confirm(channel);
                    uint32_t wait_id = 0;
                    if (xqueue_circle_dequeue(&queue, &wait_id) == 0) {
                        xlog_err("xRedis invalid response:%s", std::string(buf, len).c_str());
                        return len;
                    }
                    std::vector<VariantType> result;
                    result.push_back(0);
                    result.push_back(conv_value(push_data[2]));
                    xlog_debug("Unsubscribe confirmed for: waiter=%d, channel=%s", wait_id, channel.c_str());
                    coroutine_resume(wait_id, std::move(result));
                    return len;
                } else if (type == "message" || type == "pmessage") {
                    // 找到所有匹配的订阅
                    std::vector<std::string> matched_keys;

                    // 1. 查找服务器订阅
                    for (const auto& [key, conn] : _pool->subscribe_conns) {
                        if (conn == this) {
                            bool key_is_pattern = is_pattern(key);  // 直接判断

                            if (!key_is_pattern && key == channel) {
                                matched_keys.push_back(key);
                            }
                            else if (key_is_pattern && pattern_match(channel, key)) {
                                matched_keys.push_back(key);
                            }
                        }
                    }

                    // 2. 查找被本地覆盖的订阅
                    for (const auto& [key, callback] : _pool->callbacks) {
                        if (_pool->subscribe_conns.find(key) == _pool->subscribe_conns.end()) {
                            // 这是一个被覆盖的订阅
                            bool key_is_pattern = is_pattern(key);

                            if (!key_is_pattern && key == channel) {
                                matched_keys.push_back(key);
                            }
                            else if (key_is_pattern && pattern_match(channel, key)) {
                                matched_keys.push_back(key);
                            }
                        }
                    }

                    // 调用所有匹配的回调
                    for (const auto& key : matched_keys) {
                        auto callback_it = _pool->callbacks.find(key);
                        if (callback_it != _pool->callbacks.end()) {
                            std::vector<VariantType> result;
                            for (size_t i = 2; i < push_data.size(); ++i) {
                                result.push_back(conv_value(push_data[i]));
                            }
                            callback_it->second(type, key, result);
                        }
                    }
                }
            }
        }
        return len;
    } catch (const std::exception& e) {
        xlog_err("Error processing Redis packet: %s", e.what());
        return -1;
    }
}

static xCoroTaskT<bool> async_init_connection(RedisConn* conn) {
    if (!conn || !conn->channel) {
        co_return false;
    }
    xlog_info("Starting async initialization for Redis connection");

    if (conn->config.use_resp3) {
        xAwaiter awaiter;
        std::vector<RedisObject> hello_cmd;
        hello_cmd.emplace_back(RedisObject::Bulk("HELLO"));
        hello_cmd.emplace_back(RedisObject::Bulk("3"));
        RedisObject hello_obj = RedisObject::Array(hello_cmd);
        std::string hello_data = redis::redis_pack(hello_obj, RedisProtocol::RESP3);

        if (xchannel_rawsend(conn->channel, hello_data.data(), hello_data.size()) != hello_data.size()) {
            xlog_err("Failed to send HELLO command");
            co_return false;
        }

        uint32_t wait_id = awaiter.wait_id();
        xqueue_circle_enqueue(&conn->queue, &wait_id);

        auto result = co_await awaiter;
        if (result.empty()) {
            xlog_err("HELLO command failed: empty response");
            co_return false;
        }

        auto opt = xpack_cast_optional<std::map<std::string, std::string>>(result, 0);
        if (opt) {
            for (const auto& [key, value] : *opt) {
                xlog_info("redis server info key-value pair: %s = %s", key.c_str(), value.c_str());
            }
        }
        xlog_info("HELLO command successful");
    }

    if (!conn->config.password.empty()) {
        xAwaiter awaiter;
        std::vector<RedisObject> auth_cmd;
        auth_cmd.emplace_back(RedisObject::Bulk("AUTH"));
        auth_cmd.emplace_back(RedisObject::Bulk(conn->config.password));
        RedisObject auth_obj = RedisObject::Array(auth_cmd);
        std::string auth_data = redis::redis_pack(auth_obj,
            conn->config.use_resp3 ? RedisProtocol::RESP3 : RedisProtocol::RESP2);

        if (xchannel_rawsend(conn->channel, auth_data.data(), auth_data.size()) != auth_data.size()) {
            xlog_err("Failed to send AUTH command");
            co_return false;
        }
        uint32_t wait_id = awaiter.wait_id();
        xqueue_circle_enqueue(&conn->queue, &wait_id);

        auto result = co_await awaiter;
        if (result.size() < 1) {
            xlog_err("AUTH command failed: insufficient response");
            co_return false;
        }

        const auto& resp = result[0];
        if (std::holds_alternative<std::string>(resp)) {
            if (std::get<std::string>(resp) != "OK") {
                xlog_err("AUTH command failed: %s", std::get<std::string>(resp).c_str());
                co_return false;
            }
        } else {
            xlog_err("AUTH command failed: invalid response type");
            co_return false;
        }

        xlog_info("AUTH command successful");
    }

    if (conn->config.db_index != 0) {
        xAwaiter awaiter;
        std::vector<RedisObject> select_cmd;
        select_cmd.emplace_back(RedisObject::Bulk("SELECT"));
        select_cmd.emplace_back(RedisObject::Bulk(std::to_string(conn->config.db_index)));
        RedisObject select_obj = RedisObject::Array(select_cmd);
        std::string select_data = redis::redis_pack(select_obj,
            conn->config.use_resp3 ? RedisProtocol::RESP3 : RedisProtocol::RESP2);

        if (xchannel_rawsend(conn->channel, select_data.data(), select_data.size()) != select_data.size()) {
            xlog_err("Failed to send SELECT command");
            co_return false;
        }

        uint32_t wait_id = awaiter.wait_id();
        xqueue_circle_enqueue(&conn->queue, &wait_id);
        auto result = co_await awaiter;
        if (result.size() ==0) {
            xlog_err("SELECT command failed: insufficient response");
            co_return false;
        }

        const auto& resp = result[0];
        if (std::holds_alternative<std::string>(resp)) {
            if (std::get<std::string>(resp) != "OK") {
                xlog_err("SELECT command failed: %s", std::get<std::string>(resp).c_str());
                co_return false;
            }
        } else {
            xlog_err("SELECT command failed: invalid response type");
            co_return false;
        }

        xlog_info("SELECT command successful, database %d selected", conn->config.db_index);
    }

    conn->ready = true;
    _pool->free_conns.push_back(conn);
    xlog_info("Redis connection initialization completed successfully");
    co_return true;
}

static xCoroTask create_and_init_connection(void*) {
    if (!_pool) co_return;

    if (_pool->total_created >= _pool->max_conn) {
        co_return;
    }

    RedisConn* conn = new RedisConn(_pool->config);
    conn->pool = _pool;
    xqueue_circle_init(&conn->queue, sizeof(uint32_t), 1000);

    conn->channel = xchannel_conn(
        const_cast<char*>(_pool->config.ip.c_str()),
        _pool->config.port,
        [](xChannel* s, char* buf, int len) {
            RedisConn* conn = static_cast<RedisConn*>(s->userdata);
            return conn->handle_packet(buf, len);
        },
        [](xChannel* s, char* buf, int len) {
            RedisConn* conn = static_cast<RedisConn*>(s->userdata);
            RedisPool* pool = (RedisPool*)conn->pool;
            pool->free_conns.remove(conn);
            pool->busy_conns.remove(conn);
            if (!conn->ready) {
                conn->ready = false;
                delete conn;
                xlog_err("Redis connection closed");
            }
            return 0;
        },
        conn,
        _pool->config.use_resp3 ? xproto_crlf_resp3 : xproto_crlf_resp2
    );

    if (!conn->channel) {
        xlog_err("Failed to create Redis connection");
        delete conn;
        co_return;
    }

    _pool->initializing++;

    bool init_success = false;
    try {
        xlog_info("Starting initialization for connection %p", conn);
        init_success = co_await async_init_connection(conn);
    } catch (const std::exception& e) {
        xlog_err("Exception during connection initialization: %s", e.what());
        init_success = false;
    }
    _pool->initializing--;

    if (init_success && conn->ready) {
        _pool->free_conns.push_back(conn);
        xlog_warn("New Redis connection created and initialized, total: %d", _pool->total_created);
        co_return;
    } else {
        xlog_err("Failed to initialize Redis connection");
        delete conn;
        _pool->total_created--;
        co_return;
    }
}

int xredis_init(const RedisConnConfig& config, int max_conn) {
    if (_pool) {
        xlog_warn("Redis pool already initialized");
        return -1;
    }

    if (max_conn <= 0) {
        xlog_err("Invalid max_conn: %d", max_conn);
        return -1;
    }

    _pool = new RedisPool;
    _pool->config = config;
    _pool->max_conn = max_conn;
    _pool->initialized = true;

    xlog_warn("Initializing Redis pool: %s:%d, max_conn=%d, use_resp3=%s",
        config.ip.c_str(), config.port, max_conn,
        config.use_resp3 ? "true" : "false");

    for (int i = 0; i < max_conn; ++i) {
        coroutine_run(create_and_init_connection, nullptr);
    }
    return 0;
}

int xredis_init(const std::string& ip, int port, int max_conn) {
    RedisConnConfig config;
    config.ip = ip;
    config.port = port;
    config.use_resp3 = true;

    return xredis_init(config, max_conn);
}

void xredis_deinit() {
    if (!_pool) return;
    xlog_warn("Deinitializing Redis pool");

    auto close_all = [](std::list<RedisConn*>& conn_list) {
        for (auto conn : conn_list) {
            delete conn;
        }
        conn_list.clear();
    };

    close_all(_pool->free_conns);
    close_all(_pool->busy_conns);

    delete _pool;
    _pool = nullptr;
}

static inline RedisConn* fetch_free_conn() {
    if(_pool->free_conns.empty()) return nullptr;
    RedisConn* conn = _pool->free_conns.front();
    _pool->free_conns.pop_front();

    if (conn->ready) {
        conn->in_use = true;
        _pool->busy_conns.push_back(conn);
        return conn;
    }
    return nullptr;
}

static void release_connection(RedisConn* conn) {
    if (!conn || !_pool) return;
    _pool->busy_conns.remove(conn);

    if (conn->ready) {
        _pool->free_conns.push_back(conn);
        conn->in_use = false;
        _pool->cv.notify_one();
    } else {
        delete conn;
        _pool->total_created--;
    }
}

struct xConnGuard {
    RedisConn* conn = nullptr;
    ~xConnGuard() {
        if (conn) {
            release_connection(conn);
        }
    }
};

xCoroTaskT<bool> wait_void(RedisConn* conn ) {
    xAwaiter awaiter;
    uint32_t wait_id = awaiter.wait_id();
    xqueue_circle_enqueue(&conn->queue, &wait_id);
    auto result = co_await awaiter;
    int error_code = xpack_cast<int>(result[0]);
    int redis_resp = xpack_cast<long long>(result[1]);

    co_return error_code==0 && redis_resp==0;
}

xAwaiter xredis_command(const std::vector<std::string>& args) {
    RedisConn* conn = fetch_free_conn();
    if (!_pool) return xAwaiter(XNET_REDIS_NOT_INIT);
    if (!conn){
        int retry = 5;
        while (!conn) {
            coroutine_sleep(100);
            conn = fetch_free_conn();
            if ((--retry) == 0) break;
        }
        if (_pool->total_created < _pool->max_conn) {
            // TODO : create new connection
        } else {

        }
    }
    if (!conn) {
        return xAwaiter(XNET_REDIS_CONNECT);
    }

    xAwaiter awaiter;
    uint32_t wait_id = awaiter.wait_id();
    xConnGuard guard{ conn };
    try {
        std::vector<RedisObject> cmd_args;
        for (const auto& arg : args) {
            cmd_args.emplace_back(RedisObject::Bulk(arg));
        }
        RedisObject cmd = RedisObject::Array(cmd_args);
        std::string resp_data;
        if (conn->config.use_resp3) {
            // TODO : maybe need attach attr
            resp_data = redis::redis_pack(cmd, RedisProtocol::RESP3);
        } else {
            resp_data = redis::redis_pack(cmd, RedisProtocol::RESP2);
        }

        // 发送命令
        int send_len = xchannel_rawsend(conn->channel, resp_data.data(), resp_data.size());
        if (send_len != (int)resp_data.size()) {
            xlog_err("Failed to send Redis command, sent %d of %d bytes", send_len, resp_data.size());
            return xAwaiter(XNET_REDIS_SEND);
        }
        xlog_debug("Sending Redis conn(%d) wait_id(%d) command: %s", (int)conn->channel->fd, wait_id, resp_data.c_str());
        xqueue_circle_enqueue(&conn->queue, &wait_id);
        return awaiter;
    }
    catch (const std::exception& e) {
        xlog_err("Exception in xredis_command: %s", e.what());
        return xAwaiter(XNET_REDIS_ERROR);
    }
}

// RedisObject转VariantType
static VariantType conv_value(const RedisObject& obj) {
    switch (obj.type()) {
    case RedisType::SimpleString:
    case RedisType::Error:
    case RedisType::BulkString:
    case RedisType::BigNumber:
        return obj.get_string();
    case RedisType::Integer:
        return static_cast<long long>(obj.get_integer());
    case RedisType::Boolean:
        return obj.get_boolean();
    case RedisType::Double:
        return obj.get_double();
    case RedisType::Array:
    case RedisType::Push: {
        std::vector<std::string> arr;
        const auto& obj_array = obj.get_array();
        for (const auto& item : obj_array) {
            auto variant_item = conv_value(item);
            if (std::holds_alternative<std::string>(variant_item)) {
                arr.push_back(std::get<std::string>(variant_item));
            } else if (std::holds_alternative<long long>(variant_item)) {
                arr.push_back(std::to_string(std::get<long long>(variant_item)));
            } else if (std::holds_alternative<double>(variant_item)) {
                arr.push_back(std::to_string(std::get<double>(variant_item)));
            } else if (std::holds_alternative<bool>(variant_item)) {
                arr.push_back(std::get<bool>(variant_item) ? "true" : "false");
            } else {
                arr.push_back("(unknown type)");
            }
        }
        return arr;
    }
    case RedisType::Map:
    case RedisType::Attribute: {
        std::map<std::string, std::string> res;
        const auto& map = obj.get_map();
        for (const auto& [k, v] : map) {
            auto key_variant = conv_value(k);
            auto value_variant = conv_value(v);

            std::string key_str;
            if (std::holds_alternative<std::string>(key_variant)) {
                key_str = std::get<std::string>(key_variant);
            } else if (std::holds_alternative<long long>(key_variant)) {
                key_str = std::to_string(std::get<long long>(key_variant));
            } else {
                key_str = "(invalid key)";
            }

            std::string value_str;
            if (std::holds_alternative<std::string>(value_variant)) {
                value_str = std::get<std::string>(value_variant);
            } else if (std::holds_alternative<long long>(value_variant)) {
                value_str = std::to_string(std::get<long long>(value_variant));
            } else if (std::holds_alternative<double>(value_variant)) {
                value_str = std::to_string(std::get<double>(value_variant));
            } else if (std::holds_alternative<bool>(value_variant)) {
                value_str = std::get<bool>(value_variant) ? "true" : "false";
            }
            else {
                value_str = "(unknown value)";
            }

            res[key_str] = value_str;
        }
        return res;
    }
    case RedisType::Set: {
        std::unordered_set<std::string> res;
        const auto& set = obj.get_set();
        for (const auto& item : set) {
            auto variant_item = conv_value(item);
            if (std::holds_alternative<std::string>(variant_item)) {
                res.insert(std::get<std::string>(variant_item));
            } else if (std::holds_alternative<long long>(variant_item)) {
                res.insert(std::to_string(std::get<long long>(variant_item)));
            } else {
                res.insert("(unknown)");
            }
        }
        return res;
    }
    case RedisType::Null:
        return std::string("null");
    default:
        return std::string("(unsupported type)");
    }
}

xAwaiter xredis_set(const std::string& key, const std::string& value) {
    return xredis_command({ "SET", key, value });
}

xAwaiter xredis_get(const std::string& key) {
    return xredis_command({ "GET", key });
}

xAwaiter xredis_hset(const std::string& key, const std::string& field, const std::string& value) {
    return xredis_command({ "HSET", key, field, value });
}

xAwaiter xredis_hget(const std::string& key, const std::string& field) {
    return xredis_command({ "HGET", key, field });
}

xAwaiter xredis_hgetall(const std::string& key) {
    return xredis_command({ "HGETALL", key });
}

//xAwaiter xredis_unsubscribe_task(std::string channel) { 
//    return xredis_unsubscribe(channel);
//}

xCoroTaskT<bool> xredis_subscribe_task(std::string channel, RedisSubscribeCallback fnCallback) {
    co_await xredis_subscribe(channel, fnCallback);
    co_return true;
}

xCoroTaskT<int> xredis_subscribe(const std::string& channel_or_pattern, RedisSubscribeCallback fnCallback) {
    // 1. 检查是否已经存在相同订阅
    auto existing_it = _pool->subscribe_conns.find(channel_or_pattern);
    if (existing_it != _pool->subscribe_conns.end()) {
        // 更新回调函数
        _pool->callbacks[channel_or_pattern] = fnCallback;
        xlog_info("Subscription updated for: %s", channel_or_pattern.c_str());
        co_return XNET_SUCCESS;
    }

    // 2. 检查是否被现有订阅覆盖（本地覆盖）
    for (const auto& [existing_key, conn] : _pool->subscribe_conns) {
        SubscriptionRelation relation = check_subscription_relation(existing_key, channel_or_pattern);

        if (relation == SubscriptionRelation::A_COVERS_B || relation == SubscriptionRelation::IDENTICAL) {
            // 本地覆盖：不需要发送Redis命令，只更新本地映射
            _pool->callbacks[channel_or_pattern] = fnCallback;
            xlog_info("Local coverage: %s covered by existing %s",
                channel_or_pattern.c_str(), existing_key.c_str());
            co_return XNET_SUCCESS;
        }
    }

    // 3. 检查是否会覆盖现有订阅（需要先取消旧的）
    std::vector<std::string> to_unsubscribe;
    for (const auto& [existing_key, conn] : _pool->subscribe_conns) {
        SubscriptionRelation relation = check_subscription_relation(channel_or_pattern, existing_key);

        if (relation == SubscriptionRelation::A_COVERS_B) {
            to_unsubscribe.push_back(existing_key);
        }
    }

    // 4. 取消被覆盖的订阅（发送Redis命令）
    for (const auto& key : to_unsubscribe) {
        xlog_info("Auto unsubscribing %s covered by new subscription %s",
            key.c_str(), channel_or_pattern.c_str());
        co_await xredis_unsubscribe(key);
    }

    if (!to_unsubscribe.empty()) {
        coroutine_sleep(100);
    }

    // 5. 创建新的服务器订阅
    RedisConn* conn = fetch_free_conn();
    if (!conn) {
        xlog_err("Failed to fetch free connection for subscription: %s", channel_or_pattern.c_str());
        co_return (XNET_REDIS_CONNECT);
    }

    bool pattern = is_pattern(channel_or_pattern);
    std::string cmd_type = pattern ? "PSUBSCRIBE" : "SUBSCRIBE";

    conn->ready = false;
    _pool->subscribe_conns[channel_or_pattern] = conn;
    _pool->callbacks[channel_or_pattern] = fnCallback;

    try {
        std::vector<RedisObject> cmd_args;
        cmd_args.emplace_back(RedisObject::Bulk(cmd_type));
        cmd_args.emplace_back(RedisObject::Bulk(channel_or_pattern));
        RedisObject cmd = RedisObject::Array(cmd_args);

        std::string resp_data = redis::redis_pack(cmd,
            conn->config.use_resp3 ? RedisProtocol::RESP3 : RedisProtocol::RESP2);

        if (xchannel_rawsend(conn->channel, resp_data.data(), resp_data.size()) != resp_data.size()) {
            xlog_err("Failed to send subscribe command for: %s", channel_or_pattern.c_str());
            _pool->subscribe_conns.erase(channel_or_pattern);
            _pool->callbacks.erase(channel_or_pattern);
            conn->ready = true;
            release_connection(conn);
            co_return (XNET_REDIS_SEND);
        }
        xAwaiter awaiter;
        uint32_t wait_id = awaiter.wait_id();
        xqueue_circle_enqueue(&conn->queue, &wait_id);
        co_await awaiter;

        xlog_info("Server subscription created: %s (type: %s)",
            channel_or_pattern.c_str(), cmd_type.c_str());
        co_return (XNET_SUCCESS);
    }
    catch (const std::exception& e) {
        xlog_err("Exception in xredis_subscribe for %s: %s", channel_or_pattern.c_str(), e.what());
        _pool->subscribe_conns.erase(channel_or_pattern);
        _pool->callbacks.erase(channel_or_pattern);
        conn->ready = true;
        release_connection(conn);
        co_return (XNET_REDIS_ERROR);
    }
}

xAwaiter xredis_publish(const std::string& channel, const std::string& message) {
    std::vector<std::string> args = { "PUBLISH", channel, message };
    return xredis_command(args);
}

xCoroTaskT<int> xredis_unsubscribe(const std::string& channel_or_pattern) {
    // 1. 检查是否是服务器订阅
    auto server_it = _pool->subscribe_conns.find(channel_or_pattern);
    if (server_it != _pool->subscribe_conns.end()) {
        RedisConn* conn = server_it->second;
        bool pattern = is_pattern(channel_or_pattern);
        std::string cmd_type = pattern ? "PUNSUBSCRIBE" : "UNSUBSCRIBE";

        // 2. 立即查找需要提升的订阅
        std::vector<std::pair<std::string, RedisSubscribeCallback>> to_promote;
        for (const auto& [key, callback] : _pool->callbacks) {
            if (key != channel_or_pattern && _pool->subscribe_conns.find(key) == _pool->subscribe_conns.end()) {
                // 这是一个被覆盖的订阅
                SubscriptionRelation relation = check_subscription_relation(channel_or_pattern, key);
                if (relation == SubscriptionRelation::A_COVERS_B) {
                    // 检查是否还有其他订阅覆盖它
                    bool still_covered = false;
                    for (const auto& [other_key, other_conn] : _pool->subscribe_conns) {
                        if (other_key != channel_or_pattern) {
                            SubscriptionRelation other_relation = check_subscription_relation(other_key, key);
                            if (other_relation == SubscriptionRelation::A_COVERS_B) {
                                still_covered = true;
                                break;
                            }
                        }
                    }
                    if (!still_covered) {
                        to_promote.emplace_back(key, callback);
                    }
                }
            }
        }

        // 3. 发送取消订阅命令
        _pool->pending_unsubscribe.insert(channel_or_pattern);
        _pool->callbacks.erase(channel_or_pattern);

        try {
            std::vector<RedisObject> cmd_args;
            cmd_args.emplace_back(RedisObject::Bulk(cmd_type));
            cmd_args.emplace_back(RedisObject::Bulk(channel_or_pattern));
            RedisObject cmd = RedisObject::Array(cmd_args);

            std::string resp_data = redis::redis_pack(cmd,
                conn->config.use_resp3 ? RedisProtocol::RESP3 : RedisProtocol::RESP2);

            if (xchannel_rawsend(conn->channel, resp_data.data(), resp_data.size()) != resp_data.size()) {
                xlog_err("Failed to send unsubscribe command for: %s", channel_or_pattern.c_str());
                co_return (XNET_REDIS_SEND);
            }
            {
                auto res = co_await wait_void(conn);
                server_it = _pool->subscribe_conns.find(channel_or_pattern);
                xlog_debug("Unsubscribe command sent for: %s, resp=%d", channel_or_pattern.c_str(), res);
                _pool->pending_unsubscribe.erase(channel_or_pattern);
                if (server_it != _pool->subscribe_conns.end())
                    _pool->subscribe_conns.erase(server_it);
                conn->ready = true;
                release_connection(conn);
            }

            // 4. 立即提升被覆盖的订阅（不需要等待确认）
            for (const auto& [to_promote_key, to_promote_callback] : to_promote) {
                xlog_info("Promoting subscription: %s", to_promote_key.c_str());
                co_await xredis_subscribe_task(to_promote_key, to_promote_callback);
            }

            xlog_info("Unsubscribe command sent for: %s", channel_or_pattern.c_str());
            co_return (XNET_SUCCESS);
        }
        catch (const std::exception& e) {
            xlog_err("Exception in xredis_unsubscribe for %s: %s", channel_or_pattern.c_str(), e.what());
            _pool->pending_unsubscribe.erase(channel_or_pattern);
            _pool->subscribe_conns.erase(server_it);
            conn->ready = true;
            release_connection(conn);
            co_return (XNET_REDIS_ERROR);
        }
    }

    // 5. 检查是否是被本地覆盖的订阅
    for (const auto& [server_key, conn] : _pool->subscribe_conns) {
        SubscriptionRelation relation = check_subscription_relation(server_key, channel_or_pattern);
        if (relation == SubscriptionRelation::A_COVERS_B) {
            // 本地覆盖的订阅，只需要本地清理
            _pool->callbacks.erase(channel_or_pattern);
            xlog_info("Removed local covered subscription: %s", channel_or_pattern.c_str());
            co_return (XNET_SUCCESS);
        }
    }

    xlog_warn("Subscription not found: %s", channel_or_pattern.c_str());
    co_return (XNET_REDIS_NOT_SUBSCRIBED);
}

int xredis_status(int* total, int* idle, int* in_use, int* initializing) {
    if (!_pool) return -1;

    if (total) *total = _pool->total_created;
    if (idle) *idle = static_cast<int>(_pool->free_conns.size());
    if (in_use) *in_use = static_cast<int>(_pool->busy_conns.size());
    if (initializing) *initializing = _pool->initializing;

    return 0;
}
