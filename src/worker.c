#include "bench.h"
#include "log.h"
#include <time.h> 

static inline double get_time_ms_fast() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts); 
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static inline void fast_int_to_str(int value, char *buf) {
    char temp[16];
    int i = 0;
    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (value > 0) {
        temp[i++] = (value % 10) + '0';
        value /= 10;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = temp[--i];
    }
    buf[j] = '\0';
}

static inline unsigned long djb2_hash(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; 
    return hash;
}

static inline int fast_ulong_to_hex(unsigned long value, char *buf) {
    static const char hex_digits[] = "0123456789abcdef";
    char temp[16];
    int i = 0;
    do {
        temp[i++] = hex_digits[value & 0xF];
        value >>= 4;
    } while (value > 0);
    int len = i;
    int j = 0;
    while (i > 0) {
        buf[j++] = temp[--i];
    }
    buf[j] = '\0';
    return len;
}

static int classify_obs_status(obs_status status) {
    if (status == OBS_STATUS_OK) { 
        return 200;
    }
    
    if (status >= OBS_STATUS_BUTT) {
        return -1;
    }

    switch (status) {
        case OBS_STATUS_AccessDenied: return 403;
        case OBS_STATUS_NoSuchBucket:
        case OBS_STATUS_NoSuchKey:
        case OBS_STATUS_NoSuchUpload: return 404;
        case OBS_STATUS_BucketAlreadyExists:
        case OBS_STATUS_BucketNotEmpty: return 409;
        case OBS_STATUS_InvalidParameter:
        case OBS_STATUS_EntityTooSmall:
        case OBS_STATUS_EntityTooLarge:
        case OBS_STATUS_MetadataTooLarge:
        case OBS_STATUS_InvalidTargetBucketForLogging:
        case OBS_STATUS_InlineDataTooLarge:
        case OBS_STATUS_InvalidPart:
        case OBS_STATUS_InvalidPartOrder: return 400;
        case OBS_STATUS_InternalError:
        case OBS_STATUS_ServiceUnavailable: return 500;
        default: return -1; 
    }
}

#define CHECK_TIMEOUT(args) \
    if ((args)->config->run_seconds > 0) { \
        if (get_time_ms_fast() > (args)->stop_timestamp_ms) { \
            goto EXIT_WORKER; \
        } \
    }

static void handle_result_stats(WorkerArgs *args, obs_status status, long long loop, int step, int batch, int current_case) {
    int type = classify_obs_status(status);
    
    if (type == 200) {
        args->stats.success_count++;
    } else {
        switch (type) {
            case 403: 
                args->stats.fail_403_count++; 
                LOG_DEBUG("Req Failed (403 AccessDenied): %s", obs_get_status_name(status));
                break;
            case 404: 
                args->stats.fail_404_count++; 
                LOG_DEBUG("Req Failed (404 NotFound): %s", obs_get_status_name(status));
                break;
            case 409: 
                args->stats.fail_409_count++; 
                LOG_DEBUG("Req Failed (409 Conflict): %s", obs_get_status_name(status));
                break;
            case 400: 
                args->stats.fail_4xx_other_count++; 
                LOG_DEBUG("Req Failed (Other 4xx): %s", obs_get_status_name(status));
                break;
            case 500: 
                args->stats.fail_5xx_count++; 
                LOG_WARN("Req Failed (5xx Server): %s", obs_get_status_name(status));
                break;
            default: 
                args->stats.fail_other_count++; 
                LOG_WARN("Req Failed (Net/Other): %s", obs_get_status_name(status));
                break;
        }

        if (batch >= 0) {
            LOG_DEBUG("Context: Thread %d, Loop %lld, Step %d, Batch %d", args->thread_id, loop, step, batch);
        } else {
            LOG_DEBUG("Context: Thread %d, Req %lld", args->thread_id, loop);
        }
    }
}

