#include "bench.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h> 
#include <ctype.h>
#include <signal.h>

volatile sig_atomic_t g_graceful_stop = 0;

void handle_sigint(int sig) {
    static volatile sig_atomic_t sigint_count = 0;
    sigint_count++;

    if (sigint_count == 1) {
        g_graceful_stop = 1;
        const char msg[] = "\n[WARN] Received SIGINT (1/2). Initiating graceful shutdown... Please wait.\n"
                           "       Press CTRL+C again to FORCE QUIT if it hangs.\n";
        if (write(STDOUT_FILENO, msg, sizeof(msg) - 1) < 0) { }
    } else {
        const char msg[] = "\n[FATAL] Received SIGINT (2/2). Force quitting immediately!\n";
        if (write(STDOUT_FILENO, msg, sizeof(msg) - 1) < 0) { }
        _exit(1); 
    }
}

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
                           long long f5xx, long long fother, long long fvalidate,
                           double tps, double throughput) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/brief.txt", cfg->task_log_dir);

    FILE *fp = fopen(filepath, "w");
    if (!fp) return;

    time_t now = time(NULL);
    struct tm t_res;
    struct tm *t = localtime_r(&now, &t_res);

    fprintf(fp, "===========================================\n");
    fprintf(fp, "      OBS C SDK Benchmark Execution Report \n");
    fprintf(fp, "===========================================\n");
    if (t) {
        fprintf(fp, "Execution Time:      %04d-%02d-%02d %02d:%02d:%02d\n",
                t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    }
    
    fprintf(fp, "---------------- Configuration ----------------\n");
    fprintf(fp, "[Environment]\n");
    fprintf(fp, "  Endpoint:          %s\n", cfg->endpoint);
    fprintf(fp, "  Bucket(Fixed):     %s\n", cfg->bucket_name_fixed[0] ? cfg->bucket_name_fixed : "N/A");
    fprintf(fp, "  Bucket(Prefix):    %s\n", cfg->bucket_name_prefix[0] ? cfg->bucket_name_prefix : "N/A");
    fprintf(fp, "  STS Auth Mode:     %s\n", cfg->is_temporary_token ? "true" : "false");
    
    fprintf(fp, "[Network]\n");
    fprintf(fp, "  Protocol:          %s\n", cfg->protocol);
    fprintf(fp, "  KeepAlive:         %s\n", cfg->keep_alive ? "true" : "false");
    fprintf(fp, "  ConnectTimeout:    %d sec\n", cfg->connect_timeout_sec);
    fprintf(fp, "  RequestTimeout:    %d sec\n", cfg->request_timeout_sec);
    
    fprintf(fp, "[TestPlan]\n");
    fprintf(fp, "  Total Threads:     %d (%d Users x %d Threads/User)\n", 
            cfg->threads, cfg->loaded_user_count, cfg->threads_per_user);
    fprintf(fp, "  RunSeconds:        %d %s\n", cfg->run_seconds, cfg->run_seconds > 0 ? "(Time Limited)" : "(No Limit)");
    if (cfg->use_mix_mode) {
        fprintf(fp, "  TestMode:          Mixed Operations (900)\n");
        fprintf(fp, "  MixLoopCount:      %lld\n", cfg->mix_loop_count);
    } else {
        fprintf(fp, "  TestMode:          Standard TestCase (%d)\n", cfg->test_case);
    }
    fprintf(fp, "  ReqsPerOp/Thread:  %d\n", cfg->requests_per_thread);

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
    fprintf(fp, "  |- Internal Validation Fail: %lld\n", fvalidate);
    
    fprintf(fp, "\nPerformance:\n");
    fprintf(fp, "  Final TPS:           %.2f\n", tps);
    fprintf(fp, "  Final Throughput:    %.2f MB/s\n", throughput);
    fprintf(fp, "===========================================\n");

    fclose(fp);
    LOG_INFO("Execution report saved to: %s", filepath);
}

void str_tolower(char *dst, const char *src) {
    while(*src) {
        *dst = tolower((unsigned char)*src);
        dst++; src++;
    }
    *dst = '\0';
}

