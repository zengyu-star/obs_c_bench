#include "bench.h"
#include <unistd.h>
#include <sys/syscall.h>

#define PATTERN_BUF_SIZE (1 * 1024 * 1024)
#define MAX_ROWS_PER_FILE 1000000

void fill_pattern_buffer(char *buf, size_t size, int seed) {
    unsigned int s = seed;
    const unsigned int A = 1664525;
    const unsigned int C = 1013904223;
    for (size_t i = 0; i < size; i++) {
        buf[i] = (char)((i * A + C + s) % 255);
    }
}

// 精准的 SDK 错误码到 HTTP 状态码映射函数
static int infer_http_code(obs_status status) {
    switch (status) {
        case OBS_STATUS_AccessDenied:
        case OBS_STATUS_InvalidAccessKeyId:
        case OBS_STATUS_SignatureDoesNotMatch:
        case OBS_STATUS_InvalidSecurity:
        case OBS_STATUS_HttpErrorForbidden:
            return 403;
        case OBS_STATUS_NoSuchBucket:
        case OBS_STATUS_NoSuchKey:
        case OBS_STATUS_NoSuchUpload:
        case OBS_STATUS_NoSuchVersion:
        case OBS_STATUS_HttpErrorNotFound:
            return 404;
        case OBS_STATUS_BucketAlreadyExists:
        case OBS_STATUS_BucketAlreadyOwnedByYou:
        case OBS_STATUS_BucketNotEmpty:
        case OBS_STATUS_HttpErrorConflict:
            return 409;
        case OBS_STATUS_InternalError:
        case OBS_STATUS_ServiceUnavailable:
        case OBS_STATUS_SlowDown:
            return 500;
        case OBS_STATUS_InitCurlFailed:
        case OBS_STATUS_FailedToConnect:
        case OBS_STATUS_ConnectionFailed:
        case OBS_STATUS_RequestTimeout:
        case OBS_STATUS_NameLookupError:
            return 0;
        default:
            if (status >= OBS_STATUS_AccessDenied) return 400;
            return 0;
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

    // 规划总请求数逻辑
    long long reqs_per_op = args->config->requests_per_thread > 0 ? args->config->requests_per_thread : 1;
    long long total_planned_requests = 0;
    
    if (args->config->use_mix_mode) {
        total_planned_requests = (long long)args->config->mix_loop_count * args->config->mix_op_count * reqs_per_op;
    } else {
        total_planned_requests = (long long)args->config->requests_per_thread;
    }

    unsigned int thread_seed = (unsigned int)(time(NULL) ^ (long)pthread_self());
    FILE *detail_fp = NULL;
    ReqRecord *batch_buffer = NULL;
    int batch_count = 0;
    int file_part_idx = 0;
    long long total_written_rows = 0;
    char detail_filename[512];

    if (args->config->enable_detail_log) {
        snprintf(detail_filename, sizeof(detail_filename), "%s/detail_%d_part%d.csv", 
                 args->config->task_log_dir, args->thread_id, file_part_idx);
        detail_fp = fopen(detail_filename, "w");
        if (detail_fp) {
            fprintf(detail_fp, "Timestamp(s),OpType,Key,Latency(ms),SDKStatus,HTTPCode,Bytes,RequestID\n");
            batch_buffer = (ReqRecord *)malloc(sizeof(ReqRecord) * BATCH_SIZE);
        }
    }

    obs_status status = OBS_STATUS_OK;
    long long op_index = 0;
    
    while (!g_graceful_stop) {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC_COARSE, &ts_now);
        double now_ms = ts_now.tv_sec * 1000.0 + ts_now.tv_nsec / 1000000.0;
        
        if (now_ms >= args->stop_timestamp_ms) break;
        if (total_planned_requests > 0 && op_index >= total_planned_requests) break;

        int current_case = args->config->test_case;
        char *selected_range = NULL;
        long long object_seq_id = op_index; 

        if (args->config->use_mix_mode) {
            long long current_block_idx = op_index / reqs_per_op;
            int mix_idx = current_block_idx % args->config->mix_op_count;
            current_case = args->config->mix_ops[mix_idx];
            long long current_loop_iteration = op_index / (args->config->mix_op_count * reqs_per_op);
            long long current_req_in_block = op_index % reqs_per_op;
            object_seq_id = current_loop_iteration * reqs_per_op + current_req_in_block;
        }

        // [修改]: 扩容本地 key 缓冲区至支持 1024 字节
        char key[MAX_KEY_LEN]; 
        if (args->config->obj_name_pattern_hash) {
             unsigned int seed_val = (unsigned int)(args->thread_id + object_seq_id);
             unsigned int lcg_hash = (seed_val * 1103515245 + 12345) & 0x7FFFFFFF;
             int hash_prefix = lcg_hash % 10000;
             snprintf(key, sizeof(key), "%04d-%s-%s-%d", hash_prefix, args->username, args->config->key_prefix, args->thread_id);
        } else {
             snprintf(key, sizeof(key), "%s-%s-%d-%lld", args->username, args->config->key_prefix, args->thread_id, object_seq_id);
        }

        long long current_req_size = args->config->is_dynamic_size ? 
            (args->config->object_size_min + (rand_r(&thread_seed) % (args->config->object_size_max - args->config->object_size_min + 1))) : 
            args->config->object_size_max;

        long long prev_val_count = args->stats.fail_validation_count;
        struct timeval tv_abs;
        gettimeofday(&tv_abs, NULL);
        double abs_timestamp = tv_abs.tv_sec + tv_abs.tv_usec / 1000000.0;

        struct timespec ts_start, ts_end;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        char current_req_id[64] = "-";
        int current_http_code = 0;

        switch(current_case) {
            case TEST_CASE_PUT:
                status = run_put_benchmark(args, key, current_req_size, current_req_id);
                break;
            case TEST_CASE_GET:
                if (args->config->range_count > 0) {
                    int r_idx = rand_r(&thread_seed) % args->config->range_count;
                    selected_range = args->config->range_options[r_idx];
                }
                status = run_get_benchmark(args, key, selected_range, current_req_id);
                break;
            case TEST_CASE_DELETE:
                status = run_delete_benchmark(args, key, current_req_id);
                break;
            case TEST_CASE_MULTIPART:
                status = run_multipart_benchmark(args, key, current_req_id);
                break;
            case TEST_CASE_RESUMABLE:
                status = run_upload_file_benchmark(args, key, current_req_id);
                break;
            default:
                status = OBS_STATUS_InternalError;
                break;
        }

        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        double latency_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0 + (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0;

        if (status == OBS_STATUS_OK && args->stats.fail_validation_count > prev_val_count) {
            status = OBS_STATUS_InternalError;
        }

        if (status == OBS_STATUS_OK) {
            if (current_case == TEST_CASE_GET && selected_range != NULL) current_http_code = 206;
            else if (current_case == TEST_CASE_DELETE) current_http_code = 204;
            else current_http_code = 200;
        } else {
            current_http_code = infer_http_code(status);
        }

        if (detail_fp && batch_buffer) {
            batch_buffer[batch_count].timestamp_s = abs_timestamp;
            batch_buffer[batch_count].op_type = current_case;
            // [修改]: 确保拷贝 key 时适配新的长度
            snprintf(batch_buffer[batch_count].key, sizeof(batch_buffer[batch_count].key), "%s", key);
            batch_buffer[batch_count].latency_ms = latency_ms;
            batch_buffer[batch_count].status_code = status;
            batch_buffer[batch_count].http_code = current_http_code;
            batch_buffer[batch_count].bytes = current_req_size;
            snprintf(batch_buffer[batch_count].request_id, sizeof(batch_buffer[batch_count].request_id), "%s", current_req_id);
            batch_count++;

            if (batch_count >= BATCH_SIZE) {
                for (int i = 0; i < batch_count; i++) {
                    fprintf(detail_fp, "%.3f,%d,%s,%.2f,%d,%d,%lld,%s\n",
                            batch_buffer[i].timestamp_s, batch_buffer[i].op_type, batch_buffer[i].key,
                            batch_buffer[i].latency_ms, batch_buffer[i].status_code,
                            batch_buffer[i].http_code, batch_buffer[i].bytes, batch_buffer[i].request_id);
                }
                total_written_rows += batch_count;
                batch_count = 0;
                if (total_written_rows >= MAX_ROWS_PER_FILE) {
                    fclose(detail_fp);
                    file_part_idx++;
                    total_written_rows = 0;
                    snprintf(detail_filename, sizeof(detail_filename), "%s/detail_%d_part%d.csv", args->config->task_log_dir, args->thread_id, file_part_idx);
                    detail_fp = fopen(detail_filename, "w");
                    if (detail_fp) fprintf(detail_fp, "Timestamp(s),OpType,Key,Latency(ms),SDKStatus,HTTPCode,Bytes,RequestID\n");
                }
            }
        }

        if (status == OBS_STATUS_OK) {
            args->stats.success_count++;
            if (current_case == TEST_CASE_PUT) args->stats.total_success_bytes += current_req_size;
        } else {
            if (args->stats.fail_validation_count == prev_val_count) {
                if (current_http_code == 403) args->stats.fail_403_count++;
                else if (current_http_code == 404) args->stats.fail_404_count++;
                else if (current_http_code == 409) args->stats.fail_409_count++;
                else if (current_http_code >= 400 && current_http_code < 500) args->stats.fail_4xx_other_count++;
                else if (current_http_code >= 500 && current_http_code < 600) args->stats.fail_5xx_count++;
                else args->stats.fail_other_count++;
                usleep(50000); 
            }
        }
        op_index++;
    }

    if (detail_fp) {
        if (batch_buffer && batch_count > 0) {
            for (int i = 0; i < batch_count; i++) {
                fprintf(detail_fp, "%.3f,%d,%s,%.2f,%d,%d,%lld,%s\n",
                        batch_buffer[i].timestamp_s, batch_buffer[i].op_type, batch_buffer[i].key,
                        batch_buffer[i].latency_ms, batch_buffer[i].status_code,
                        batch_buffer[i].http_code, batch_buffer[i].bytes, batch_buffer[i].request_id);
            }
        }
        fclose(detail_fp);
    }
    if (batch_buffer) free(batch_buffer);
    if (args->pattern_buffer) free(args->pattern_buffer);
    return NULL;
}

