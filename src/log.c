#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <pthread.h>

static LogLevel g_log_level = LOG_INFO;

// [新增] 线程局部存储
static __thread int g_thread_id = -1;
static __thread char g_username[64] = {0};

void log_init(LogLevel level) {
    g_log_level = level;
}

// [新增] 设置上下文实现
void log_set_context(int thread_id, const char *username) {
    g_thread_id = thread_id;
    if (username) {
        strncpy(g_username, username, sizeof(g_username) - 1);
        g_username[sizeof(g_username) - 1] = '\0';
    } else {
        g_username[0] = '\0';
    }
}

LogLevel log_level_from_string(const char *level) {
    if (level == NULL) return LOG_INFO;
    if (strcmp(level, "DEBUG") == 0) return LOG_DEBUG;
    if (strcmp(level, "INFO") == 0) return LOG_INFO;
    if (strcmp(level, "WARN") == 0) return LOG_WARN;
    if (strcmp(level, "ERROR") == 0) return LOG_ERROR;
    if (strcmp(level, "OFF") == 0) return LOG_OFF;
    return LOG_INFO;
}

void log_message(LogLevel level, const char *file, int line, const char *fmt, ...) {
    if (level < g_log_level) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_buf[64];
    if (t) {
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", t);
    } else {
        strcpy(time_buf, "UNKNOWN_TIME");
    }

    const char *level_str = "UNKNOWN";
    switch(level) {
        case LOG_DEBUG: level_str = "DEBUG"; break;
        case LOG_INFO:  level_str = "INFO "; break;
        case LOG_WARN:  level_str = "WARN "; break;
        case LOG_ERROR: level_str = "ERROR"; break;
        default: break;
    }

    // [新增] 构建上下文前缀
    char context_buf[128];
    if (g_thread_id >= 0) {
        if (g_username[0] != '\0') {
            snprintf(context_buf, sizeof(context_buf), "[TID:%d|%s]", g_thread_id, g_username);
        } else {
            snprintf(context_buf, sizeof(context_buf), "[TID:%d]", g_thread_id);
        }
    } else {
        strcpy(context_buf, "[MAIN]");
    }

    // 输出: [时间] [级别] [Context] [文件:行] 内容
    fprintf(stderr, "[%s] [%s] %s [%s:%d] ", time_buf, level_str, context_buf, file, line);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}

