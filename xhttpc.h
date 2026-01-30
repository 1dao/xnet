#ifndef HTTPC_H
#define HTTPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
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
    HTTPC_ERR_PARSE = -8,     // 解析结果失败
    HTTPC_ERR_REDIRECT = -9,  // 重定向失败
    HTTPC_ERR_TOO_MANY_REDIRECTS = -10  // 重定向次数过多
} httpc_err_t;

/**
 * @brief HTTP 响应信息
 */
typedef struct {
    int status_code;          // HTTP状态码 (200, 301, 302, etc.)
    char location[1024];      // 重定向URL (如果有)
    size_t header_length;     // HTTP头部长度
    size_t content_length;    // 内容长度
    char* content_start;      // 内容起始位置
} httpc_response_t;

/**
 * @brief HTTP 客户端配置（ca_cert_path 可选）
 * @note ca_cert_path：NULL/空字符串 → 使用内置证书；有效路径 → 使用指定证书文件
 */
typedef struct {
    // 连接配置
    const char* server_host;  // 服务器域名/IP（如 "cn.bing.com"）
    const char* server_port;  // 服务器端口（HTTP "80"，HTTPS "443"）
    int is_https;             // 是否启用 HTTPS（1=启用，0=禁用）
    const char* ca_cert_path; // 可选：CA 证书文件路径（如 "./cacert.pem"）
    uint32_t debug_level;     // 调试级别（0=关闭，1=开启）

    // HTTP请求 - 两种模式：
    // 模式1：提供组件，动态构建
    // 模式2：提供完整请求字符串
    const char* request;      // 模式2：完整的HTTP请求字符串（如果不为NULL，优先使用）

    // HTTP请求组件（模式1，当request为NULL时使用）
    const char* method;        // HTTP方法（GET, POST, PUT, DELETE等）
    const char* url_path;     // URL路径（如 "/translator/api/v2/Http.svc/TranslateArray"）
    const char* content_type;  // Content-Type头（如 "application/x-www-form-urlencoded"）
    const char* user_agent;    // User-Agent头（可选，NULL使用默认）
    const char* data;         // POST/PUT数据（GET请求可为NULL）
    size_t data_length;       // 数据长度（0表示自动计算）

    // 额外头部（可选，格式："Header1: value1\r\nHeader2: value2"）
    const char* extra_headers;
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
 * @brief 发送 HTTP/HTTPS 请求并接收响应（自动处理重定向）
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
 * @brief 解析HTTP响应头
 * @param response_data 完整的HTTP响应数据
 * @param response 解析后的响应信息
 * @return 错误码
 */
httpc_err_t httpc_parse_response(const char* response_data, httpc_response_t* response);

/**
 * @brief 释放 HTTP 客户端资源
 * @param client 客户端上下文
 */
void httpc_client_free(httpc_client_t* client);

/**
 * @brief 动态拼接HTTP请求字符串
 * @param config HTTP配置
 * @return 拼接后的请求字符串（需要调用者释放内存）
 */
char* httpc_build_request(const httpc_config_t* config);

/**
 * @brief URL编码字符串
 * @param str 要编码的字符串
 * @return 编码后的字符串（需要调用者释放内存）
 */
char* httpc_url_encode(const char* str);

/**
 * @brief 简单的语言检测
 * @param text 要检测的文本
 * @return 语言代码（"en"或"zh-cn"）
 */
const char* httpc_detect_language(const char* text);

/**
 * @brief 从JSON响应中提取pattern字段
 * @param content JSON响应字符串
 * @param start_pattern 起始模式
 * @param end_pattern 结束模式
 * @param result 结果缓冲区
 * @param result_len 结果缓冲区长度
 * @return 错误码（HTTPC_SUCCESS 表示成功，HTTPC_ERR_INVALID_PARAM 表示参数错误）
 */
int httpc_extract_pattern(const char* content, const char* start_pattern, const char* end_pattern, char* result, size_t result_len);

/**
 * @brief 统一将 GBK/UTF-8 字符串转为 UTF-8（对外接口）
 * @param input_str 输入字符串（GBK/UTF-8）
 * @param output_buf 输出 UTF-8 缓冲区
 * @param buf_len 缓冲区长度
 * @return 成功: 转换后长度, 失败: -1
 */
int httpc_any_to_utf8(const char* input_str, char* output_buf, size_t buf_len);

/**
 * @brief 将 UTF-8 编码的 Unicode 字符串解码为 UTF-8（对外接口）
 * @param start 起始位置
 * @param len 长度
 * @param result 结果缓冲区
 * @param result_len 结果缓冲区长度
 * @return 成功: 转换后长度, 失败: -1
 */
int httpc_decode_unicode(const char* start, size_t len, char* result, size_t result_len);

#endif // HTTPC_H
