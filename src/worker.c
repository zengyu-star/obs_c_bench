#include "bench.h"
#include <unistd.h>
#include <sys/syscall.h>

#define PATTERN_BUF_SIZE (1 * 1024 * 1024)

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
    
    args->pattern_size = PATTERN_BUF_SIZE;
    args->pattern_mask = PATTERN_BUF_SIZE - 1;
    args->pattern_buffer = (char *)malloc(args->pattern_size);
    
    if (args->pattern_buffer) {
        fill_pattern_buffer(args->pattern_buffer, args->pattern_size, 0);
        args->data_buffer = args->pattern_buffer;
    } else {
        LOG_ERROR("Thread %d failed to allocate pattern buffer", args->thread_id);
        return NULL;
    }

    // 用于数据内容的随机数种子
    unsigned int thread_seed = (unsigned int)(time(NULL) ^ (long)pthread_self());

    obs_status status = OBS_STATUS_OK;
    int op_index = 0;
    
    while (1) {
        // --- 1. 时间/配额检查 ---
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
        double now_ms = ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
        if (now_ms >= args->stop_timestamp_ms) {
            LOG_INFO("Thread %d reaching RunSeconds limit. Stopping...", args->thread_id);
            break;
        }

        if (args->config->requests_per_thread > 0) {
            long long current_requests = args->stats.success_count + 
                                         args->stats.fail_403_count + 
                                         args->stats.fail_404_count + 
                                         args->stats.fail_409_count + 
                                         args->stats.fail_4xx_other_count + 
                                         args->stats.fail_5xx_count + 
                                         args->stats.fail_other_count +
                                         args->stats.fail_validation_count;

            if (current_requests >= args->config->requests_per_thread) {
                LOG_INFO("Thread %d finished its quota (%d requests). Stopping early.", 
                         args->thread_id, args->config->requests_per_thread);
                break;
            }
        }
        
        // --- 2. 动态参数准备 ---
        // A. 随机对象大小
        long long current_req_size;
        if (args->config->is_dynamic_size) {
            current_req_size = args->config->object_size_min + 
                               (rand_r(&thread_seed) % (args->config->object_size_max - args->config->object_size_min + 1));
        } else {
            current_req_size = args->config->object_size_max;
        }

        // B. 混合模式判断
        int current_case = args->config->test_case;
        if (args->config->use_mix_mode) {
             if (op_index >= args->config->mix_op_count * args->config->mix_loop_count) {
                 LOG_INFO("Thread %d finished mix operations. Stopping early.", args->thread_id);
                 break;
             }
             current_case = args->config->mix_ops[op_index % args->config->mix_op_count];
        }

        // C. Key 生成 (优化：使用 LCG 伪随机算法)
        char key[256];
        if (args->config->obj_name_pattern_hash) {
             // 使用 LCG (Linear Congruential Generator) 算法
             // 参数来源：glibc rand() 实现标准 (Multiplier: 1103515245, Increment: 12345)
             // 优势：极高性能（仅1次乘法1次加法），且能将顺序输入（0,1,2）转化为伪随机分布
             unsigned int seed_val = (unsigned int)(args->thread_id + op_index);
             unsigned int lcg_hash = (seed_val * 1103515245 + 12345) & 0x7FFFFFFF;
             
             // 取模 10000 得到 0000-9999 的前缀
             int hash_prefix = lcg_hash % 10000;

             // Pattern: {hash}-{username}-{prefix}-{threadId}
             snprintf(key, sizeof(key), "%04d-%s-%s-%d", hash_prefix, args->username, args->config->key_prefix, args->thread_id);
        } else {
             // 默认模式: {username}-{prefix}-{threadId}
             snprintf(key, sizeof(key), "%s-%s-%d", args->username, args->config->key_prefix, args->thread_id);
        }

        // --- 3. 执行请求 ---
        
        // [关键] 记录执行前的校验失败次数，用于后续排重
        long long prev_val_count = args->stats.fail_validation_count;

        switch(current_case) {
            case TEST_CASE_PUT:
                status = run_put_benchmark(args, key, current_req_size);
                break;
            case TEST_CASE_GET:
                {
                    // 随机 Range 选择
                    char *selected_range = NULL;
                    if (args->config->range_count > 0) {
                        int r_idx = rand_r(&thread_seed) % args->config->range_count;
                        selected_range = args->config->range_options[r_idx];
                    }
                    status = run_get_benchmark(args, key, selected_range);
                }
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

        // --- 4. 统计归类 ---
        if (status == OBS_STATUS_OK) {
            args->stats.success_count++;
            // PUT 操作在此处累加流量，GET 操作在 adapter 中累加
            if (current_case == TEST_CASE_PUT) {
                args->stats.total_success_bytes += current_req_size;
            }
        } else {
            // [关键] 检查是否发生了校验错误
            // 如果 adapter 内部已经增加了校验失败计数，则在此处跳过归类
            // 避免 status == InternalError 时被误判为 Net/SDK Error
            if (args->stats.fail_validation_count > prev_val_count) {
                // 已处理为 DataConsistencyError，不做额外计数
            } 
            else if (status == OBS_STATUS_AccessDenied) {
                args->stats.fail_403_count++;
            } 
            else if (status == OBS_STATUS_NoSuchBucket || status == OBS_STATUS_NoSuchKey || status == OBS_STATUS_NoSuchUpload) {
                args->stats.fail_404_count++;
            } 
            else if (status == OBS_STATUS_BucketAlreadyExists || status == OBS_STATUS_BucketNotEmpty) {
                args->stats.fail_409_count++;
            } 
            else if (status >= 400 && status < 500) {
                args->stats.fail_4xx_other_count++;
            } 
            else if (status >= 500 && status < 600) {
                args->stats.fail_5xx_count++;
            } 
            else {
                // 真正的网络或SDK内部错误
                args->stats.fail_other_count++;
            }
        }
        op_index++;
    }

    if (args->pattern_buffer) free(args->pattern_buffer);
    return NULL;
}

