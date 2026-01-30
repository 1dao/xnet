#include "xhttpc.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>  // for strdup

#include "xhttpc_cacert.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#endif

/**
 * @brief 客户端上下文具体实现（对外隐藏）
 */
struct httpc_client_s {
    httpc_config_t config;                // 配置拷贝
    mbedtls_net_context net_fd;           // 网络套接字
    mbedtls_ssl_context ssl;              // SSL 上下文（HTTPS 用）
    mbedtls_ssl_config ssl_conf;          // SSL 配置（HTTPS 用）
    mbedtls_x509_crt cacert;              // CA 证书（HTTPS 用）
    mbedtls_ctr_drbg_context ctr_drbg;    // 随机数生成器（HTTPS 用）
    mbedtls_entropy_context entropy;      // 熵源（HTTPS 用）
    int is_init;                          // 初始化标记
};

static int is_empty_string(const char* str) {
    return (str == NULL || strlen(str) == 0);
}

static void httpc_debug(void* ctx, int level, const char* file, int line, const char* str) {
    (void)level;
    fprintf((FILE*)ctx, u8"%s:%04d: %s", file, line, str);
    fflush((FILE*)ctx);
}

/**
 * @brief 初始化 HTTPS 相关上下文（双证书策略，适配 mbedtls 2.16.11）
 */
