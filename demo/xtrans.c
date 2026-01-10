#include "xhttpc.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>    // 解决 isalnum 未定义警告
#include <wchar.h>    // 宽字符相关
#include <locale.h>   // setlocale 所需

#ifdef _WIN32
// Windows 平台：用系统 API
#include <windows.h>
#else
// Linux/嵌入式：用 iconv
#include <iconv.h>
#include <errno.h>
//gcc httpc.c - o httpc - lmbedtls - liconv；
#endif

#define RESP_BUF_SIZE 16384  // 足够存储必应词典响应
#define MAX_URL_ENCODE_LEN 1024

static int is_chinese_string(const char* str) {
    if (str == NULL || *str == '\0') return 0;
    while (*str != '\0') {
        unsigned char c = (unsigned char)*str;
        // UTF-8中文编码规则：
        // 中文是3字节编码，首字节范围 0xE4~0xE9（228~233），后续字节 0x80~0xBF
        if (c >= 0xE4 && c <= 0xE9) {
            // 可选：校验后续2个字节是否为合法UTF-8（增强鲁棒性）
            if (*(str + 1) != '\0' && *(str + 2) != '\0' &&
                ((unsigned char)*(str + 1) >= 0x80 && (unsigned char)*(str + 1) <= 0xBF) &&
                ((unsigned char)*(str + 2) >= 0x80 && (unsigned char)*(str + 2) <= 0xBF)) {
                return 1;
            }
        }

        // 跳过UTF-8多字节的后续字节（优化遍历效率）
        if (c < 0x80) {
            str++; // 单字节（ASCII）
        } else if (c < 0xE0) {
            str += 2; // 2字节UTF-8（非中文）
        } else if (c < 0xF0) {
            str += 3; // 3字节UTF-8（中文/其他）
        } else {
            str += 4; // 4字节UTF-8（极少用）
        }
    }
    return 0;
}

static size_t url_encode(const char* src, char* dst, size_t dst_len) {
    if (src == NULL || dst == NULL || dst_len == 0) return 0;
    const char* hex = "0123456789ABCDEF";
    size_t i = 0, j = 0;
    while (src[i] != '\0' && j < dst_len - 1) {
        if (isalnum(src[i]) || src[i] == '-' || src[i] == '_' || src[i] == '.' || src[i] == '~') {
            dst[j++] = src[i];
        } else {
            if (j + 3 > dst_len - 1) break; // 预留%XX的空间
            dst[j++] = '%';
            dst[j++] = hex[(src[i] >> 4) & 0x0F];
            dst[j++] = hex[src[i] & 0x0F];
        }
        i++;
    }
    dst[j] = '\0';
    return j;
}

static size_t wchar_to_utf8(wchar_t wc, char* utf8_buf, size_t buf_len) {
    if (utf8_buf == NULL || buf_len == 0) return 0;

    // ASCII 字符
    if (wc <= 0x7F) {
        if (buf_len < 2) return 0;
        utf8_buf[0] = (char)wc;
        utf8_buf[1] = '\0';
        return 1;
    }
    // 2 字节 UTF-8
    else if (wc <= 0x7FF) {
        if (buf_len < 3) return 0;
        utf8_buf[0] = 0xC0 | ((wc >> 6) & 0x1F);
        utf8_buf[1] = 0x80 | (wc & 0x3F);
        utf8_buf[2] = '\0';
        return 2;
    }
    // 3 字节 UTF-8（覆盖大部分中文）
    else if (wc <= 0xFFFF) {
        if (buf_len < 4) return 0;
        utf8_buf[0] = 0xE0 | ((wc >> 12) & 0x0F);
        utf8_buf[1] = 0x80 | ((wc >> 6) & 0x3F);
        utf8_buf[2] = 0x80 | (wc & 0x3F);
        utf8_buf[3] = '\0';
        return 3;
    }
    // 4 字节 UTF-8（极少用）
    else if (wc <= 0x10FFFF) {
        if (buf_len < 5) return 0;
        utf8_buf[0] = 0xF0 | ((wc >> 18) & 0x07);
        utf8_buf[1] = 0x80 | ((wc >> 12) & 0x3F);
        utf8_buf[2] = 0x80 | ((wc >> 6) & 0x3F);
        utf8_buf[3] = 0x80 | (wc & 0x3F);
        utf8_buf[4] = '\0';
        return 4;
    }
    return 0;
}

