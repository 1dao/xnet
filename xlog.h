#ifndef XLOG_H
#define XLOG_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include "fmacros.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif


#define XLOG_DEBUG 1
#define XLOG_INFO  2
#define XLOG_WARN  3
#define XLOG_ERROR 4
#define XLOG_CLOSE 7

#define XLOG_MAX_FILE_SIZE (200 * 1024 * 1024) // 200MB
#define XLOG_MAX_FILE_COUNT 10 // 最多保留10个日志文件
#define XLOG_TAG "tag"

typedef void (*xlog_hook)(int level, const char* tag, const char* message, size_t len, void* userdata);

#ifdef LUAT_USE_LOG2
	#define xlog_err(format, ...)	xlog_printf(XLOG_ERROR, "[ERROR]" XLOG_TAG " " format "\n", ##__VA_ARGS__)
	#define xlog_warn(format, ...)	xlog_printf(XLOG_WARN,  "[WARN]" XLOG_TAG " " format "\n", ##__VA_ARGS__)
	#define xlog_info(format, ...)	xlog_printf(XLOG_INFO,  "[INFO]" XLOG_TAG " " format "\n", ##__VA_ARGS__)
	#define xlog_debug(format, ...) xlog_printf(XLOG_DEBUG, "[DEBUG]" XLOG_TAG " " format "\n", ##__VA_ARGS__)
	void xlog_printf(int level, const char* _fmt, ...);
#else
	void xlog_log(int level, const char* tag, const char* _fmt, ...);

	#define xlog_err(format, ...)	xlog_log(XLOG_ERROR, XLOG_TAG, format, ##__VA_ARGS__)
	#define xlog_warn(format, ...)	xlog_log(XLOG_WARN, XLOG_TAG, format, ##__VA_ARGS__)
	#define xlog_info(format, ...)	xlog_log(XLOG_INFO, XLOG_TAG, format, ##__VA_ARGS__)
	#define xlog_debug(format, ...)	xlog_log(XLOG_DEBUG, XLOG_TAG, format, ##__VA_ARGS__)
	#define xlog_dump(ptr,len) xlog_dump(XLOG_TAG, ptr, len)

	void xlog_dump_all(const char* tag, void* ptr, size_t len);
#endif

bool xlog_init(int level, bool file_enable, bool colo_enable, const char* file_path);
void xlog_uninit(void);
void xlog_nprint(char* s, size_t l);
void xlog_write(char* s, size_t l);
void xlog_set_uart_port(int port);
uint8_t xlog_get_uart_port(void);
void xlog_set_level(int level);
int xlog_get_level(void);

// 文件日志功能
void xlog_set_file_path(const char* path);
const char* xlog_get_file_path(void);
void xlog_set_file_enable(int enable);
int xlog_get_file_enable(void);
void xlog_rotate_file(void);

void xlog_set_hook(xlog_hook hook, void* userdata);
void xlog_set_show_timestamp(int enable);
void xlog_set_show_color(int enable);
void xlog_set_show_thread_name(int enable);
void xlog_set_thread_name(const char* name);
const char* xlog_get_thread_name(void);
void xlog_flush(void);

// 文件系统API声明
int xfs_fseek(void* stream, long offset, int whence);
long xfs_ftell(void* stream);
size_t xfs_fwrite(const void* ptr, size_t size, size_t nmemb, void* stream);
void* xfs_fopen(const char* filename, const char* mode);
int xfs_fclose(void* stream);
int xfs_rename(const char* oldpath, const char* newpath);
int xfs_mkdir(const char* path);
int xfs_fsync(void* stream);

// 字符串格式化函数声明
int vsnprintf_(char* str, size_t size, const char* format, va_list ap);
int sprintf_(char* str, const char* format, ...);

#ifdef __cplusplus
}
#endif
#endif