static httpc_err_t httpc_https_init(httpc_client_t* client) {
    int ret;
    const char* pers = "httpc_client";

    // 初始化随机数生成器
    mbedtls_ctr_drbg_init(&client->ctr_drbg);
    mbedtls_entropy_init(&client->entropy);
    ret = mbedtls_ctr_drbg_seed(&client->ctr_drbg, mbedtls_entropy_func, &client->entropy,
        (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        fprintf(stderr, u8"随机数生成器初始化失败: %d\n", ret);
        return HTTPC_ERR_INIT;
    }

    // ===================== 核心：双证书分支加载逻辑 =====================
    mbedtls_x509_crt_init(&client->cacert);
    int cert_ret = 0;

    // 1. 优先使用指定的证书文件（若路径有效）
    if (!is_empty_string(client->config.ca_cert_path)) {
        cert_ret = mbedtls_x509_crt_parse_file(&client->cacert, client->config.ca_cert_path);
        if (cert_ret < 0) {
            fprintf(stderr, u8"证书文件 [%s] 加载失败: -0x%04x\n", client->config.ca_cert_path, (unsigned int)-cert_ret);
            return HTTPC_ERR_SSL_CERT;
        }
        //fprintf(stdout, u8"✅ 证书文件 [%s] 加载成功（跳过 %d 个无效证书）\n", client->config.ca_cert_path, cert_ret);
    }
    // 2. 回退使用内置内存证书（路径无效时）
    else {
        const unsigned char* cacert_data = httpc_cacert_get_data();
        size_t cacert_len = httpc_cacert_get_len();
        cert_ret = mbedtls_x509_crt_parse(&client->cacert, cacert_data, cacert_len);
        if (cert_ret < 0) {
            fprintf(stderr, u8"❌ 内置证书解析失败: -0x%04x\n", (unsigned int)-cert_ret);
            return HTTPC_ERR_SSL_CERT;
        }
        //fprintf(stdout, u8"✅ 内置证书解析成功（跳过 %d 个无效证书）\n", cert_ret);
    }

    // 初始化 SSL 配置
    mbedtls_ssl_config_init(&client->ssl_conf);
    ret = mbedtls_ssl_config_defaults(&client->ssl_conf, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        fprintf(stderr, u8"SSL 配置初始化失败: %d\n", ret);
        return HTTPC_ERR_INIT;
    }

    // 设置 SSL 验证模式和 CA 证书链
    mbedtls_ssl_conf_authmode(&client->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&client->ssl_conf, &client->cacert, NULL);
    mbedtls_ssl_conf_rng(&client->ssl_conf, mbedtls_ctr_drbg_random, &client->ctr_drbg);

    // 启用调试（如果配置开启）
    if (client->config.debug_level > 0) {
        mbedtls_ssl_conf_dbg(&client->ssl_conf, httpc_debug, stdout);
    }

    // 初始化 SSL 上下文
    mbedtls_ssl_init(&client->ssl);
    ret = mbedtls_ssl_setup(&client->ssl, &client->ssl_conf);
    if (ret != 0) {
        fprintf(stderr, u8"SSL 上下文初始化失败: %d\n", ret);
        return HTTPC_ERR_INIT;
    }

    // 设置服务器主机名（SNI 扩展，mbedtls 2.16.11 支持）
    ret = mbedtls_ssl_set_hostname(&client->ssl, client->config.server_host);
    if (ret != 0) {
        fprintf(stderr, u8"设置 SNI 失败: %d\n", ret);
        return HTTPC_ERR_INIT;
    }

    return HTTPC_SUCCESS;
}

/**
 * @brief 初始化客户端上下文
 */
httpc_client_t* httpc_client_init(const httpc_config_t* config) {
    if (config == NULL || config->server_host == NULL || config->server_port == NULL) {
        fprintf(stderr, u8"参数非法（服务器地址/端口不能为空）\n");
        return NULL;
    }

    // 检查HTTP请求配置
    if (!config->request && (!config->method || !config->url_path)) {
        fprintf(stderr, u8"参数非法（必须提供完整request字符串或method+url_path组件）\n");
        return NULL;
    }

    // 分配客户端上下文
    httpc_client_t* client = (httpc_client_t*)calloc(1, sizeof(httpc_client_t));
    if (client == NULL) {
        fprintf(stderr, u8"内存分配失败\n");
        return NULL;
    }

    // 拷贝配置
    memcpy(&client->config, config, sizeof(httpc_config_t));
    client->is_init = 0;

    // 初始化网络套接字
    mbedtls_net_init(&client->net_fd);

    // 连接服务器（TCP）
    int ret = mbedtls_net_connect(&client->net_fd, config->server_host, config->server_port, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        fprintf(stderr, u8"连接服务器 %s:%s 失败: %d\n", config->server_host, config->server_port, ret);
        free(client);
        return NULL;
    }

    // 如果是 HTTPS，初始化 SSL 相关逻辑
    if (config->is_https) {
        ret = httpc_https_init(client);
        if (ret != HTTPC_SUCCESS) {
            mbedtls_net_free(&client->net_fd);
            free(client);
            return NULL;
        }

        // 绑定 SSL BIO
        mbedtls_ssl_set_bio(&client->ssl, &client->net_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

        // SSL 握手
        ret = 0;
        while ((ret = mbedtls_ssl_handshake(&client->ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                fprintf(stderr, u8"SSL 握手失败: -0x%04x\n", (unsigned int)-ret);
                mbedtls_net_free(&client->net_fd);
                free(client);
                return NULL;
            }
        }

        // 验证服务器证书
        uint32_t verify_flags = mbedtls_ssl_get_verify_result(&client->ssl);
        if (verify_flags != 0) {
            char vrfy_buf[512];
            mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "  ! ", verify_flags);
            fprintf(stderr, u8"服务器证书验证失败: %s\n", vrfy_buf);
            mbedtls_net_free(&client->net_fd);
            free(client);
            return NULL;
        }
    }

    client->is_init = 1;
    return client;
}

/**
 * @brief 解析HTTP响应头
 */
httpc_err_t httpc_parse_response(const char* response_data, httpc_response_t* response) {
    if (!response_data || !response) {
        return HTTPC_ERR_PARAM;
    }

    // 初始化响应结构
    memset(response, 0, sizeof(httpc_response_t));

    // 查找状态行 "HTTP/1.1 200 OK"
    const char* status_line = response_data;
    while (*status_line && *status_line != '\n') {
        status_line++;
    }

    // 解析状态码 - 查找 "HTTP/1.x XXX" 格式
    const char* status_start = response_data;
    // 跳过 "HTTP/"
    while (*status_start && *status_start != ' ') {
        status_start++;
    }
    // 跳过空格
    while (*status_start && *status_start == ' ') {
        status_start++;
    }

    if (isdigit(*status_start)) {
        response->status_code = atoi(status_start);
    }

    // 查找Location头
    const char* location_pos = strstr(response_data, "\nLocation:");
    if (location_pos) {
        location_pos += 10; // 跳过 "\nLocation:"
        while (*location_pos && isspace(*location_pos)) {
            location_pos++;
        }

        const char* location_end = location_pos;
        while (*location_end && *location_end != '\r' && *location_end != '\n') {
            location_end++;
        }

        size_t location_len = location_end - location_pos;
        if (location_len > 0 && location_len < sizeof(response->location) - 1) {
            strncpy(response->location, location_pos, location_len);
            response->location[location_len] = '\0';
        }
    }

    // 查找头部和内容的分界 "\r\n\r\n"
    const char* header_end = strstr(response_data, "\r\n\r\n");
    if (header_end) {
        response->header_length = header_end - response_data + 4;
        response->content_start = (char*)header_end + 4;
        response->content_length = strlen(response->content_start);
    } else {
        // 没有找到头部，假设整个都是内容
        response->content_start = (char*)response_data;
        response->content_length = strlen(response_data);
    }

    return HTTPC_SUCCESS;
}

/**
 * @brief 从URL解析主机和路径
 */
static httpc_err_t parse_url(const char* url, char* host, size_t host_len, char* path, size_t path_len, int* is_https) {
    if (!url || !host || !path || !is_https) {
        return HTTPC_ERR_PARAM;
    }

    // 默认值
    *is_https = 0;
    host[0] = '\0';
    path[0] = '\0';

    // 跳过协议
    const char* url_start = url;
    if (strncmp(url_start, "https://", 8) == 0) {
        *is_https = 1;
        url_start += 8;
    } else if (strncmp(url_start, "http://", 7) == 0) {
        url_start += 7;
    }

    // 提取主机名
    const char* host_start = url_start;
    const char* host_end = strchr(host_start, '/');

    size_t host_len_actual;
    if (host_end) {
        host_len_actual = host_end - host_start;
        // 提取路径
        strncpy(path, host_end, path_len - 1);
        path[path_len - 1] = '\0';
    } else {
        host_len_actual = strlen(host_start);
        strncpy(path, "/", path_len - 1);
        path[path_len - 1] = '\0';
    }

    if (host_len_actual >= host_len) {
        return HTTPC_ERR_PARAM;
    }

    strncpy(host, host_start, host_len_actual);
    host[host_len_actual] = '\0';

    return HTTPC_SUCCESS;
}

/**
 * @brief 动态拼接HTTP请求字符串
 */
char* httpc_build_request(const httpc_config_t* config) {
    if (!config) {
        return NULL;
    }

    // 模式2：如果提供了完整请求字符串，直接复制
    if (config->request) {
        char* request = malloc(strlen(config->request) + 1);
        if (!request) {
            return NULL;
        }
        strcpy(request, config->request);
        return request;
    }

    // 模式1：动态构建请求
    if (!config->method || !config->url_path) {
        return NULL;
    }

    // 计算所需缓冲区大小
    size_t buffer_size = 4096; // 基础大小
    if (config->data && config->data_length > 0) {
        buffer_size += config->data_length + 1024; // 额外空间给数据和其他头部
    }
    if (config->extra_headers) {
        buffer_size += strlen(config->extra_headers) + 256;
    }

    char* request = malloc(buffer_size);
    if (!request) {
        return NULL;
    }

    // 开始构建请求
    int pos = 0;

    // 请求行
    pos += snprintf(request + pos, buffer_size - pos,
                   "%s %s HTTP/1.1\r\n",
                   config->method, config->url_path);

    // Host头部
    pos += snprintf(request + pos, buffer_size - pos,
                   "Host: %s\r\n",
                   config->server_host);

    // Connection头部
    pos += snprintf(request + pos, buffer_size - pos,
                   "Connection: close\r\n");

    // User-Agent头部
    if (config->user_agent) {
        pos += snprintf(request + pos, buffer_size - pos,
                       "User-Agent: %s\r\n",
                       config->user_agent);
    } else {
        pos += snprintf(request + pos, buffer_size - pos,
                       "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0.0) Safari/537.36\r\n");
    }

    // Content-Type头部（如果有数据）
    if (config->data && (config->data_length > 0 || strlen(config->data) > 0)) {
        if (config->content_type) {
            pos += snprintf(request + pos, buffer_size - pos,
                           "Content-Type: %s\r\n",
                           config->content_type);
        } else {
            pos += snprintf(request + pos, buffer_size - pos,
                           "Content-Type: application/x-www-form-urlencoded\r\n");
        }

        // Content-Length头部
        size_t data_len = config->data_length > 0 ? config->data_length : strlen(config->data);
        pos += snprintf(request + pos, buffer_size - pos,
                       "Content-Length: %zu\r\n",
                       data_len);
    }

    // 额外头部
    if (config->extra_headers) {
        pos += snprintf(request + pos, buffer_size - pos,
                       "%s\r\n",
                       config->extra_headers);
    }

    // 结束头部
    pos += snprintf(request + pos, buffer_size - pos, "\r\n");

    // 添加数据（如果有）
    if (config->data && (config->data_length > 0 || strlen(config->data) > 0)) {
        size_t data_len = config->data_length > 0 ? config->data_length : strlen(config->data);
        memcpy(request + pos, config->data, data_len);
        pos += (int)data_len;
    }

    request[pos] = '\0';
    return request;
}

/**
 * @brief 发送单个HTTP请求（不处理重定向）
 */
static httpc_err_t httpc_single_request(httpc_client_t* client, char* resp_buf, size_t resp_buf_len, size_t* actual_read) {
    if (client == NULL || !client->is_init || resp_buf == NULL || resp_buf_len == 0) {
        return HTTPC_ERR_PARAM;
    }

    int ret;
    size_t total_read = 0;

    // 动态构建请求
    char* req = client->config.request?(char*)client->config.request:httpc_build_request(&client->config);
    if (!req) {
        fprintf(stderr, "Failed to build HTTP request\n");
        return HTTPC_ERR_PARAM;
    }

    size_t req_len = strlen(req)+1;
    if (client->config.debug_level > 0)
        printf("[DEBUG] http request sending, len=%d: \n%s\n", (int)req_len, req);

    // 发送请求
    if (client->config.is_https) {
        ret = 0;
        while ((ret = mbedtls_ssl_write(&client->ssl, (const unsigned char*)req, req_len)) <= 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                fprintf(stderr, u8"HTTPS 发送失败: %d\n", ret);
                if (!client->config.request)
                    free(req);
                return HTTPC_ERR_WRITE;
            }
        }
    } else {
        ret = mbedtls_net_send(&client->net_fd, (const unsigned char*)req, req_len);
        if (ret <= 0) {
            fprintf(stderr, u8"HTTP 发送失败: %d\n", ret);
            if (!client->config.request)
                free(req);
            return HTTPC_ERR_WRITE;
        }
    }

    if (!client->config.request)
        free(req);

    // 接收响应
    memset(resp_buf, 0, resp_buf_len);
    while (1) {
        size_t read_len = resp_buf_len - total_read - 1; // 留空终止符
        if (read_len == 0) break;

        if (client->config.is_https) {
            ret = mbedtls_ssl_read(&client->ssl, (unsigned char*)(resp_buf + total_read), read_len);
        }
        else {
            ret = mbedtls_net_recv(&client->net_fd, (unsigned char*)(resp_buf + total_read), read_len);
        }

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
            break; // 连接关闭
        }
        if (ret < 0) {
            fprintf(stderr, u8"接收响应失败: %d\n", ret);
            return HTTPC_ERR_READ;
        }

        total_read += ret;
        if (total_read >= resp_buf_len - 1) break; // 缓冲区满
    }

    if (actual_read != NULL) {
        *actual_read = total_read;
    }
    if (client->config.debug_level > 0)
        printf("[DEBUG] Receive response, len=%d.\n", (int)total_read);
    return HTTPC_SUCCESS;
}