// 支持中文的 URL 编码（输入：宽字符字符串；输出：URL 编码后的 char*）
static size_t url_encode_utf8(const wchar_t* src_w, char* dst, size_t dst_len) {
    if (src_w == NULL || dst == NULL || dst_len == 0) return 0;
    const char* hex = "0123456789ABCDEF";
    size_t j = 0; // 目标缓冲区索引
    char utf8_buf[5] = { 0 }; // 存储单个宽字符的 UTF-8 编码

    while (*src_w != L'\0' && j < dst_len - 1) {
        // 步骤1：将宽字符转换为 UTF-8 多字节
        size_t utf8_len = wchar_to_utf8(*src_w, utf8_buf, sizeof(utf8_buf));
        if (utf8_len == 0) {
            src_w++;
            continue;
        }

        // 步骤2：对 UTF-8 每个字节做 URL 转义
        for (size_t k = 0; k < utf8_len; k++) {
            unsigned char c = (unsigned char)utf8_buf[k];
            // 安全字符：字母、数字、- _ . ~
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                if (j >= dst_len - 1) break;
                dst[j++] = c;
            } else {
                // 非安全字符：%XX 转义（需预留 3 个字节空间）
                if (j + 3 > dst_len - 1) break;
                dst[j++] = '%';
                dst[j++] = hex[(c >> 4) & 0x0F];
                dst[j++] = hex[c & 0x0F];
            }
        }
        src_w++;
    }

    dst[j] = '\0'; // 字符串终止符
    return j;
}

// 兼容原始 char* 输入的封装（需确保输入是 UTF-8 编码）
static size_t url_encode_compat(const char* src, char* dst, size_t dst_len) {
    if (src == NULL || dst == NULL || dst_len == 0) return 0;
    // 初始化本地化（确保宽字符转换正常）
    setlocale(LC_ALL, "zh_CN.UTF-8");

    // 步骤1：char* (UTF-8) 转 wchar_t
    size_t src_len = strlen(src);
    wchar_t* src_w = (wchar_t*)malloc((src_len + 1) * sizeof(wchar_t));
    if (src_w == NULL) return 0;
    mbstowcs(src_w, src, src_len + 1);

    // 步骤2：调用宽字符版 URL 编码
    size_t ret = url_encode_utf8(src_w, dst, dst_len);

    // 清理
    free(src_w);
    return ret;
}

static int parse_bing_translate(const char* resp, char* result, size_t result_len) {
    if (resp == NULL || result == NULL || result_len == 0) return 0;
    memset(result, 0, result_len);

    // ===================== 第一步：优先从 meta description 提取（最稳定） =====================
    const char* meta_desc_start = "<meta name=\"description\" content=\"";
    const char* meta_desc_end = "\" />";
    char* desc_start = strstr(resp, meta_desc_start);
    if (desc_start != NULL) {
        desc_start += (strlen(meta_desc_start)); // 跳过起始标签
        const char* desc_end = strstr(desc_start, meta_desc_end);
        if (desc_end != NULL) {
            // 截取 meta description 内容
            char desc_buf[1024] = { 0 };
            size_t desc_len = desc_end - desc_start;
            if (desc_len > sizeof(desc_buf) - 1) desc_len = sizeof(desc_buf) - 1;
            strncpy(desc_buf, desc_start, desc_len);

            // ========== 解析 description 中的翻译结果 ==========
            // 场景1：英文→中文（格式：hello的释义，美/英音标，int. 你好；喂；...；网络释义：...）
            const char* zh_tag = "int. "; // 英文词性标记（int./n./v.等）
            const char* zh_start = strstr(desc_buf, zh_tag);
            if (zh_start != NULL) {
                zh_start += strlen(zh_tag);
                // 截取到「；网络释义：」前（中文翻译核心区）
                const char* zh_end = strstr(zh_start, u8"；网络释义：");
                if (zh_end == NULL) zh_end = strstr(zh_start, u8"; 网络释义："); // 兼容半角分号
                if (zh_end != NULL) {
                    size_t ret_len = zh_end - zh_start;
                    if (ret_len > result_len - 1) ret_len = result_len - 1;
                    strncpy(result, zh_start, ret_len);
                    return 1;
                }
            }

            // 场景2：中文→英文（格式：你好的释义，英/美音标，n. Hello；Hi；...；网络释义：...）
            const char* en_tag = "n. "; // 中文译英文的词性标记（n./v./int.等）
            const char* en_start = strstr(desc_buf, en_tag);
            if (en_start != NULL) {
                en_start += strlen(en_tag);
                // 截取到「；网络释义：」前（英文翻译核心区）
                const char* en_end = strstr(en_start, u8"；网络释义：");
                if (en_end == NULL) en_end = strstr(en_start, u8"; 网络释义：");
                if (en_end != NULL) {
                    size_t ret_len = en_end - en_start;
                    if (ret_len > result_len - 1) ret_len = result_len - 1;
                    strncpy(result, en_start, ret_len);
                    return 1;
                }
            }
        }
    }

    // ===================== 第二步：兜底从正文「网络释义」提取 =====================
    const char* web_def_start = "<meta name=\"description\" content=\"";
    const char* web_start = strstr(resp, web_def_start);
    if (web_start != NULL) {
        web_start += strlen(web_def_start);
        const char* web_end = strstr(web_start, "；");
        if (web_end == NULL) web_end = strstr(web_start, "\" \/>"); // 到下一个HTML标签截止
        if (web_end != NULL) {
            size_t web_len = web_end - web_start;
            if (web_len > result_len - 1) web_len = result_len - 1;
            strncpy(result, web_start, web_len);
            return 1;
        }
    }

    return 0; // 解析失败
}

