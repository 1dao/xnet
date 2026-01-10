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
#include "xhttpc_cacert.h"

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
    if (config == NULL || config->server_host == NULL || config->server_port == NULL || config->request == NULL) {
        fprintf(stderr, u8"参数非法（服务器地址/端口/请求不能为空）\n");
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

httpc_err_t httpc_client_request(httpc_client_t* client, char* resp_buf, size_t resp_buf_len, size_t* actual_read) {
    if (client == NULL || !client->is_init || resp_buf == NULL || resp_buf_len == 0) {
        return HTTPC_ERR_PARAM;
    }

    int ret;
    size_t total_read = 0;
    const char* req = client->config.request;
    size_t req_len = strlen(req);

    // 发送请求
    if (client->config.is_https) {
        ret = 0;
        while ((ret = mbedtls_ssl_write(&client->ssl, (const unsigned char*)req, req_len)) <= 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                fprintf(stderr, u8"HTTPS 发送失败: %d\n", ret);
                return HTTPC_ERR_WRITE;
            }
        }
    } else {
        ret = mbedtls_net_send(&client->net_fd, (const unsigned char*)req, req_len);
        if (ret <= 0) {
            fprintf(stderr, u8"HTTP 发送失败: %d\n", ret);
            return HTTPC_ERR_WRITE;
        }
    }

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
