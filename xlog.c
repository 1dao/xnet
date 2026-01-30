#include "xlog.h"
#include <string.h>
#include <time.h>
#include "xmutex.h"

static uint8_t xlog_uart_port = 0;
static uint8_t xlog_level_cur = XLOG_DEBUG;
static xMutex  xlog_mutex;
static BOOL    xlog_inited = FALSE;

// 文件日志相关变量
static int     xlog_file_enable = 0;
static char    xlog_file_path[256] = "/xlog/luat.log";
static size_t  xlog_current_size = 0;
static void*   xlog_file_handle = NULL;

static xlog_hook xlog_hook_func = NULL;
static void*     xlog_hook_userdata = NULL;
static int       xlog_show_timestamp = 1;
static int       xlog_show_color = 1;
static int       xlog_show_thread_name = 0;
#ifdef _WIN32
    static __declspec(thread) char  xlog_thread_name[32] = "main";
#else
    static __thread char           xlog_thread_name[32] = "main";
#endif

// VT100 颜色代码
#define XLOG_COLOR_RED     "\033[31m"
#define XLOG_COLOR_GREEN   "\033[32m"
#define XLOG_COLOR_YELLOW  "\033[33m"
#define XLOG_COLOR_BLUE    "\033[34m"
#define XLOG_COLOR_MAGENTA "\033[35m"
#define XLOG_COLOR_CYAN    "\033[36m"
#define XLOG_COLOR_RESET   "\033[0m"

#define LOGLOG_SIZE 1024*5

static inline void xlog_init_mutex(void) {
    if (!xlog_inited) {
        xnet_mutex_init(&xlog_mutex);
        xlog_inited = TRUE;
    }
}

bool xlog_init(int level, bool file_enable, bool colo_enable, const char* file_path) {
    if (xlog_inited) {
        return true;
    }

    // 初始化互斥锁
    xnet_mutex_init(&xlog_mutex);

    // 设置日志级别
    xlog_level_cur = level;

    // 设置文件日志
    if (file_path) {
        strncpy(xlog_file_path, file_path, sizeof(xlog_file_path) - 1);
        xlog_file_path[sizeof(xlog_file_path) - 1] = '\0';
    }

    xlog_file_enable = file_enable ? 1 : 0;

    // 设置默认值
    xlog_show_timestamp = 1;
    xlog_show_color = colo_enable ? 1 : 0;
    xlog_show_thread_name = 0;
    strcpy(xlog_thread_name, "main");
    xlog_inited = TRUE;

    xlog_info("xlog system initialized, level=%d, file=%s", level,
              file_enable ? file_path : "disabled");

    return true;
}

void xlog_uninit(void) {
    if (!xlog_inited) {
        return;
    }

    xlog_info("xlog system uninitializing");
    xnet_mutex_lock(&xlog_mutex);
    xlog_inited = FALSE;

    if (xlog_file_handle) {
        xfs_fclose(xlog_file_handle);
        xlog_file_handle = NULL;
    }

    xnet_mutex_uninit(&xlog_mutex);
}

// ============================================================================
// 文件系统API实现
// ============================================================================
#include <stdio.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif
int xfs_fseek(void* stream, long offset, int whence) {
    if (!stream) return -1;
    return fseek((FILE*)stream, offset, whence);
}

long xfs_ftell(void* stream) {
    if (!stream) return -1;
    return ftell((FILE*)stream);
}

size_t xfs_fwrite(const void* ptr, size_t size, size_t nmemb, void* stream) {
    if (!stream || !ptr) return 0;
    return fwrite(ptr, size, nmemb, (FILE*)stream);
}

void* xfs_fopen(const char* filename, const char* mode) {
    if (!filename || !mode) return NULL;
    return fopen(filename, mode);
}

int xfs_fclose(void* stream) {
    if (!stream) return -1;
    return fclose((FILE*)stream);
}

int xfs_rename(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath) return -1;
    return rename(oldpath, newpath);
}