#ifdef _WIN32
#include <windows.h>
void set_console_font_to_consolas() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole == INVALID_HANDLE_VALUE) {
        return;
    }

    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof(CONSOLE_FONT_INFOEX);
    if (!GetCurrentConsoleFontEx(hConsole, FALSE, &cfi)) {
        return;
    }

    wcscpy_s(cfi.FaceName, LF_FACESIZE, L"Consolas");
    cfi.dwFontSize.X = 12;
    cfi.dwFontSize.Y = 24;
    cfi.FontWeight = FW_NORMAL;

    SetCurrentConsoleFontEx(hConsole, FALSE, &cfi);
}
#endif

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

/**
 * @brief 统一将 GBK/UTF-8 字符串转为 UTF-8（对外接口）
 * @param input_str 输入字符串（GBK/UTF-8）
 * @param output_buf 输出 UTF-8 缓冲区
 * @param buf_len 缓冲区长度
 * @return 成功: 转换后长度, 失败: -1
 */
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

/**
 * @brief URL 编码（UTF-8 输入，适配 HTTP 传输）
 * @param utf8_str UTF-8 字符串
 * @param dst 输出缓冲区
 * @param dst_len 缓冲区长度
 * @return 成功: 编码后长度, 失败: -1
 */
int httpc_url_encode(const char* utf8_str, char* dst, size_t dst_len) {
    if (utf8_str == NULL || dst == NULL || dst_len == 0) return -1;

    const char* hex = "0123456789ABCDEF";
    size_t i = 0, j = 0;
    while (utf8_str[i] != '\0' && j < dst_len - 1) {
        unsigned char c = (unsigned char)utf8_str[i];
        // 安全字符：字母、数字、- _ . ~
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = c;
        } else {
            // 非安全字符：%XX 编码（预留 3 字节）
            if (j + 3 > dst_len - 1) return -1;
            dst[j++] = '%';
            dst[j++] = hex[(c >> 4) & 0x0F];
            dst[j++] = hex[c & 0x0F];
        }
        i++;
    }
    dst[j] = '\0';
    return (int)j;
}

void log_info(const char* msg) {
    printf(u8"✅ %s\n", msg ? msg : "无匹配内容");
}

void print_usage(const char* prog_name) {
    printf(u8"用法: %s [--ca-file 证书文件路径] <要查询的英文/中文>\n", prog_name);
    printf(u8"示例1: %s apple                （使用内置证书查询 apple）\n", prog_name);
    printf(u8"示例2: %s --ca-file ./cacert.pem 苹果  （使用指定证书文件查询 苹果）\n", prog_name);
}

