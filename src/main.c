#include "bench.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h> 
#include <ctype.h>

const char* log_level_to_string(LogLevel level) {
    switch(level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_OFF:   return "OFF";
        default:        return "UNKNOWN";
    }
}

// 报告输出
void save_benchmark_report(Config *cfg, long long total, 
                           long long success, long long fail, 
                           long long f403, long long f404, long long f409, long long f4other,
                           long long f5xx, long long fother, long long fvalidate,
                           double tps, double throughput) {
    struct stat st = {0};
    if (stat("logs", &st) == -1) {
        if (mkdir("logs", 0755) != 0) return;
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", t);
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "logs/obscbench_%s.log", timestamp);

    FILE *fp = fopen(filepath, "w");
    if (!fp) return;

    fprintf(fp, "===========================================\n");
    fprintf(fp, "      OBS C SDK Benchmark Execution Report \n");
    fprintf(fp, "===========================================\n");
    fprintf(fp, "Execution Time:      %04d-%02d-%02d %02d:%02d:%02d\n",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    
    fprintf(fp, "---------------- Configuration ----------------\n");
    fprintf(fp, "[Environment]\n");
    fprintf(fp, "  Endpoint:          %s\n", cfg->endpoint);
    if(cfg->threads > 0) fprintf(fp, "  Bucket(Prefix):    %s\n", cfg->bucket_name_prefix);
    
    fprintf(fp, "[Network]\n");
    fprintf(fp, "  Protocol:          %s\n", cfg->protocol);
    fprintf(fp, "  KeepAlive:         %s\n", cfg->keep_alive ? "true" : "false");
    
    fprintf(fp, "[TestPlan - General]\n");
    fprintf(fp, "  Threads:           %d\n", cfg->threads);
    fprintf(fp, "  RunSeconds:        %d %s\n", cfg->run_seconds, cfg->run_seconds > 0 ? "(Time Limited)" : "(No Limit)");
    fprintf(fp, "  LogLevel:          %s\n", log_level_to_string(cfg->log_level));
    fprintf(fp, "  Validation:        %s\n", cfg->enable_data_validation ? "Enabled" : "Disabled");
    
    fprintf(fp, "[TestPlan - Mode]\n");
    if (cfg->use_mix_mode) {
        fprintf(fp, "  Mode:              Mixed Operation (900)\n");
        fprintf(fp, "  MixLoopCount:      %lld\n", cfg->mix_loop_count);
        fprintf(fp, "  MixOperation:      ");
        for(int i=0; i<cfg->mix_op_count; i++) {
            fprintf(fp, "%d%s", cfg->mix_ops[i], (i < cfg->mix_op_count - 1) ? "," : "");
        }
        fprintf(fp, "\n");
    } else {
        fprintf(fp, "  Mode:              Standard Case\n");
        fprintf(fp, "  TestCase:          %d\n", cfg->test_case);
        fprintf(fp, "  RequestsPerThread: %d\n", cfg->requests_per_thread);
    }

    fprintf(fp, "[TestPlan - Object]\n");
    if (cfg->is_dynamic_size) {
        fprintf(fp, "  ObjectSize:        %lld ~ %lld bytes (Dynamic)\n", cfg->object_size_min, cfg->object_size_max);
    } else {
        fprintf(fp, "  ObjectSize:        %lld bytes (Fixed)\n", cfg->object_size_max);
    }
    
    if (cfg->range_count > 0) {
        fprintf(fp, "  RangeOptions:      %d defined\n", cfg->range_count);
    }

    fprintf(fp, "---------------- Statistics -------------------\n");
    fprintf(fp, "Total Requests:      %lld\n", total);
    fprintf(fp, "Success:             %lld\n", success);
    fprintf(fp, "Failed:              %lld\n", fail);
    fprintf(fp, "  |- 403 (Forbidden):  %lld\n", f403);
    fprintf(fp, "  |- 404 (NotFound):   %lld\n", f404);
    fprintf(fp, "  |- 409 (Conflict):   %lld\n", f409);
    fprintf(fp, "  |- 4xx (Other):      %lld\n", f4other);
    fprintf(fp, "  |- 5xx (Server):     %lld\n", f5xx);
    fprintf(fp, "  |- Other (Net/SDK):  %lld\n", fother);
    fprintf(fp, "  |- Other (DataConsistencyError):  %lld\n", fvalidate);
    
    fprintf(fp, "\nPerformance:\n");
    fprintf(fp, "  TPS:                 %.2f\n", tps);
    fprintf(fp, "  Throughput:          %.2f MB/s\n", throughput);
    fprintf(fp, "===========================================\n");

    fclose(fp);
    LOG_INFO("Execution report saved to: %s", filepath);
}

int file_exists(const char *filepath) {
    struct stat buffer;
    return (stat(filepath, &buffer) == 0);
}

int create_deterministic_file(const char *filepath, long long size) {
    if (file_exists(filepath)) {
        struct stat st;
        stat(filepath, &st);
        if (st.st_size == size) {
            LOG_INFO("File exists and matches size, reusing: %s", filepath);
            return 0;
        }
    }

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        LOG_ERROR("Failed to create file: %s", filepath);
        return -1;
    }

    long long buf_size = 1024 * 1024;
    char *buf = malloc(buf_size);
    if (!buf) { fclose(fp); return -1; }
    
    fill_pattern_buffer(buf, buf_size, 0);

    long long written = 0;
    while (written < size) {
        long long to_write = (size - written > buf_size) ? buf_size : (size - written);
        fwrite(buf, 1, to_write, fp);
        written += to_write;
    }
    
    free(buf);
    fclose(fp);
    LOG_INFO("Created deterministic file: %s (%lld bytes)", filepath, size);
    return 0;
}

