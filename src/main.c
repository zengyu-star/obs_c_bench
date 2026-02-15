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

void save_benchmark_report(Config *cfg, long long total, 
                           long long success, long long fail, 
                           long long f403, long long f404, long long f409, long long f4other,
                           long long f5xx, long long fother,
                           double tps, double throughput) {
    struct stat st = {0};
    if (stat("logs", &st) == -1) {
        if (mkdir("logs", 0755) != 0) {
            LOG_ERROR("Failed to create logs directory.");
            return;
        }
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", t);

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "logs/obscbench_%s.log", timestamp);

    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        LOG_ERROR("Failed to open log file: %s", filepath);
        return;
    }

    fprintf(fp, "===========================================\n");
    fprintf(fp, "      OBS C SDK Benchmark Execution Report \n");
    fprintf(fp, "===========================================\n");
    fprintf(fp, "Execution Time:      %04d-%02d-%02d %02d:%02d:%02d\n",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec);
    
    fprintf(fp, "---------------- Configuration ----------------\n");
    fprintf(fp, "[Environment]\n");
    fprintf(fp, "  Endpoint:          %s\n", cfg->endpoint);
    
    fprintf(fp, "[Multi-User]\n");
    fprintf(fp, "  Users Loaded:      %d\n", cfg->loaded_user_count);
    fprintf(fp, "  ThreadsPerUser:    %d\n", cfg->threads_per_user);
    fprintf(fp, "  Total Threads:     %d\n", cfg->threads);
    fprintf(fp, "  BucketStrategy:    %s\n", 
            strlen(cfg->bucket_name_fixed) > 0 ? cfg->bucket_name_fixed : "Dynamic (UserAK.Prefix)");

    fprintf(fp, "[Network]\n");
    fprintf(fp, "  Protocol:          %s\n", cfg->protocol);
    fprintf(fp, "  KeepAlive:         %s\n", cfg->keep_alive ? "true" : "false");
    
    fprintf(fp, "[TestPlan]\n");
    fprintf(fp, "  RunSeconds:        %d\n", cfg->run_seconds);
    fprintf(fp, "  TestCase:          %d\n", cfg->test_case);
    fprintf(fp, "  UseMixMode:        %s\n", cfg->use_mix_mode ? "Yes" : "No");

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

int create_dummy_file(const char *filepath, long long size) {
    if (file_exists(filepath)) {
        struct stat st;
        stat(filepath, &st);
        if (st.st_size == size) {
            LOG_INFO("File exists and size matches, reusing: %s", filepath);
            return 0;
        } else {
            LOG_WARN("File exists but size mismatch (%lld vs %lld), recreating...", (long long)st.st_size, size);
        }
    }

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        LOG_ERROR("Failed to create file: %s", filepath);
        return -1;
    }

    char buf[1024];
    memset(buf, 'A', sizeof(buf));
    fwrite(buf, 1, sizeof(buf), fp);

    if (size > 1024) {
        fseek(fp, size - 1, SEEK_SET);
        fwrite("", 1, 1, fp); 
    }
    
    fclose(fp);
    LOG_INFO("Created dummy file: %s (%lld bytes)", filepath, size);
    return 0;
}

// [辅助] 转小写
void str_tolower(char *dst, const char *src) {
    while(*src) {
        *dst = tolower((unsigned char)*src);
        dst++; src++;
    }
    *dst = '\0';
}