typedef struct {
    WorkerArgs *t_args;
    int thread_count;
    int interval_sec;
    volatile int stop_flag;
    char task_log_dir[256]; 
} MonitorArgs;

void *monitor_routine(void *arg) {
    MonitorArgs *m_args = (MonitorArgs *)arg;
    char rt_filepath[512];
    snprintf(rt_filepath, sizeof(rt_filepath), "%s/realtime.txt", m_args->task_log_dir);
    FILE *rt_fp = fopen(rt_filepath, "w");
    if (rt_fp) {
        fprintf(rt_fp, "RunTime(s),Process(%%),Cumul_TPS,Cumul_BW(MB/s),Success_Rate(%%),Total_Reqs\n");
        fflush(rt_fp);
    }

    struct timeval start_tv, curr_tv;
    gettimeofday(&start_tv, NULL);

    while (!m_args->stop_flag && !g_graceful_stop) {
        for(int i = 0; i < m_args->interval_sec * 10 && !m_args->stop_flag && !g_graceful_stop; i++) {
            usleep(100000); 
        }
        if (m_args->stop_flag || g_graceful_stop) break;

        long long current_success = 0, current_fail = 0, current_bytes = 0;
        for (int i = 0; i < m_args->thread_count; i++) {
            current_success += m_args->t_args[i].stats.success_count;
            current_bytes   += m_args->t_args[i].stats.total_success_bytes;
            current_fail    += (m_args->t_args[i].stats.fail_403_count + m_args->t_args[i].stats.fail_404_count + 
                                m_args->t_args[i].stats.fail_409_count + m_args->t_args[i].stats.fail_4xx_other_count + 
                                m_args->t_args[i].stats.fail_5xx_count + m_args->t_args[i].stats.fail_other_count +
                                m_args->t_args[i].stats.fail_validation_count);
        }

        long long current_total = current_success + current_fail;
        gettimeofday(&curr_tv, NULL);
        double total_elapsed_s = (curr_tv.tv_sec - start_tv.tv_sec) + (curr_tv.tv_usec - start_tv.tv_usec) / 1000000.0;
        
        if (total_elapsed_s > 0) {
            double cumul_tps = current_total / total_elapsed_s;
            double cumul_throughput = (current_bytes / 1024.0 / 1024.0) / total_elapsed_s;
            double success_rate = current_total > 0 ? ((double)current_success / current_total) * 100.0 : 0.0;

            Config *cfg = m_args->t_args[0].config;
            double progress_pct = -1.0; 
            
            if (cfg->run_seconds > 0) {
                progress_pct = (total_elapsed_s / cfg->run_seconds) * 100.0;
            } else {
                long long reqs_per_op = cfg->requests_per_thread > 0 ? cfg->requests_per_thread : 1;
                long long expected_total_reqs = 0;
                
                if (cfg->use_mix_mode) {
                    expected_total_reqs = (long long)cfg->threads * cfg->mix_op_count * cfg->mix_loop_count * reqs_per_op;
                } else if (cfg->requests_per_thread > 0) {
                    expected_total_reqs = (long long)cfg->threads * reqs_per_op;
                }
                
                if (expected_total_reqs > 0) {
                    progress_pct = ((double)current_total / expected_total_reqs) * 100.0;
                }
            }

            if (progress_pct > 100.0) progress_pct = 100.0;

            if (progress_pct >= 0.0) {
                printf("[Monitor] RunTime: %8.1fs | Process: %6.2f%% | Cumul TPS: %8.2f | Cumul BW: %8.2f MB/s | Success Rate: %7.3f%% | Total Reqs: %lld\n", 
                       total_elapsed_s, progress_pct, cumul_tps, cumul_throughput, success_rate, current_total);
            } else {
                printf("[Monitor] RunTime: %8.1fs | Process:    N/A | Cumul TPS: %8.2f | Cumul BW: %8.2f MB/s | Success Rate: %7.3f%% | Total Reqs: %lld\n", 
                       total_elapsed_s, cumul_tps, cumul_throughput, success_rate, current_total);
            }
                   
            if (rt_fp) {
                fprintf(rt_fp, "%.1f,%.2f,%.2f,%.2f,%.3f,%lld\n", 
                        total_elapsed_s, progress_pct >= 0 ? progress_pct : 0.0, 
                        cumul_tps, cumul_throughput, success_rate, current_total);
                fflush(rt_fp);
            }
        }
    }
    
    if (rt_fp) fclose(rt_fp);
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_sigint);

    struct stat st = {0};
    if (stat("logs", &st) == -1) mkdir("logs", 0755);
    
    time_t now = time(NULL);
    struct tm t_res;
    struct tm *t = localtime_r(&now, &t_res);
    Config cfg;
    memset(&cfg, 0, sizeof(Config));
    
    if (t) {
        snprintf(cfg.task_log_dir, sizeof(cfg.task_log_dir), "logs/task_%04d%02d%02d_%02d%02d%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    } else {
        snprintf(cfg.task_log_dir, sizeof(cfg.task_log_dir), "logs/task_UNKNOWN");
    }
             
    if (stat(cfg.task_log_dir, &st) == -1) mkdir(cfg.task_log_dir, 0755);

    const char *config_file = "config.dat";
    int cli_test_case = 0;
    if (argc > 1) {
        if (isdigit(argv[1][0])) cli_test_case = atoi(argv[1]);
        else config_file = argv[1];
    }

    // 仅仅加载配置，不提前做耗时计算
    if (load_config(config_file, &cfg) != 0) return 1;

    if (cli_test_case > 0) {
        cfg.test_case = cli_test_case;
        if (cli_test_case == TEST_CASE_MIX && cfg.mix_op_count > 0) cfg.use_mix_mode = 1;
        else cfg.use_mix_mode = 0;
    }

    log_init(cfg.log_level);
    LOG_INFO("--- OBS C SDK Benchmark Tool ---");
    LOG_INFO("Task Output Dir: %s", cfg.task_log_dir);

    // ==========================================================
    // [前置阶段]: 获取 Token 与加载账户信息，保证不进入最终耗时统计
    // ==========================================================
    if (cfg.is_temporary_token) {
        LOG_INFO("IsTemporaryToken enabled. Fetching STS tokens...");
        if (strlen(cfg.bucket_name_fixed) == 0) {
            LOG_ERROR("FATAL: BucketNameFixed MUST be configured in TempToken mode to avoid random 403!");
            return 1;
        }

        if (system("python3 generate_temp_ak_sk.py") != 0) {
            LOG_ERROR("FATAL: Failed to generate temporary credentials.");
            return 1;
        }
        
        if (load_users_file("temptoken.dat", &cfg, 1) < 0) return 1;
    } else {
        if (load_users_file("users.dat", &cfg, 0) < 0) return 1;
    }

    if (cfg.threads_per_user <= 0) cfg.threads_per_user = 1;
    cfg.threads = cfg.loaded_user_count * cfg.threads_per_user;
        
    printf("[Config] Multi-User Mode: %d Users Loaded. %d Threads/User. Total Threads: %d\n", 
           cfg.loaded_user_count, cfg.threads_per_user, cfg.threads);

    if (cfg.enable_data_validation) printf("[Config] Data Validation: ENABLED\n");
    if (cfg.enable_detail_log) printf("[Config] Detail Request Log: ENABLED\n");

    obs_status status = obs_initialize(OBS_INIT_ALL);
    if (status != OBS_STATUS_OK) return -1;

    // ==========================================================
    // [精准计时开始]
    // ==========================================================
    double current_ms = 0; 
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts_now);
    current_ms = ts_now.tv_sec * 1000.0 + ts_now.tv_nsec / 1000000.0;
    double stop_ms = (cfg.run_seconds > 0) ? (current_ms + cfg.run_seconds * 1000.0) : 1e15; 

    pthread_t *tids = (pthread_t *)malloc(cfg.threads * sizeof(pthread_t));
    WorkerArgs *t_args = (WorkerArgs *)calloc(cfg.threads, sizeof(WorkerArgs));

    struct timeval main_start_tv, main_end_tv;
    gettimeofday(&main_start_tv, NULL);

    int global_thread_idx = 0;
    for (int u = 0; u < cfg.loaded_user_count; u++) {
        UserCredential *curr_user = &cfg.user_list[u];
        
        char target_bucket[256]; 
        memset(target_bucket, 0, sizeof(target_bucket));

        if (strlen(cfg.bucket_name_fixed) > 0) {
            strcpy(target_bucket, cfg.bucket_name_fixed);
        } else {
            char ak_lower[128] = {0};
            if (strlen(curr_user->ak) > 0) str_tolower(ak_lower, curr_user->ak);

            if (strlen(cfg.bucket_name_prefix) > 0) {
                if (strlen(ak_lower) > 0) snprintf(target_bucket, sizeof(target_bucket), "%s.%s", ak_lower, cfg.bucket_name_prefix);
                else snprintf(target_bucket, sizeof(target_bucket), "%s", cfg.bucket_name_prefix);
            } else if (strlen(ak_lower) > 0) {
                snprintf(target_bucket, sizeof(target_bucket), "%s", ak_lower);
            } else {
                strcpy(target_bucket, "default-bench-bucket");
            }
        }
        
        for (int t_idx = 0; t_idx < cfg.threads_per_user; t_idx++) {
            if (global_thread_idx >= cfg.threads) break;
            WorkerArgs *args = &t_args[global_thread_idx];
            args->thread_id = global_thread_idx;
            args->config = &cfg;
            args->stop_timestamp_ms = stop_ms;
            strcpy(args->effective_ak, curr_user->ak);
            strcpy(args->effective_sk, curr_user->sk);
            strcpy(args->effective_bucket, target_bucket);
            strcpy(args->username, curr_user->username);
            
            // [新增]: 将解析到的 STS Token 传递给线程
            if (cfg.is_temporary_token) {
                strcpy(args->effective_token, curr_user->security_token);
            } else {
                memset(args->effective_token, 0, sizeof(args->effective_token));
            }

            pthread_create(&tids[global_thread_idx], NULL, worker_routine, args);
            global_thread_idx++;
        }
    }
    
    MonitorArgs m_args;
    m_args.t_args = t_args;
    m_args.thread_count = cfg.threads;
    m_args.interval_sec = 3; 
    m_args.stop_flag = 0;
    strcpy(m_args.task_log_dir, cfg.task_log_dir); 
    
    pthread_t monitor_tid;
    pthread_create(&monitor_tid, NULL, monitor_routine, &m_args);

    for (int i = 0; i < cfg.threads; i++) pthread_join(tids[i], NULL);

    m_args.stop_flag = 1;
    pthread_join(monitor_tid, NULL);

    gettimeofday(&main_end_tv, NULL);
    double actual_time_s = (main_end_tv.tv_sec - main_start_tv.tv_sec) + (main_end_tv.tv_usec - main_start_tv.tv_usec) / 1000000.0;

    // 核心结果汇总逻辑
    long long total_success = 0, t_403=0, t_404=0, t_409=0, t_4xx=0, t_5xx=0, t_other=0, t_val=0, total_bytes=0;

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

    if (g_graceful_stop) {
        printf("\n[WARN] Benchmark interrupted by user (Graceful Stop).\n");
    }

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
    printf("  |- Internal Validation Fail: %lld\n", t_val);
    
    printf("TPS:             %.2f\n", tps);
    printf("Throughput:      %.2f MB/s\n", throughput_mb);

    save_benchmark_report(&cfg, total_reqs, total_success, total_fail, 
                          t_403, t_404, t_409, t_4xx, t_5xx, t_other, t_val,
                          tps, throughput_mb);

    free(tids); 
    free(t_args);

    if (cfg.user_list) {
        free(cfg.user_list);
        cfg.user_list = NULL;
    }
    for (int i = 0; i < cfg.range_count; i++) {
        if (cfg.range_options[i]) {
            free(cfg.range_options[i]);
            cfg.range_options[i] = NULL;
        }
    }

    obs_deinitialize();
    return 0;
}

