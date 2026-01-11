#ifndef HTTPC_H
#define HTTPC_H

#include <stdint.h>
#include <stddef.h>
#ifndef _WIN32
#define u8 ""
#endif

/**
 * @brief HTTP 客户端错误码
 */
typedef enum {
    HTTPC_SUCCESS = 0,        // 成功
    HTTPC_ERR_INIT = -1,      // 初始化失败
    HTTPC_ERR_CONNECT = -2,   // 连接服务器失败
    HTTPC_ERR_SSL_HANDSHAKE = -3, // SSL 握手失败
    HTTPC_ERR_SSL_CERT = -4,  // 证书加载/验证失败
    HTTPC_ERR_WRITE = -5,     // 数据发送失败
    HTTPC_ERR_READ = -6,      // 数据接收失败
    HTTPC_ERR_PARAM = -7,     // 参数非法
    HTTPC_ERR_PARSE = -8      // 解析结果失
} httpc_err_t;

/**
 * @brief HTTP 客户端配置（ca_cert_path 可选）
 * @note ca_cert_path：NULL/空字符串 → 使用内置证书；有效路径 → 使用指定证书文件
 */
typedef struct {
    const char* server_host;  // 服务器域名/IP（如 "cn.bing.com"）
    const char* server_port;  // 服务器端口（HTTP "80"，HTTPS "443"）
    int is_https;             // 是否启用 HTTPS（1=启用，0=禁用）
    const char* ca_cert_path; // 可选：CA 证书文件路径（如 "./cacert.pem"）
    const char* request;      // HTTP 请求内容
    uint32_t debug_level;     // 调试级别（0=关闭，1=开启）
} httpc_config_t;

/**
 * @brief HTTP 客户端上下文（对外隐藏具体实现）
 */
typedef struct httpc_client_s httpc_client_t;

/**
 * @brief 初始化 HTTP 客户端
 * @param config 客户端配置（必填）
 * @return 客户端上下文（NULL 表示失败）
 */
httpc_client_t* httpc_client_init(const httpc_config_t* config);

/**
 * @brief 发送 HTTP/HTTPS 请求并接收响应
 * @param client 客户端上下文
 * @param resp_buf 接收响应的缓冲区
 * @param resp_buf_len 缓冲区长度
 * @param actual_read 实际读取的响应长度（输出参数，可传 NULL）
 * @return 错误码（HTTPC_SUCCESS 表示成功）
 */
httpc_err_t httpc_client_request(httpc_client_t* client,
    char* resp_buf,
    size_t resp_buf_len,
    size_t* actual_read);

/**
 * @brief 释放 HTTP 客户端资源
 * @param client 客户端上下文
 */
void httpc_client_free(httpc_client_t* client);

#endif // HTTPC_H