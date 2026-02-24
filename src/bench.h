#ifndef BENCH_H
#define BENCH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include "log.h" 

#ifdef MOCK_SDK_MODE
    #include "../include/mock_eSDKOBS.h"
    #define SDK_MODE_DESC "Mock SDK (Simulation Mode)"
#else
    #include "eSDKOBS.h" 
    #define SDK_MODE_DESC "Real Huawei OBS C SDK"
#endif

// TestCase 编号定义
#define TEST_CASE_PUT           201
#define TEST_CASE_GET           202
#define TEST_CASE_DELETE        204
#define TEST_CASE_MULTIPART     216
#define TEST_CASE_RESUMABLE     230
#define TEST_CASE_MIX           900

#define MAX_MIX_OPS 32 
#define MAX_RANGE_OPTIONS 64 

// [新增] 批量落盘大小
#define BATCH_SIZE 1000

typedef struct {
    char username[64];
    char ak[128];
    char sk[128];
} UserCredential;

// [新增] 请求流水记录结构体
typedef struct {
    double timestamp_s;
    int op_type;
    char key[256];
    double latency_ms;
    int status_code;
    long long bytes;
} ReqRecord;

typedef struct {
    char endpoint[256];
    char protocol[16];
    int keep_alive;

    // --- 并发与用户配置 ---
    int threads;
    int target_user_count;
    int threads_per_user;
    char bucket_name_prefix[64];
    char bucket_name_fixed[128];

    // --- 用户列表 ---
    UserCredential *user_list;
    int loaded_user_count;

    // --- 测试计划 ---
    int requests_per_thread; 
    int test_case; 
    
    // --- 对象大小配置 ---
    long long object_size;      
    long long object_size_min;  
    long long object_size_max;  
    int is_dynamic_size;        

    // --- Range 下载配置 ---
    char *range_options[MAX_RANGE_OPTIONS];
    int range_count;

    long long part_size;
    char key_prefix[64];
    int run_seconds;
    
    LogLevel log_level;
    int obj_name_pattern_hash; 

    // --- 断点续传 ---
    int enable_checkpoint;      
    char upload_file_path[256]; 

    // --- 混合操作 ---
    int mix_ops[MAX_MIX_OPS];  
    int mix_op_count;          
    long long mix_loop_count;  
    int use_mix_mode;          

    // --- 安全配置 ---
    int gm_mode_switch;
    long ssl_min_version;
    long ssl_max_version;
    int mutual_ssl_switch;
    char client_cert_path[256];
    char client_key_path[256];
    char client_key_password[256];

    // --- 数据校验与日志 ---
    int enable_data_validation;
    int enable_detail_log;      // [新增] 是否开启 detail.csv 流水日志
    char task_log_dir[256];     // [新增] 本次压测的独立日志目录路径

} Config;

typedef struct {
    long long success_count;    
    
    // [详细错误统计]
    long long fail_403_count;   
    long long fail_404_count;   
    long long fail_409_count;   
    long long fail_4xx_other_count; 
    long long fail_5xx_count;   
    long long fail_other_count; 
    long long fail_validation_count; 

    long long total_success_bytes; 

    double total_latency_ms;
    double max_latency_ms;
    double min_latency_ms;
} ThreadStats;

typedef struct {
    int thread_id;
    Config *config;
    ThreadStats stats;
    char *data_buffer; 
    double stop_timestamp_ms;

    char effective_ak[128];
    char effective_sk[128];
    char effective_bucket[128];
    char username[64];
    
    char *pattern_buffer;       
    long long pattern_size;     
    long long pattern_mask;     
} WorkerArgs;

// 函数声明
int load_config(const char *filename, Config *cfg);
void *worker_routine(void *arg);
void fill_pattern_buffer(char *buf, size_t size, int seed);

void save_benchmark_report(Config *cfg, long long total, 
                           long long success, long long fail, 
                           long long f403, long long f404, long long f409, long long f4other,
                           long long f5xx, long long fother, long long fvalidate,
                           double tps, double throughput);

obs_status run_put_benchmark(WorkerArgs *args, char *key, long long object_size);
obs_status run_get_benchmark(WorkerArgs *args, char *key, char *range_str);
obs_status run_delete_benchmark(WorkerArgs *args, char *key);
obs_status run_list_benchmark(WorkerArgs *args);
obs_status run_multipart_benchmark(WorkerArgs *args, char *key);
obs_status run_upload_file_benchmark(WorkerArgs *args, char *key);

#endif