void *worker_routine(void *arg) {
    WorkerArgs *args = (WorkerArgs *)arg;
    Config *cfg = args->config;
    
    // [新增] 注册线程上下文，后续日志自动携带 [TID:xx|User:xx]
    log_set_context(args->thread_id, args->username);
    
    memset(&args->stats, 0, sizeof(ThreadStats));
    args->stats.min_latency_ms = 999999999.0;

    LOG_INFO("Worker started. Target Bucket: %s", args->effective_bucket);

    int need_alloc = 0;
    if (cfg->use_mix_mode) {
        if (cfg->object_size > 0) need_alloc = 1;
    } else {
        if (cfg->test_case == TEST_CASE_PUT || cfg->test_case == TEST_CASE_MULTIPART) need_alloc = 1;
    }

    if (need_alloc) { 
        long long buf_size = (cfg->test_case == TEST_CASE_MULTIPART) ? cfg->part_size : cfg->object_size;
        if (buf_size <= 0) buf_size = 1;
        
        args->data_buffer = (char *)malloc(buf_size);
        if (args->data_buffer) {
            memset(args->data_buffer, 'X', buf_size);
        } else {
            LOG_ERROR("Worker %d failed alloc memory.", args->thread_id);
            return NULL;
        }
    }

    // [修改] Key 前缀生成：增加 username，避免多用户写入冲突
    char part2_prefix_buf[256];
    snprintf(part2_prefix_buf, sizeof(part2_prefix_buf), "%s-t%d-%s-", args->username, args->thread_id, cfg->key_prefix);
    int part2_prefix_len = strlen(part2_prefix_buf);

    char final_key[512]; 
    char part2_full_buf[256];
    strcpy(part2_full_buf, part2_prefix_buf);

    int use_hash = cfg->obj_name_pattern_hash;

    // =========================================================
    // 压测循环逻辑
    // =========================================================
    
    if (cfg->use_mix_mode) {
        // === 混合模式 (三层循环) ===
        for (long long i = 0; i < cfg->mix_loop_count; i++) {
            CHECK_TIMEOUT(args); 
            for (int j = 0; j < cfg->mix_op_count; j++) {
                CHECK_TIMEOUT(args); 
                int current_case = cfg->mix_ops[j];
                for (int k = 0; k < cfg->requests_per_thread; k++) {
                    CHECK_TIMEOUT(args); 

                    fast_int_to_str(k, part2_full_buf + part2_prefix_len);
                    
                    char *key_ptr = final_key;
                    if (use_hash) {
                        unsigned long hash_val = djb2_hash(part2_full_buf);
                        int hash_len = fast_ulong_to_hex(hash_val, final_key);
                        final_key[hash_len] = '-';
                        strcpy(final_key + hash_len + 1, part2_full_buf);
                        key_ptr = final_key;
                    } else {
                        key_ptr = part2_full_buf;
                    }

                    double start = get_time_ms_fast();
                    obs_status status = OBS_STATUS_BUTT;

                    switch (current_case) {
                        case TEST_CASE_PUT: status = run_put_benchmark(args, key_ptr); break;
                        case TEST_CASE_GET: status = run_get_benchmark(args, key_ptr); break;
                        case TEST_CASE_DELETE: status = run_delete_benchmark(args, key_ptr); break;
                        case TEST_CASE_MULTIPART: status = run_multipart_benchmark(args, key_ptr); break;
                        case TEST_CASE_RESUMABLE: status = run_upload_file_benchmark(args, key_ptr); break;
                        default: break;
                    }

                    double end = get_time_ms_fast();
                    double latency = end - start;

                    handle_result_stats(args, status, i, j, k, current_case);

                    args->stats.total_latency_ms += latency;
                    if (latency > args->stats.max_latency_ms) args->stats.max_latency_ms = latency;
                    if (latency < args->stats.min_latency_ms) args->stats.min_latency_ms = latency;
                }
            }
        }
    } else {
        // === 标准模式 (单层循环) ===
        for (long long k = 0; k < cfg->requests_per_thread; k++) {
            CHECK_TIMEOUT(args); 

            fast_int_to_str((int)k, part2_full_buf + part2_prefix_len);
            
            char *key_ptr = final_key;
            if (use_hash) {
                unsigned long hash_val = djb2_hash(part2_full_buf);
                int hash_len = fast_ulong_to_hex(hash_val, final_key);
                final_key[hash_len] = '-';
                strcpy(final_key + hash_len + 1, part2_full_buf);
                key_ptr = final_key;
            } else {
                key_ptr = part2_full_buf;
            }

            int current_case = cfg->test_case;
            double start = get_time_ms_fast();
            obs_status status = OBS_STATUS_BUTT;

            switch (current_case) {
                case TEST_CASE_PUT: status = run_put_benchmark(args, key_ptr); break;
                case TEST_CASE_GET: status = run_get_benchmark(args, key_ptr); break;
                case TEST_CASE_DELETE: status = run_delete_benchmark(args, key_ptr); break;
                case TEST_CASE_MULTIPART: status = run_multipart_benchmark(args, key_ptr); break;
                case TEST_CASE_RESUMABLE: status = run_upload_file_benchmark(args, key_ptr); break;
                default: 
                    if (current_case == 4) status = run_list_benchmark(args);
                    else LOG_WARN("Unknown TestCase: %d", current_case);
                    break;
            }

            double end = get_time_ms_fast();
            double latency = end - start;

            handle_result_stats(args, status, k, -1, -1, current_case);

            args->stats.total_latency_ms += latency;
            if (latency > args->stats.max_latency_ms) args->stats.max_latency_ms = latency;
            if (latency < args->stats.min_latency_ms) args->stats.min_latency_ms = latency;
        }
    }

EXIT_WORKER:
    if (args->config->run_seconds > 0 && get_time_ms_fast() > args->stop_timestamp_ms) {
        LOG_INFO("Worker stopped (Time Limit Reached).");
    } else {
        LOG_INFO("Worker finished.");
    }

    if (args->data_buffer) {
        free(args->data_buffer);
        args->data_buffer = NULL;
    }

    return NULL;
}