int main(int argc, char **argv) {
    const char *config_file = "config.dat";
    int cli_test_case = 0;

    if (argc > 1) {
        if (isdigit(argv[1][0])) {
            cli_test_case = atoi(argv[1]);
        } else {
            config_file = argv[1];
        }
    }

    Config cfg;
    memset(&cfg, 0, sizeof(Config));
    
    // 如果加载失败（包括用户数不足），直接退出
    if (load_config(config_file, &cfg) != 0) {
        return 1;
    }

    if (cli_test_case > 0) {
        cfg.test_case = cli_test_case;
        printf("[CLI] Overriding TestCase to: %d\n", cli_test_case);
        if (cli_test_case == TEST_CASE_MIX && cfg.mix_op_count > 0) {
            cfg.use_mix_mode = 1;
        } else {
            cfg.use_mix_mode = 0;
        }
    }

    log_init(cfg.log_level);
    LOG_INFO("--- OBS C SDK Benchmark Tool ---");
    LOG_INFO("Mode: %s", SDK_MODE_DESC);

    obs_status status = obs_initialize(OBS_INIT_ALL);
    if (status != OBS_STATUS_OK) {
        LOG_ERROR("SDK Initialize failed. Status: %d", status);
        return -1;
    }

    initialize_break_point_lock();

    // 检查 Checkpoint 目录
    int need_checkpoint_dir = 0;
    if (cfg.test_case == TEST_CASE_RESUMABLE) need_checkpoint_dir = 1;
    if (cfg.use_mix_mode) {
        for(int i=0; i<cfg.mix_op_count; i++) if(cfg.mix_ops[i] == TEST_CASE_RESUMABLE) need_checkpoint_dir = 1;
    }
    if (need_checkpoint_dir) {
        struct stat st = {0};
        if (stat("upload_checkpoint", &st) == -1) {
            mkdir("upload_checkpoint", 0755);
        }
    }

    double current_ms = 0; 
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    current_ms = ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;

    double stop_ms = 0;
    if (cfg.run_seconds > 0) {
        stop_ms = current_ms + (cfg.run_seconds * 1000.0);
    } else {
        stop_ms = 1e15; 
    }

    pthread_t *tids = (pthread_t *)malloc(cfg.threads * sizeof(pthread_t));
    WorkerArgs *t_args = (WorkerArgs *)malloc(cfg.threads * sizeof(WorkerArgs));

    struct timeval start_tv, end_tv;
    gettimeofday(&start_tv, NULL);

    int global_thread_idx = 0;

    // =========================================================
    // [核心] 双重循环分配任务 (严格多用户模式)
    // =========================================================
    // 这里不再检查 cfg.loaded_user_count > 0，因为 load_config 保证了这一点
    for (int u = 0; u < cfg.loaded_user_count; u++) {
        UserCredential *curr_user = &cfg.user_list[u];
        
        char target_bucket[256]; 

        // 桶名生成
        if (strlen(cfg.bucket_name_fixed) > 0) {
            strcpy(target_bucket, cfg.bucket_name_fixed);
        } else {
            char ak_lower[128];
            str_tolower(ak_lower, curr_user->ak);
            if (strlen(cfg.bucket_name_prefix) > 0)
                snprintf(target_bucket, sizeof(target_bucket), "%s.%s", ak_lower, cfg.bucket_name_prefix);
            else 
                snprintf(target_bucket, sizeof(target_bucket), "%s", ak_lower);
        }

        for (int t = 0; t < cfg.threads_per_user; t++) {
            if (global_thread_idx >= cfg.threads) break;

            WorkerArgs *args = &t_args[global_thread_idx];
            args->thread_id = global_thread_idx;
            args->config = &cfg;
            args->data_buffer = NULL;
            args->stop_timestamp_ms = stop_ms;

            // [关键] 注入线程专属凭证
            strcpy(args->effective_ak, curr_user->ak);
            strcpy(args->effective_sk, curr_user->sk);
            strcpy(args->effective_bucket, target_bucket);
            strcpy(args->username, curr_user->username);

            int ret = pthread_create(&tids[global_thread_idx], NULL, worker_routine, args);
            if (ret != 0) LOG_ERROR("Failed to create thread %d", global_thread_idx);
            
            global_thread_idx++;
        }
    }
    
    // 等待所有线程完成
    for (int i = 0; i < cfg.threads; i++) {
        pthread_join(tids[i], NULL);
    }

    gettimeofday(&end_tv, NULL);
    double total_time_s = (end_tv.tv_sec - start_tv.tv_sec) + 
                          (end_tv.tv_usec - start_tv.tv_usec) / 1000000.0;

    // 统计汇总
    long long total_success = 0;
    long long total_fail = 0;
    long long total_403 = 0, total_404 = 0, total_409 = 0;
    long long total_4other = 0, total_5xx = 0, total_other = 0;

    for (int i = 0; i < cfg.threads; i++) {
        total_success += t_args[i].stats.success_count;
        total_403 += t_args[i].stats.fail_403_count;
        total_404 += t_args[i].stats.fail_404_count;
        total_409 += t_args[i].stats.fail_409_count;
        total_4other += t_args[i].stats.fail_4xx_other_count;
        total_5xx += t_args[i].stats.fail_5xx_count;
        total_other += t_args[i].stats.fail_other_count;
    }
    
    total_fail = total_403 + total_404 + total_409 + total_4other + total_5xx + total_other;
    long long total_reqs = total_success + total_fail;
    
    double tps = (total_time_s > 0) ? (total_reqs / total_time_s) : 0.0;
    double throughput_mb = 0.0;
    
    long long calc_size = cfg.object_size;
    if (cfg.test_case == TEST_CASE_RESUMABLE) {
        struct stat st;
        if (stat(cfg.upload_file_path, &st) == 0) calc_size = st.st_size;
    }
    throughput_mb = (total_success * calc_size) / 1024.0 / 1024.0 / total_time_s;

    printf("\n--- Test Result ---\n");
    printf("Total Requests: %lld\n", total_reqs);
    printf("Success:        %lld\n", total_success);
    printf("Failed:         %lld\n", total_fail);
    printf("TPS:            %.2f\n", tps);
    printf("Throughput:     %.2f MB/s\n", throughput_mb);

    save_benchmark_report(&cfg, total_reqs, total_success, total_fail, 
                          total_403, total_404, total_409, total_4other, total_5xx, total_other,
                          tps, throughput_mb);

    free(tids);
    free(t_args);
    if (cfg.user_list) free(cfg.user_list);

    deinitialize_break_point_lock();
    obs_deinitialize();

    return 0;
}

