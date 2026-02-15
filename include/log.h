#ifndef LOG_H
#define LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_OFF
} LogLevel;

// 初始化日志级别
void log_init(LogLevel level);

// [关键修复] 必须包含此函数声明
void log_set_context(int thread_id, const char *username);

// 将字符串转换为枚举
LogLevel log_level_from_string(const char *level);

// 统一的日志输出函数
void log_message(LogLevel level, const char *file, int line, const char *fmt, ...);

// 宏定义：自动填入文件名和行号
#define LOG_DEBUG(fmt, ...) log_message(LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_message(LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_message(LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_message(LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // LOG_H

