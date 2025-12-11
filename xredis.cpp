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

    RedisConn(const RedisConnConfig& cfg) : config(cfg) {}
    ~RedisConn() {
        if (channel) {
            xchannel_close(channel);
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
    xCircleQueue queue;
};

#ifdef _WIN32
static _declspec(thread) RedisPool* _pool = NULL;
#else
static __thread RedisPool*          _pool = NULL;
#endif

static VariantType conv_value(const RedisObject& val);

int RedisConn::handle_packet(char* buf, int len) {
    xlog_debug("RedisConn::handle_packet:%.*s", len, buf);

    try {
        std::vector<RedisObject> objs = redis::redis_unpack(buf, len,
            config.use_resp3 ? RedisProtocol::RESP3 : RedisProtocol::RESP2);
        if (!ready) {
            std::vector<VariantType> result;
            std::map<std::string, std::string> server_infos;
            for (const auto& obj : objs) {
                result.push_back(conv_value(obj));
            }
            uint32_t wait_id = 0;
            xqueue_circle_dequeue(&_pool->queue, &wait_id);
            coroutine_resume(wait_id, std::move(result));
            return len;
        }

        for (const auto& obj : objs) {
            // 查找带wait_id属性的响应
            uint32_t wait_id = 0;
            xqueue_circle_dequeue(&_pool->queue, &wait_id);

            //// 检查是否是属性包装的数组
            //if (obj.is_array() && obj.get_array().size() >= 2) {
            //    const auto& arr = obj.get_array();
            //    const auto& first = arr[0];

            //    if (first.is_map()) {
            //        const auto& attrs = first.get_map();
            //        for (const auto& [key, value] : attrs) {
            //            if (key.get_string() == "wait_id") {
            //                try {
            //                    wait_id = static_cast<uint32_t>(std::stoul(value.get_string()));
            //                    break;
            //                }
            //                catch (...) {
            //                    xlog_err("Invalid wait_id in response");
            //                }
            //            }
            //        }
            //    }
            //}

            if (wait_id != 0) {
                std::vector<VariantType> result;
                result.push_back(0);

                if (obj.is_array()) {
                    const auto& arr = obj.get_array();
                    for (size_t i = 1; i < arr.size(); ++i) {
                        result.push_back(conv_value(arr[i]));
                    }
                } else {
                    result.push_back(conv_value(obj));
                }

                coroutine_resume(wait_id, std::move(result));
            } else {
                // 没有wait_id的响应，可能是PUB/SUB消息或其他推送
                // 这里可以根据需要处理推送消息
                if (obj.is_push()) {
                    // 处理推送数据
                    const auto& push_data = obj.get_array();
                    if (!push_data.empty()) {
                        std::string type = push_data[0].get_string();
                        xlog_info("Received push data of type: %s", type.c_str());
                        //TODO:订阅消息、配置更新等
                    }
                } else {
                    xlog_err("Received Response : %s", std::string(buf, len).c_str());
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
        xqueue_circle_enqueue(&_pool->queue, &wait_id);

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
        uint32_t wait_id = awaiter.wait_id();
        xqueue_circle_enqueue(&_pool->queue, &wait_id);

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
        uint32_t wait_id = awaiter.wait_id();
        xqueue_circle_enqueue(&_pool->queue, &wait_id);

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
    xqueue_circle_init(&_pool->queue, sizeof(uint32_t), 100);

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
    xqueue_circle_uninit(&_pool->queue);

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

xAwaiter xredis_command(const std::vector<std::string>& args) {
    RedisConn* conn = fetch_free_conn();
    if (!_pool) return xAwaiter(XNET_REDIS_NOT_INIT);
    if (!conn){
        if (_pool->total_created < _pool->max_conn) {
            // TODO : create new connection
        } else {
            int retry = 5;
            while (!conn) {
                coroutine_sleep(100);
                conn = fetch_free_conn();
                if ((--retry) == 0) break;
            }
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
            //// 创建带wait_id属性的请求
            //RedisAttributes attrs;
            //attrs.emplace_back(
            //    RedisObject::Simple("wait_id"),
            //    RedisObject::BigNumber(std::to_string(wait_id))
            //);

            //std::vector<RedisObject> request_parts;
            //request_parts.emplace_back(cmd);
            //RedisObject full_request = RedisObject::Array(request_parts);

            resp_data = redis::redis_pack(cmd, RedisProtocol::RESP3);
        } else {
            resp_data = redis::redis_pack(cmd, RedisProtocol::RESP2);
        }

        // 发送命令
        xlog_debug("Sending Redis command: %s", resp_data.c_str());
        int send_len = xchannel_rawsend(conn->channel, resp_data.data(), resp_data.size());
        if (send_len != (int)resp_data.size()) {
            xlog_err("Failed to send Redis command, sent %d of %d bytes", send_len, resp_data.size());
            return xAwaiter(XNET_REDIS_SEND);
        }
        xqueue_circle_enqueue(&_pool->queue, &wait_id);
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

int xredis_status(int* total, int* idle, int* in_use, int* initializing) {
    if (!_pool) return -1;

    if (total) *total = _pool->total_created;
    if (idle) *idle = static_cast<int>(_pool->free_conns.size());
    if (in_use) *in_use = static_cast<int>(_pool->busy_conns.size());
    if (initializing) *initializing = _pool->initializing;

    return 0;
}