void str_tolower(char *dst, const char *src) {
    while(*src) {
        *dst = tolower((unsigned char)*src);
        dst++; src++;
    }
    *dst = '\0';
}

// [新增] 监控线程参数结构体
typedef struct {
    WorkerArgs *t_args;
    int thread_count;
    int interval_sec;
    volatile int stop_flag;
} MonitorArgs;

// [新增] 无锁监控线程：每隔 N 秒读取一次所有 Worker 的本地统计数据并打印瞬时指标
void *monitor_routine(void *arg) {
    MonitorArgs *m_args = (MonitorArgs *)arg;
    
    long long last_total_reqs = 0;
    long long last_total_bytes = 0;
    
    struct timeval last_tv, curr_tv;
    gettimeofday(&last_tv, NULL);

    while (!m_args->stop_flag) {
        // 使用 100ms 步进休眠，确保能够极速响应主线程的退出信号
        for(int i = 0; i < m_args->interval_sec * 10 && !m_args->stop_flag; i++) {
            usleep(100000); 
        }
        if (m_args->stop_flag) break;

        long long current_success = 0;
        long long current_fail = 0;
        long long current_bytes = 0;

        // 无锁遍历，性能零损耗
        for (int i = 0; i < m_args->thread_count; i++) {
            current_success += m_args->t_args[i].stats.success_count;
            current_bytes   += m_args->t_args[i].stats.total_success_bytes;
            current_fail    += (m_args->t_args[i].stats.fail_403_count + 
                                m_args->t_args[i].stats.fail_404_count + 
                                m_args->t_args[i].stats.fail_409_count + 
                                m_args->t_args[i].stats.fail_4xx_other_count + 
                                m_args->t_args[i].stats.fail_5xx_count + 
                                m_args->t_args[i].stats.fail_other_count +
                                m_args->t_args[i].stats.fail_validation_count);
        }

        long long current_total = current_success + current_fail;
        gettimeofday(&curr_tv, NULL);
        double elapsed_s = (curr_tv.tv_sec - last_tv.tv_sec) + (curr_tv.tv_usec - last_tv.tv_usec) / 1000000.0;
        
        if (elapsed_s > 0) {
            long long req_delta = current_total - last_total_reqs;
            long long bytes_delta = current_bytes - last_total_bytes;
            
            double current_tps = req_delta / elapsed_s;
            double current_throughput = (bytes_delta / 1024.0 / 1024.0) / elapsed_s;
            double success_rate = current_total > 0 ? ((double)current_success / current_total) * 100.0 : 0.0;

            printf("[Monitor] Interval: %.1fs | Instant TPS: %8.2f | BW: %8.2f MB/s | Success Rate: %6.2f%% | Total Reqs: %lld\n", 
                   elapsed_s, current_tps, current_throughput, success_rate, current_total);
        }

        last_total_reqs = current_total;
        last_total_bytes = current_bytes;
        last_tv = curr_tv;
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *config_file = "config.dat";
    int cli_test_case = 0;
    if (argc > 1) {
        if (isdigit(argv[1][0])) cli_test_case = atoi(argv[1]);
        else config_file = argv[1];
    }

    Config cfg;
    memset(&cfg, 0, sizeof(Config));
    if (load_config(config_file, &cfg) != 0) return 1;

    if (cli_test_case > 0) {
        cfg.test_case = cli_test_case;
        printf("[CLI] Overriding TestCase to: %d\n", cli_test_case);
        if (cli_test_case == TEST_CASE_MIX && cfg.mix_op_count > 0) cfg.use_mix_mode = 1;
        else cfg.use_mix_mode = 0;
    }

    log_init(cfg.log_level);
    LOG_INFO("--- OBS C SDK Benchmark Tool ---");
    LOG_INFO("Mode: %s", SDK_MODE_DESC);

    obs_status status = obs_initialize(OBS_INIT_ALL);
    if (status != OBS_STATUS_OK) {
        LOG_ERROR("SDK Initialize failed: %d", status);
        return -1;
    }

    initialize_break_point_lock();

    int need_upload_file = 0;
    if (cfg.test_case == TEST_CASE_RESUMABLE) need_upload_file = 1;
    if (cfg.use_mix_mode) {
        for(int i=0; i<cfg.mix_op_count; i++) if(cfg.mix_ops[i] == TEST_CASE_RESUMABLE) need_upload_file = 1;
    }

    if (need_upload_file) {
        struct stat st = {0};
        if (stat("upload_checkpoint", &st) == -1) mkdir("upload_checkpoint", 0755);
        if (strlen(cfg.upload_file_path) > 0) {
            create_deterministic_file(cfg.upload_file_path, 10 * 1024 * 1024); 
        }
    }

    double current_ms = 0; 
    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts_start);
    current_ms = ts_start.tv_sec * 1000.0 + ts_start.tv_nsec / 1000000.0;

    double stop_ms;
    if (cfg.run_seconds > 0) {
        stop_ms = current_ms + (cfg.run_seconds * 1000.0);
    } else {
        stop_ms = 1e15; 
    }

    pthread_t *tids = (pthread_t *)malloc(cfg.threads * sizeof(pthread_t));
    WorkerArgs *t_args = (WorkerArgs *)calloc(cfg.threads, sizeof(WorkerArgs));

    struct timeval start_tv, end_tv;
    gettimeofday(&start_tv, NULL);

    int global_thread_idx = 0;
    for (int u = 0; u < cfg.loaded_user_count; u++) {
        UserCredential *curr_user = &cfg.user_list[u];
        char target_bucket[256]; 
        
        // 智能桶名生成
        if (strlen(cfg.bucket_name_fixed) > 0) {
            strcpy(target_bucket, cfg.bucket_name_fixed);
        } else {
            char ak_lower[128];
            if (strlen(curr_user->ak) > 0) {
                str_tolower(ak_lower, curr_user->ak);
            } else {
                ak_lower[0] = '\0';
            }

            if (strlen(ak_lower) > 0 && strlen(cfg.bucket_name_prefix) > 0) {
                snprintf(target_bucket, sizeof(target_bucket), "%s.%s", ak_lower, cfg.bucket_name_prefix);
            } else if (strlen(ak_lower) > 0) {
                snprintf(target_bucket, sizeof(target_bucket), "%s", ak_lower);
            } else if (strlen(cfg.bucket_name_prefix) > 0) {
                snprintf(target_bucket, sizeof(target_bucket), "%s", cfg.bucket_name_prefix);
            } else {
                strcpy(target_bucket, "default-bench-bucket");
            }
        }

        for (int t = 0; t < cfg.threads_per_user; t++) {
            if (global_thread_idx >= cfg.threads) break;
            WorkerArgs *args = &t_args[global_thread_idx];
            args->thread_id = global_thread_idx;
            args->config = &cfg;
            args->stop_timestamp_ms = stop_ms;
            strcpy(args->effective_ak, curr_user->ak);
            strcpy(args->effective_sk, curr_user->sk);
            strcpy(args->effective_bucket, target_bucket);
            strcpy(args->username, curr_user->username);

            pthread_create(&tids[global_thread_idx], NULL, worker_routine, args);
            global_thread_idx++;
        }
    }
    
    // [新增] 启动监控线程
    MonitorArgs m_args;
    m_args.t_args = t_args;
    m_args.thread_count = cfg.threads;
    m_args.interval_sec = 3; 
    m_args.stop_flag = 0;
    
    pthread_t monitor_tid;
    pthread_create(&monitor_tid, NULL, monitor_routine, &m_args);

    // 等待所有 Worker 完成
    for (int i = 0; i < cfg.threads; i++) {
        pthread_join(tids[i], NULL);
    }

    // [新增] 通知监控线程退出并等待回收
    m_args.stop_flag = 1;
    pthread_join(monitor_tid, NULL);

    gettimeofday(&end_tv, NULL);
    double actual_time_s = (end_tv.tv_sec - start_tv.tv_sec) + (end_tv.tv_usec - start_tv.tv_usec) / 1000000.0;

    long long total_success = 0;
    long long t_403=0, t_404=0, t_409=0, t_4xx=0, t_5xx=0, t_other=0, t_val=0;
    long long total_bytes = 0;

    for (int i = 0; i < cfg.threads; i++) {
        total_success += t_args[i].stats.success_count;
        total_bytes += t_args[i].stats.total_success_bytes;
        
        t_403 += t_args[i].stats.fail_403_count;
        t_404 += t_args[i].stats.fail_404_count;
        t_409 += t_args[i].stats.fail_409_count;
        t_4xx += t_args[i].stats.fail_4xx_other_count;
        t_5xx += t_args[i].stats.fail_5xx_count;
        t_other += t_args[i].stats.fail_other_count;
        t_val += t_args[i].stats.fail_validation_count; 
    }
    
    long long total_fail = t_403 + t_404 + t_409 + t_4xx + t_5xx + t_other + t_val;
    long long total_reqs = total_success + total_fail;
    
    double tps = (actual_time_s > 0) ? (total_reqs / actual_time_s) : 0.0;
    double throughput_mb = (total_bytes) / 1024.0 / 1024.0 / actual_time_s;

    printf("\n--- Test Result ---\n");
    printf("Actual Duration: %.2f s\n", actual_time_s);
    printf("Total Requests:  %lld\n", total_reqs);
    printf("Success:         %lld\n", total_success);
    
    printf("Failed:          %lld\n", total_fail);
    printf("  |- 403 (Forbidden):  %lld\n", t_403);
    printf("  |- 404 (NotFound):   %lld\n", t_404);
    printf("  |- 409 (Conflict):   %lld\n", t_409);
    printf("  |- 4xx (Other):      %lld\n", t_4xx);
    printf("  |- 5xx (Server):     %lld\n", t_5xx);
    printf("  |- Other (Net/SDK):  %lld\n", t_other);
    printf("  |- Other (DataConsistencyError):  %lld\n", t_val);
    
    printf("TPS:             %.2f\n", tps);
    printf("Throughput:      %.2f MB/s\n", throughput_mb);

    save_benchmark_report(&cfg, total_reqs, total_success, total_fail, 
                          t_403, t_404, t_409, t_4xx, t_5xx, t_other, t_val,
                          tps, throughput_mb);

    free(tids);
    free(t_args);
    for(int i=0; i<MAX_RANGE_OPTIONS; i++) {
        if (cfg.range_options[i]) free(cfg.range_options[i]);
    }
    if (cfg.user_list) free(cfg.user_list);
    deinitialize_break_point_lock();
    obs_deinitialize();
    return 0;
}