void httpc_client_free(httpc_client_t* client) {
    if (client == NULL) return;

    if (client->config.is_https) {
        mbedtls_ssl_close_notify(&client->ssl);
        mbedtls_x509_crt_free(&client->cacert);
        mbedtls_ssl_free(&client->ssl);
        mbedtls_ssl_config_free(&client->ssl_conf);
        mbedtls_ctr_drbg_free(&client->ctr_drbg);
        mbedtls_entropy_free(&client->entropy);
    }

    mbedtls_net_free(&client->net_fd);
    free(client);
}

/**
 * @brief 发送 HTTP/HTTPS 请求并接收响应（带响应信息）
 */
httpc_err_t httpc_client_request(httpc_client_t* client,
    char* resp_buf,
    size_t resp_buf_len,
    size_t* actual_read) {

    if (!client || !resp_buf || resp_buf_len == 0) {
        return HTTPC_ERR_PARAM;
    }

    int redirect_count = 0;
    const int max_redirects = 5;  // 最大重定向次数

    // 保存原始配置
    httpc_err_t result = HTTPC_ERR_REDIRECT;
    httpc_client_t* new_client = NULL;

    while (redirect_count < max_redirects) {
        // 发送当前请求
        result = httpc_single_request(client, resp_buf, resp_buf_len, actual_read);

        if (result != HTTPC_SUCCESS) {
            break;
        }

        // 解析响应
        httpc_response_t current_response;
        result = httpc_parse_response(resp_buf, &current_response);

        if (result != HTTPC_SUCCESS) {
            break;
        }

        // 检查是否需要重定向
        if (current_response.status_code == 301 || current_response.status_code == 302) {
            if (strlen(current_response.location) == 0) {
                result = HTTPC_ERR_REDIRECT;
                break;
            }

            // 解析重定向URL
            char new_host[256] = {0};
            char new_path[1024] = {0};
            int new_is_https = 0;

            // 处理相对URL和绝对URL
            if (current_response.location[0] == '/') {
                // 相对路径，保持当前主机
                strcpy(new_host, client->config.server_host);
                strncpy(new_path, current_response.location, sizeof(new_path) - 1);
                new_is_https = client->config.is_https;
            } else {
                // 绝对路径，解析新主机
                result = parse_url(current_response.location, new_host, sizeof(new_host),
                                 new_path, sizeof(new_path), &new_is_https);

                if (result != HTTPC_SUCCESS) {
                    break;
                }
            }

            if (client->config.debug_level) {
                printf("[REDIRECT] %d: %s -> %s (Path: %s)\n",
                       current_response.status_code,
                       client->config.server_host,
                       new_host,
                       new_path);
            }

            // 更新客户端配置
            httpc_config_t new_config = client->config;
            new_config.server_host = new_host;
            new_config.url_path = new_path;
            new_config.is_https = new_is_https;
            new_config.request = NULL;

            // 重新连接到新主机
            if(new_client)
                httpc_client_free(new_client);
            new_client = httpc_client_init(&new_config);
            if (!new_client) {
                result = HTTPC_ERR_CONNECT;
                break;
            }

            // 释放旧客户端，使用新客户端
            client = new_client;

            redirect_count++;
        } else {
            // 不需要重定向，成功完成
            result = HTTPC_SUCCESS;
            break;
        }
    }

    if (redirect_count >= max_redirects) {
        result = HTTPC_ERR_TOO_MANY_REDIRECTS;
    }

    if(new_client)
        httpc_client_free(new_client);

    // 恢复原始配置（如果需要的话）
    // 注意：这里我们不恢复客户端状态，因为请求可能已经成功
    return result;
}