int xfs_mkdir(const char* path) {
    if (!path) return -1;

#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

int xfs_fsync(void* stream) {
    if (!stream) return -1;

#ifdef _WIN32
    return fflush((FILE*)stream);
#else
    return fflush((FILE*)stream);
#endif
}

// ============================================================================
// 字符串格式化函数实现
// ============================================================================

int vsnprintf_(char* str, size_t size, const char* format, va_list ap) {
    if (!str || !format) return -1;
    return vsnprintf(str, size, format, ap);
}

int sprintf_(char* str, const char* format, ...) {
    if (!str || !format) return -1;

    va_list args;
    va_start(args, format);
    int result = vsprintf(str, format, args);
    va_end(args);

    return result;
}

// ============================================================================
// UART和CMUX函数声明（需要根据实际情况实现）
// ============================================================================

// 这些函数需要根据你的硬件平台来实现
void luat_uart_write(int port, char* data, size_t len) {
    // 实现UART写入逻辑
    // 示例：输出到stdout
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}

#ifdef LUAT_USE_SHELL
// CMUX相关变量和函数
typedef struct {
    int state;
    int log_state;
} cmux_context_t;

static cmux_context_t cmux_ctx = { 0 };

void luat_cmux_write(int channel, int frame_type, char* data, size_t len) {
    // 实现CMUX写入逻辑
    // 示例：输出到stderr
    fprintf(stderr, "[CMUX] ");
    fwrite(data, 1, len, stderr);
    fflush(stderr);
}
#endif

void xlog_set_uart_port(int port) {
    xlog_uart_port = port;
}

uint8_t xlog_get_uart_port(void) {
    return xlog_uart_port;
}

// 文件日志功能实现
void xlog_set_file_path(const char* path) {
    if (!path) return;
    xlog_init_mutex();

    xnet_mutex_lock(&xlog_mutex);
    strncpy(xlog_file_path, path, sizeof(xlog_file_path) - 1);
    xlog_file_path[sizeof(xlog_file_path) - 1] = '\0';
    xnet_mutex_unlock(&xlog_mutex);
}

const char* xlog_get_file_path(void) {
    return xlog_file_path;
}

void xlog_set_file_enable(int enable) {
    xlog_init_mutex();

    xnet_mutex_lock(&xlog_mutex);
    xlog_file_enable = enable;
    if (!enable && xlog_file_handle) {
        xfs_fclose(xlog_file_handle);
        xlog_file_handle = NULL;
        xlog_current_size = 0;
    }
    xnet_mutex_unlock(&xlog_mutex);
}

int xlog_get_file_enable(void) {
    return xlog_file_enable;
}

// 打开日志文件
static int xlog_open_file(void) {
    if (xlog_file_handle) {
        return 0;
    }

    // 检查并创建目录
    char dir_path[256];
    const char* last_slash = strrchr(xlog_file_path, '/');
    if (last_slash) {
        size_t dir_len = last_slash - xlog_file_path;
        if (dir_len > 0 && dir_len < sizeof(dir_path)) {
            memcpy(dir_path, xlog_file_path, dir_len);
            dir_path[dir_len] = '\0';
            xfs_mkdir(dir_path);
        }
    }

    xlog_file_handle = xfs_fopen(xlog_file_path, "a+");
    if (xlog_file_handle) {
        xfs_fseek(xlog_file_handle, 0, SEEK_END);
        xlog_current_size = xfs_ftell(xlog_file_handle);
        return 0;
    }

    return -1;
}

// 文件轮转函数
void xlog_rotate_file(void) {
    xlog_init_mutex();
    xnet_mutex_lock(&xlog_mutex);

    if (!xlog_file_handle) {
        xnet_mutex_unlock(&xlog_mutex);
        return;
    }

    xfs_fclose(xlog_file_handle);
    xlog_file_handle = NULL;

    // 重命名当前日志文件
    char new_path[256];
    time_t now = time(NULL);
    struct tm* t = localtime(&now);

    snprintf(new_path, sizeof(new_path), "%s.%04d%02d%02d_%02d%02d%02d",
        xlog_file_path,
        t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
        t->tm_hour, t->tm_min, t->tm_sec);

    xfs_rename(xlog_file_path, new_path);

    // 重新打开日志文件
    xlog_open_file();
    xlog_current_size = 0;

    xnet_mutex_unlock(&xlog_mutex);
}

// 写入文件日志
static void xlog_write_to_file(char* s, size_t l) {
    if (!xlog_file_enable) {
        return;
    }

    if (!xlog_file_handle) {
        if (xlog_open_file() != 0) {
            return;
        }
    }

    // 检查文件大小，超过限制则轮转
    if (xlog_current_size + l > XLOG_MAX_FILE_SIZE) {
        xlog_rotate_file();
    }

    if (xlog_file_handle) {
        size_t written = xfs_fwrite(s, 1, l, xlog_file_handle);
        if (written > 0) {
            xlog_current_size += written;
            xfs_fsync(xlog_file_handle); // 确保数据写入存储
        }
    }
}

// 格式化时间戳
static inline void xlog_format_timestamp(char* buffer, size_t size) {
    if (!xlog_show_timestamp) {
        buffer[0] = '\0';
        return;
    }

    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    snprintf(buffer, size, "%02d:%02d:%02d ",
        tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

// 获取日志级别字符串和颜色
static inline const char* xlog_get_level_str(int level, const char** color) {
    static const char* colors[] = {
        XLOG_COLOR_RESET,   // DEBUG - 默认
        XLOG_COLOR_GREEN,   // INFO
        XLOG_COLOR_YELLOW,  // WARN
        XLOG_COLOR_RED,     // ERROR
    };

    static const char* levels[] = { "[DEBUG]", "[INFO]", "[WARN]", "[ERR]" };

    int index = 0;
    switch (level) {
    case XLOG_DEBUG: index = 0; break;
    case XLOG_INFO:  index = 1; break;
    case XLOG_WARN:  index = 2; break;
    case XLOG_ERROR: index = 3; break;
    default:         index = 0; break;
    }

    if (color) {
        *color = xlog_show_color ? colors[index] : "";
    }

    return levels[index];
}

void xlog_nprint(char* s, size_t l) {
    xlog_init_mutex();
    xnet_mutex_lock(&xlog_mutex);

    // 写入文件
    xlog_write_to_file(s, l);

    // 调用钩子函数
    if (xlog_hook_func) {
        xlog_hook_func(XLOG_INFO, "", s, l, xlog_hook_userdata);
    }

#ifdef LUAT_USE_SHELL
    if (cmux_ctx.state == 1 && cmux_ctx.log_state == 1) {
        luat_cmux_write(LUAT_CMUX_CH_LOG, CMUX_FRAME_UIH & ~CMUX_CONTROL_PF, s, l);
    }
    else
#endif
        luat_uart_write(xlog_uart_port, s, l);

    xnet_mutex_unlock(&xlog_mutex);
}

void xlog_write(char* s, size_t l) {
    xlog_init_mutex();
    xnet_mutex_lock(&xlog_mutex);

    // 写入文件
    xlog_write_to_file(s, l);

    // 调用钩子函数
    if (xlog_hook_func) {
        xlog_hook_func(XLOG_INFO, "", s, l, xlog_hook_userdata);
    }

#ifdef LUAT_USE_SHELL
    if (cmux_ctx.state == 1 && cmux_ctx.log_state == 1) {
        luat_cmux_write(LUAT_CMUX_CH_LOG, CMUX_FRAME_UIH & ~CMUX_CONTROL_PF, s, l);
    }
    else
#endif
        luat_uart_write(xlog_uart_port, s, l);

    xnet_mutex_unlock(&xlog_mutex);
}

void xlog_set_level(int level) {
    xlog_level_cur = level;
}

int xlog_get_level() {
    return xlog_level_cur;
}

void xlog_set_hook(xlog_hook hook, void* userdata) {
    xlog_init_mutex();
    xnet_mutex_lock(&xlog_mutex);
    xlog_hook_func = hook;
    xlog_hook_userdata = userdata;
    xnet_mutex_unlock(&xlog_mutex);
}

void xlog_set_show_timestamp(int enable) {
    xlog_show_timestamp = enable;
}

void xlog_set_show_color(int enable) {
    xlog_show_color = enable;
}

void xlog_set_show_thread_name(int enable) {
    xlog_show_thread_name = enable;
}

void xlog_set_thread_name(const char* name) {
    if (name) {
        strncpy(xlog_thread_name, name, sizeof(xlog_thread_name) - 1);
        xlog_thread_name[sizeof(xlog_thread_name) - 1] = '\0';
    }
}

const char* xlog_get_thread_name(void) {
    return xlog_thread_name;
}

void xlog_flush(void) {
    if (xlog_file_handle) {
        xfs_fsync(xlog_file_handle);
    }
}

void xlog_log(int level, const char* tag, const char* _fmt, ...) {
    if (xlog_level_cur > level) return;
    xlog_init_mutex();
    xnet_mutex_lock(&xlog_mutex);

    char log_buffer[LOGLOG_SIZE] = { 0 };
    char* tmp = log_buffer;
    size_t pos = 0;
    size_t remain = sizeof(log_buffer);

    // 添加时间戳
    if (xlog_show_timestamp) {
        char timestamp[32];
        xlog_format_timestamp(timestamp, sizeof(timestamp));
        size_t len = strlen(timestamp);
        if (len < remain) {
            memcpy(tmp, timestamp, len);
            tmp += len;
            pos += len;
            remain -= len;
        }
    }

    // 添加线程名
    if (xlog_show_thread_name && xlog_thread_name[0]) {
        size_t len = snprintf(tmp, remain, "[%s] ", xlog_thread_name);
        if (len < remain) {
            tmp += len;
            pos += len;
            remain -= len;
        }
    }

    // 添加日志级别和标签
    const char* color_code = "";
    const char* level_str = xlog_get_level_str(level, &color_code);

    size_t header_len = snprintf(tmp, remain, "%s%s%s ", color_code, level_str, tag);
    if (header_len < remain) {
        tmp += header_len;
        pos += header_len;
        remain -= header_len;
    }

    // 格式化日志内容
    va_list args;
    va_start(args, _fmt);
    int content_len = vsnprintf_(tmp, remain - 2, _fmt, args);
    va_end(args);

    if (content_len > 0) {
        pos += content_len;
        tmp += content_len;

        // 添加换行和颜色重置
        if (xlog_show_color) {
            strcpy(tmp, "\n" XLOG_COLOR_RESET);
            pos += strlen("\n" XLOG_COLOR_RESET);
        }
        else {
            strcpy(tmp, "\n");
            pos += 1;
        }
    }

    // 写入文件（不包含颜色代码）
    if (xlog_show_color) {
        // 创建不包含颜色代码的版本用于文件输出
        char file_buffer[LOGLOG_SIZE];
        char* file_tmp = file_buffer;
        size_t file_pos = 0;

        // 重新构建不含颜色的日志
        if (xlog_show_timestamp) {
            char timestamp[32];
            xlog_format_timestamp(timestamp, sizeof(timestamp));
            size_t len = strlen(timestamp);
            memcpy(file_tmp, timestamp, len);
            file_tmp += len;
            file_pos += len;
        }

        if (xlog_show_thread_name && xlog_thread_name[0]) {
            size_t len = snprintf(file_tmp, sizeof(file_buffer) - file_pos,
                "[%s] ", xlog_thread_name);
            file_tmp += len;
            file_pos += len;
        }

        size_t len = snprintf(file_tmp, sizeof(file_buffer) - file_pos,
            "%s%s ", level_str, tag);
        file_tmp += len;
        file_pos += len;

        va_start(args, _fmt);
        vsnprintf_(file_tmp, sizeof(file_buffer) - file_pos - 1, _fmt, args);
        va_end(args);

        strcat(file_buffer, "\n");
        xlog_write_to_file(file_buffer, strlen(file_buffer));
    }
    else {
        xlog_write_to_file(log_buffer, pos);
    }

    if (xlog_hook_func) {
        xlog_hook_func(level, tag, log_buffer, pos, xlog_hook_userdata);
    }

#ifdef LUAT_USE_SHELL
    if (cmux_ctx.state == 1 && cmux_ctx.log_state == 1) {
        luat_cmux_write(LUAT_CMUX_CH_LOG, CMUX_FRAME_UIH & ~CMUX_CONTROL_PF, log_buffer, pos);
    }
    else
#endif
    luat_uart_write(xlog_uart_port, log_buffer, pos);
    xnet_mutex_unlock(&xlog_mutex);
}

void xlog_printf(int level, const char* _fmt, ...) {
    if (xlog_level_cur > level) return;

    xlog_init_mutex();
    xnet_mutex_lock(&xlog_mutex);

    size_t len;
    va_list args;
    char log_printf_buff[LOGLOG_SIZE] = { 0 };

    va_start(args, _fmt);
    len = vsnprintf_(log_printf_buff, LOGLOG_SIZE - 2, _fmt, args);
    va_end(args);

    if (len > 0) {
        log_printf_buff[len] = '\n';
        xlog_write_to_file(log_printf_buff, len + 1);

        if (xlog_hook_func) {
            xlog_hook_func(level, "", log_printf_buff, len + 1, xlog_hook_userdata);
        }

#ifdef LUAT_USE_SHELL
        if (cmux_ctx.state == 1 && cmux_ctx.log_state == 1) {
            luat_cmux_write(LUAT_CMUX_CH_LOG, CMUX_FRAME_UIH & ~CMUX_CONTROL_PF, log_printf_buff, len + 1);
        }
        else
#endif
            luat_uart_write(xlog_uart_port, log_printf_buff, len + 1);
    }

    xnet_mutex_unlock(&xlog_mutex);
}

void xlog_dump_all(const char* tag, void* ptr, size_t len) {
    if (ptr == NULL) {
        xlog_log(XLOG_DEBUG, tag, "ptr is NULL");
        return;
    }
    if (len == 0) {
        xlog_log(XLOG_DEBUG, tag, "ptr len is 0");
        return;
    }

    xlog_init_mutex();
    xnet_mutex_lock(&xlog_mutex);

    char buff[256] = { 0 };
    uint8_t* ptr2 = (uint8_t*)ptr;

    xlog_log(XLOG_DEBUG, tag, "Dump %zu bytes:", len);

    for (size_t i = 0; i < len; i++) {
        sprintf_(buff + strlen(buff), "%02X ", ptr2[i]);
        if (i % 16 == 15) {
            xlog_log(XLOG_DEBUG, tag, "  %s", buff);
            buff[0] = 0;
        }
    }
    if (strlen(buff)) {
        xlog_log(XLOG_DEBUG, tag, "  %s", buff);
    }

    xnet_mutex_unlock(&xlog_mutex);
}
