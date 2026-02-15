#include "bench.h"
#include <unistd.h>
#include <sys/syscall.h>

// [新增] 1MB Pattern Buffer
#define PATTERN_BUF_SIZE (1 * 1024 * 1024)

// [新增] 确定性填充算法
void fill_pattern_buffer(char *buf, size_t size, int seed) {
    unsigned int s = seed;
    const unsigned int A = 1664525;
    const unsigned int C = 1013904223;
    for (size_t i = 0; i < size; i++) {
        buf[i] = (char)((i * A + C + s) % 255);
    }
}

void *worker_routine(void *arg) {
    WorkerArgs *args = (WorkerArgs *)arg;
    
    // [修改] 启用环形缓冲区优化
    args->pattern_size = PATTERN_BUF_SIZE;
    args->pattern_mask = PATTERN_BUF_SIZE - 1;
    args->pattern_buffer = (char *)malloc(args->pattern_size);
    
    if (args->pattern_buffer) {
        // 使用 Seed=0 填充，确保与 Python 脚本一致
        fill_pattern_buffer(args->pattern_buffer, args->pattern_size, 0);
        // 让 data_buffer 指向 pattern_buffer，兼容旧接口参数
        args->data_buffer = args->pattern_buffer;
    } else {
        LOG_ERROR("Thread %d failed to allocate pattern buffer", args->thread_id);
        return NULL;
    }

    obs_status status = OBS_STATUS_OK;
    int op_index = 0;
    
    while (1) {
        // 时间检查
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
        double now_ms = ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
        if (now_ms > args->stop_timestamp_ms) break;

        // 次数检查
        if (args->config->run_seconds == 0 && 
            (args->stats.success_count + args->stats.fail_other_count) >= args->config->requests_per_thread) {
            break;
        }
        
        int current_case = args->config->test_case;
        if (args->config->use_mix_mode) {
             current_case = args->config->mix_ops[op_index % args->config->mix_op_count];
             op_index++;
             if (op_index >= args->config->mix_op_count * args->config->mix_loop_count) break;
        }

        char key[256];
        if (args->config->obj_name_pattern_hash) {
             int hash = (args->thread_id + op_index) % 1000;
             snprintf(key, sizeof(key), "%s-%04d-%s-%d", args->username, hash, args->config->key_prefix, args->thread_id);
        } else {
             snprintf(key, sizeof(key), "%s-%s-%d", args->username, args->config->key_prefix, args->thread_id);
        }

        switch(current_case) {
            case TEST_CASE_PUT:
                status = run_put_benchmark(args, key);
                break;
            case TEST_CASE_GET:
                status = run_get_benchmark(args, key);
                break;
            case TEST_CASE_DELETE:
                status = run_delete_benchmark(args, key);
                break;
            case TEST_CASE_MULTIPART:
                status = run_multipart_benchmark(args, key);
                break;
            case TEST_CASE_RESUMABLE:
                status = run_upload_file_benchmark(args, key);
                break;
            default:
                status = OBS_STATUS_InternalError;
                break;
        }

        if (status == OBS_STATUS_OK) {
            args->stats.success_count++;
        } else {
            if (status == OBS_STATUS_AccessDenied) args->stats.fail_403_count++;
            else if (status == OBS_STATUS_NoSuchBucket || status == OBS_STATUS_NoSuchKey) args->stats.fail_404_count++;
            else if (status == OBS_STATUS_BucketAlreadyExists) args->stats.fail_409_count++;
            else if (status >= 400 && status < 500) args->stats.fail_4xx_other_count++;
            else if (status >= 500 && status < 600) args->stats.fail_5xx_count++;
            else args->stats.fail_other_count++;
        }
    }

    // 释放内存
    if (args->pattern_buffer) free(args->pattern_buffer);
    return NULL;
}