// URL编码函数
char* httpc_url_encode(const char* str) {
    if (!str) return NULL;

    size_t len = strlen(str);
    char* encoded = malloc(len * 3 + 1); // 最坏情况：所有字符都需要编码
    if (!encoded) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded[j++] = c;
        } else if (c == ' ') {
            encoded[j++] = '+';
        } else {
            snprintf(encoded + j, 4, "%%%02X", (unsigned char)c);
            j += 3;
        }
    }
    encoded[j] = '\0';

    return encoded;
}

// 简单的语言检测
const char* httpc_detect_language(const char* text) {
    if (!text) return "en";

    int chinese = 0, english = 0, total = 0;

    for (const char* p = text; *p; p++) {
        if ((*p & 0x80) && (*(p+1) & 0x80)) {
            // 简单的中文字符检测（UTF-8）
            chinese++;
            p++; // 跳过下一个字节
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            english++;
        }
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') total++;
    }

    if (total == 0) return "en";
    if ((double)chinese / total > 0.3) return "zh-cn";
    if ((double)english / total > 0.5) return "en";
    return "en";
}

/**
 * @brief 从JSON响应中提取translatedText字段
 * @param json_response JSON响应字符串
 * @return 提取的翻译文本（需要调用者释放内存）
 */
char* httpc_extract_translation(const char* json_response) {
    if (!json_response) return NULL;

    // 查找 "translatedText":"..." 模式
    const char* pattern = "\"translatedText\":\"";
    char* start = strstr(json_response, pattern);
    if (!start) return NULL;

    start += strlen(pattern);

    // 查找结束引号
    char* end = strchr(start, '"');
    if (!end) return NULL;

    // 处理Unicode转义如 \u4f60\u597d
    size_t len = end - start;
    char* result = malloc(len + 1);
    if (!result) return NULL;

    // 复制并解码Unicode转义
    httpc_decode_unicode(start, len, result, len + 1);

    return result;
}

