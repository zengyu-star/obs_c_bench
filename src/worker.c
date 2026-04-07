#include "bench.h"
#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>

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

// SplitMix64-based kernel: extremely fast, good avalanche properties.
// Perfect for high-performance key prefix generation.
static inline uint64_t fast_mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// Ultra-fast u64 to hex using a static lookup table, bypassing snprintf overhead for hex formatting.
static const char g_hex_lookup[] = "0123456789abcdef";
static inline void fast_u128_to_hex32(uint64_t v1, uint64_t v2, char *out) {
    for (int i = 15; i >= 0; i--) {
        out[i] = g_hex_lookup[v1 & 0xf];
        v1 >>= 4;
        out[i + 16] = g_hex_lookup[v2 & 0xf];
        v2 >>= 4;
    }
}

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
            fprintf(detail_fp, "Timestamp(s),OpType,Bucket,Key,Latency(ms),SDKStatus,HTTPCode,Bytes,RequestID\n");
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

        char key[MAX_KEY_LEN]; 
        if (args->config->obj_name_pattern_hash) {
             // 1. Combine thread_id and object_seq_id for strong determinism
             uint64_t seed = ((uint64_t)args->thread_id << 32) ^ (uint64_t)object_seq_id;
             
             // 2. Generate two 64-bit blocks for a total of 128 bits (32 hex characters)
             // Using golden ratio constant to maximize dispersion
             uint64_t part1 = fast_mix64(seed + 0x9e3779b97f4a7c15ULL);
             uint64_t part2 = fast_mix64(part1 + 0x9e3779b97f4a7c15ULL);

             char hex_prefix[33];
             fast_u128_to_hex32(part1, part2, hex_prefix);
             hex_prefix[32] = '\0';

             snprintf(key, sizeof(key), "%s-%s-%s-%d-%lld", hex_prefix, args->config->key_prefix, args->username, args->thread_id, object_seq_id);
        } else {
             snprintf(key, sizeof(key), "%s-%s-%d-%lld", args->config->key_prefix, args->username, args->thread_id, object_seq_id);
        }

        long long current_req_size = args->config->is_dynamic_size ? 
            (args->config->object_size_min + (rand_r(&thread_seed) % (args->config->object_size_max - args->config->object_size_min + 1))) : 
            args->config->object_size_max;

        // [核心修改]: 若为多段上传，强制替换 current_req_size 为真实产生的数据量，保证带宽统计准确
        if (current_case == TEST_CASE_MULTIPART) {
            long long p_size = args->config->part_size > 0 ? args->config->part_size : (5 * 1024 * 1024);
            current_req_size = (long long)args->config->parts_for_each_upload_id * p_size;
        }

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
            // 只有当服务端返回明确的 5xx 且有 Request ID 时，才计入 5xx 失败
            // 否则归类为本地/网络类错误 (HTTP Code 改为 0)，避免干扰服务端压测指标
            if (current_http_code >= 500 && current_http_code < 600) {
                if (strcmp(current_req_id, "-") == 0 || strlen(current_req_id) == 0) {
                    current_http_code = 0;
                }
            }
        }

        if (detail_fp && batch_buffer) {
            batch_buffer[batch_count].timestamp_s = abs_timestamp;
            batch_buffer[batch_count].op_type = current_case;
            snprintf(batch_buffer[batch_count].key, sizeof(batch_buffer[batch_count].key), "%s", key);
            batch_buffer[batch_count].latency_ms = latency_ms;
            batch_buffer[batch_count].status_code = status;
            batch_buffer[batch_count].http_code = current_http_code;
            batch_buffer[batch_count].bytes = current_req_size;
            snprintf(batch_buffer[batch_count].request_id, sizeof(batch_buffer[batch_count].request_id), "%s", current_req_id);
            batch_count++;

            if (batch_count >= BATCH_SIZE) {
                for (int i = 0; i < batch_count; i++) {
                    fprintf(detail_fp, "%.3f,%d,%s,%s,%.2f,%d,%d,%lld,%s\n",
                            batch_buffer[i].timestamp_s, batch_buffer[i].op_type, args->effective_bucket, batch_buffer[i].key,
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
                    if (detail_fp) fprintf(detail_fp, "Timestamp(s),OpType,Bucket,Key,Latency(ms),SDKStatus,HTTPCode,Bytes,RequestID\n");
                }
            }
        }

        if (status == OBS_STATUS_OK) {
            args->stats.success_count++;
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
                fprintf(detail_fp, "%.3f,%d,%s,%s,%.2f,%d,%d,%lld,%s\n",
                        batch_buffer[i].timestamp_s, batch_buffer[i].op_type, args->effective_bucket, batch_buffer[i].key,
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

