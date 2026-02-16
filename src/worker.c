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

    // [新增] 线程安全的随机种子
    unsigned int thread_seed = (unsigned int)(time(NULL) ^ (long)pthread_self());

    obs_status status = OBS_STATUS_OK;
    int op_index = 0;
    
    while (1) {
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
        
        // [修改] 动态计算本次请求的大小
        long long current_req_size;
        if (args->config->is_dynamic_size) {
            current_req_size = args->config->object_size_min + 
                               (rand_r(&thread_seed) % (args->config->object_size_max - args->config->object_size_min + 1));
        } else {
            current_req_size = args->config->object_size_max;
        }

        int current_case = args->config->test_case;
        if (args->config->use_mix_mode) {
             if (op_index >= args->config->mix_op_count * args->config->mix_loop_count) {
                 LOG_INFO("Thread %d finished mix operations. Stopping early.", args->thread_id);
                 break;
             }
             current_case = args->config->mix_ops[op_index % args->config->mix_op_count];
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
                // 传入动态大小
                status = run_put_benchmark(args, key, current_req_size);
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
            // [新增] PUT 操作由 Worker 负责累计流量 (因为是我们决定的发送大小)
            // GET 操作由 Adapter 负责累计 (因为是服务端决定的接收大小)
            if (current_case == TEST_CASE_PUT) {
                args->stats.total_success_bytes += current_req_size;
            }
        } else {
            if (status == OBS_STATUS_AccessDenied) args->stats.fail_403_count++;
            else if (status == OBS_STATUS_NoSuchBucket || status == OBS_STATUS_NoSuchKey) args->stats.fail_404_count++;
            else if (status == OBS_STATUS_BucketAlreadyExists) args->stats.fail_409_count++;
            else if (status >= 400 && status < 500) args->stats.fail_4xx_other_count++;
            else if (status >= 500 && status < 600) args->stats.fail_5xx_count++;
            else if (status == OBS_STATUS_InternalError) { } // 已处理
            else args->stats.fail_other_count++;
        }
        op_index++;
    }

    if (args->pattern_buffer) free(args->pattern_buffer);
    return NULL;
}