// Extract pattern using string search - similar to re.search in Python
int httpc_extract_pattern(const char* content, const char* start_pattern, const char* end_pattern, char* result, size_t result_len) {
    if (!content || !start_pattern || !end_pattern || !result) return 0;

    const char* start = strstr(content, start_pattern);
    if (!start) return 0;

    start += strlen(start_pattern);
    const char* end = strstr(start, end_pattern);
    if (!end) return 0;

    size_t len = end - start;
    if (len > 0 && len < result_len) {
        return httpc_decode_unicode(start, len, result, result_len);
    }

    return 0;
}

int httpc_decode_unicode(const char* start, size_t len, char* result, size_t result_len) {
    // 复制并解码Unicode转义
    size_t j = 0;
    strncpy(result, start, len);
    for (size_t i = 0; i < len; i++) {
        if (i + 5 < len && start[i] == '\\' && start[i+1] == 'u') {
            // Unicode转义处理
            int unicode;
            if (sscanf(start + i + 2, "%4x", &unicode) == 1) {
                if (unicode >= 0x0000 && unicode <= 0x007F) {
                    // ASCII字符（1字节）
                    result[j++] = (char)unicode;
                } else if (unicode >= 0x0080 && unicode <= 0x07FF) {
                    // 2字节UTF-8
                    result[j++] = 0xC0 | ((unicode >> 6) & 0x1F);
                    result[j++] = 0x80 | (unicode & 0x3F);
                } else if (unicode >= 0x0800 && unicode <= 0xFFFF) {
                    // 3字节UTF-8（包括中文）
                    result[j++] = 0xE0 | ((unicode >> 12) & 0x0F);
                    result[j++] = 0x80 | ((unicode >> 6) & 0x3F);
                    result[j++] = 0x80 | (unicode & 0x3F);
                } else if (unicode >= 0x10000 && unicode <= 0x10FFFF) {
                    // 4字节UTF-8（补充字符）
                    result[j++] = 0xF0 | ((unicode >> 18) & 0x07);
                    result[j++] = 0x80 | ((unicode >> 12) & 0x3F);
                    result[j++] = 0x80 | ((unicode >> 6) & 0x3F);
                    result[j++] = 0x80 | (unicode & 0x3F);
                } else {
                    // 不支持的Unicode的备用处理
                    result[j++] = '?';
                }
                i += 5; // 跳过 \uXXXX
            } else {
                result[j++] = start[i];
            }
        } else {
            result[j++] = start[i];
        }
    }
    result[j] = '\0';

    return 1;
}