httpc_err_t httpc_translate(const char* ca_file, const char* text, char* result, size_t result_len) {
    // 1. 参数校验
    if (text == NULL || strlen(text) == 0 || result == NULL || result_len == 0) {
        return HTTPC_ERR_PARAM;
    }
    memset(result, 0, result_len);

    // 2. 统一转换为 UTF-8
    char utf8_buf[512] = { 0 };
    int utf8_len = httpc_any_to_utf8(text, utf8_buf, sizeof(utf8_buf));
    if (utf8_len < 0) {
        fprintf(stderr, u8"编码转换失败\n");
        return -1;
    }

    // 3. 自动判断翻译方向
    int is_zh = is_chinese_string(utf8_buf);
    char req_url[1024] = { 0 };
    char encoded_text[512] = { 0 };
    url_encode_compat(utf8_buf, encoded_text, sizeof(encoded_text));

    // 4. 构造必应词典请求URL
    // 必应词典接口说明：q=关键词，mkt=zh-CN（地区），setlang=目标语言
    if (is_zh) {
        // 中文→英文：setlang=en
        snprintf(req_url, sizeof(req_url),
            "GET /dict/search?q=%s&mkt=zh-CN&setlang=en HTTP/1.1\r\n"
            "Host: cn.bing.com\r\n"
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36\r\n"
            "Connection: close\r\n\r\n", encoded_text);
    } else {
        // 英文→中文：setlang=zh
        snprintf(req_url, sizeof(req_url),
            "GET /dict/search?q=%s&mkt=zh-CN&setlang=zh HTTP/1.1\r\n"
            "Host: cn.bing.com\r\n"
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36\r\n"
            "Connection: close\r\n\r\n", encoded_text);
    }

    // 5. 初始化HTTP客户端配置
    httpc_config_t config = { 0 };
    config.server_host = "cn.bing.com";
    config.server_port = "443"; // 必应词典仅支持HTTPS
    config.request = req_url;
    config.is_https = 1;
    config.debug_level = 1; // 开启调试（可选）
    config.ca_cert_path = ca_file;

    // 6. 创建HTTP客户端并发送请求
    httpc_client_t* client = httpc_client_init(&config);
    if (client == NULL) {
        return HTTPC_ERR_INIT;
    }

    // 7. 接收响应
    char resp_buf[RESP_BUF_SIZE] = { 0 };
    size_t actual_read = 0;
    httpc_err_t ret = httpc_client_request(client, resp_buf, sizeof(resp_buf), &actual_read);
    if (ret != HTTPC_SUCCESS) {
        httpc_client_free(client);
        return ret;
    }

    // 8. 解析翻译结果
    if (!parse_bing_translate(resp_buf, result, result_len)) {
        fprintf(stderr, resp_buf);
        httpc_client_free(client);
        return HTTPC_ERR_PARSE; // 需在httpc.h中新增该错误码
    }

    // 9. 释放资源
    httpc_client_free(client);
    return HTTPC_SUCCESS;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // ===================== 自动配置控制台 UTF-8 编码（Windows 特有） =====================
    BOOL output_utf8 = SetConsoleOutputCP(CP_UTF8);
    BOOL input_utf8 = SetConsoleCP(CP_UTF8);
    if (!output_utf8) {
        fprintf(stderr, u8"警告：设置控制台输出 UTF-8 失败，中文可能乱码\n");
    }
    // 设置字体为 Consolas，避免 UTF-8 字符显示方块
    set_console_font_to_consolas();
#endif

    if (argc < 2) {
        print_usage(argv[0]);
        return -1;
    }

    // 初始化变量
    const char* ca_cert_path = NULL;  // 证书文件路径（默认 NULL，使用内置证书）
    const char* query_input = NULL;   // 要查询的内容

    // ===================== 解析命令行参数 =====================
    for (int i = 1; i < argc; i++) {
        // 识别 --ca-file 参数
        if (strcmp(argv[i], "--ca-file") == 0) {
            // 检查 --ca-file 后是否跟随证书路径
            if (i + 1 < argc) {
                ca_cert_path = argv[++i];  // 提取证书路径，i 自增跳过路径参数
            }
            else {
                fprintf(stderr, u8"错误: --ca-file 参数后必须指定证书文件路径\n");
                print_usage(argv[0]);
                return -1;
            }
        }
        // 非 --ca-file 参数，视为查询内容
        else if (query_input == NULL) {
            query_input = argv[i];
        }
        // 多余参数忽略（或报错，此处选择忽略）
        else {
            fprintf(stderr, u8"警告: 忽略多余参数 %s\n", argv[i]);
        }
    }

    // ===================== 校验参数有效性 =====================
    // 检查是否提供了查询内容
    if (query_input == NULL) {
        fprintf(stderr, u8"错误: 必须指定要查询的单词/中文\n");
        print_usage(argv[0]);
        return -1;
    }

    // 6. 解析响应并打印结果
    char result[8192] = { 0 };
    size_t result_len = sizeof(result);
    httpc_err_t ret = httpc_translate(ca_cert_path, query_input, result, result_len);
    if (ret== HTTPC_SUCCESS) {
        log_info(result);
    } else {
        log_info(NULL);
    }

    return 0;
}