static int is_utf8(const char* str, size_t len) {
    if (str == NULL) return 0;
    if (len == 0) len = strlen(str);

    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)str[i];
        int need = 0;

        if (c < 0x80) { // 单字节 ASCII
            need = 1;
        } else if (c < 0xE0) { // 双字节 UTF-8
            need = 2;
        } else if (c < 0xF0) { // 三字节 UTF-8（中文核心）
            need = 3;
        } else if (c < 0xF8) { // 四字节 UTF-8
            need = 4;
        } else {
            return 0; // 非法 UTF-8 首字节
        }

        // 检查剩余长度是否足够
        if (i + need > len) return 0;
        // 检查后续字节是否符合 10xxxxxx 格式
        for (int j = 1; j < need; j++) {
            if (((unsigned char)str[i + j] & 0xC0) != 0x80) {
                return 0;
            }
        }
        i += need;
    }
    return 1;
}

static int gbk_to_utf8(const char* gbk_str, char* utf8_buf, size_t buf_len) {
    if (gbk_str == NULL || utf8_buf == NULL || buf_len == 0) return -1;

#ifdef _WIN32
    // Windows 平台：GBK(CP936) → 宽字符 → UTF-8
    int wlen = MultiByteToWideChar(936, 0, gbk_str, -1, NULL, 0);
    if (wlen <= 0) return -1;

    wchar_t* wbuf = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (wbuf == NULL) return -1;
    MultiByteToWideChar(936, 0, gbk_str, -1, wbuf, wlen);

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, utf8_buf, (int)buf_len, NULL, NULL);
    free(wbuf);
    return (utf8_len > 0) ? utf8_len - 1 : -1; // 减去终止符

#else
    // Linux/嵌入式：iconv 实现 GBK → UTF-8
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == (iconv_t)-1) return -1;

    char* in_buf = (char*)gbk_str;
    size_t in_len = strlen(gbk_str);
    char* out_buf = utf8_buf;
    size_t out_len = buf_len - 1; // 留终止符

    size_t ret = iconv(cd, &in_buf, &in_len, &out_buf, &out_len);
    iconv_close(cd);

    if (ret == (size_t)-1) return -1;
    *out_buf = '\0'; // 加终止符
    return (buf_len - 1 - out_len); // 返回实际转换长度
#endif
}

// 转utf-8
int httpc_any_to_utf8(const char* input_str, char* output_buf, size_t buf_len) {
    if (input_str == NULL || output_buf == NULL || buf_len == 0) return -1;

    // 1. 先检测是否已经是 UTF-8
    if (is_utf8(input_str, 0)) {
        size_t len = strlen(input_str);
        if (len >= buf_len) return -1; // 缓冲区不足
        strcpy(output_buf, input_str);
        return (int)len;
    }

    // 2. 非 UTF-8 → 按 GBK 转 UTF-8
    return gbk_to_utf8(input_str, output_buf, buf_len);
}
