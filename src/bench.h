#ifndef BENCH_H
#define BENCH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include "log.h" 

#ifdef MOCK_SDK_MODE
    #include "../include/mock_eSDKOBS.h"
    #define SDK_MODE_DESC "Mock SDK (Simulation Mode)"
#else
    #include "eSDKOBS.h" 
    #define SDK_MODE_DESC "Real Huawei OBS C SDK"
#endif

// TestCase 编号定义
#define TEST_CASE_PUT           201  // 上传对象
#define TEST_CASE_GET           202  // 下载对象
#define TEST_CASE_DELETE        204  // 删除对象
#define TEST_CASE_MULTIPART     216  // 多段上传
#define TEST_CASE_RESUMABLE     230  // 断点续传上传
#define TEST_CASE_MIX           900  // 混合操作模式

#define MAX_MIX_OPS 32 

// 单个用户信息结构
typedef struct {
    char username[64];
    char ak[128];
    char sk[128];
} UserCredential;

typedef struct {
    char endpoint[256];
    char protocol[16];
    int keep_alive;

    // --- 并发与用户配置 ---
    int threads;                // 总线程数 (计算得出)
    int target_user_count;      // config: Users
    int threads_per_user;       // config: ThreadsPerUser
    char bucket_name_prefix[64];// config: BucketNamePrefix
    char bucket_name_fixed[128];// config: BucketNameFixed

    // --- 用户列表数据 ---
    UserCredential *user_list;
    int loaded_user_count;

    // --- 测试计划配置 ---
    int requests_per_thread; 
    int test_case; 
    long long object_size;
    long long part_size;
    char key_prefix[64];
    int run_seconds;
    
    LogLevel log_level;
    int obj_name_pattern_hash; 

    // --- 断点续传配置 ---
    int enable_checkpoint;      
    char upload_file_path[256]; 

    // --- 混合操作配置 ---
    int mix_ops[MAX_MIX_OPS];  
    int mix_op_count;          
    long long mix_loop_count;  
    int use_mix_mode;          

    // --- [新增] 安全与国密配置 ---
    // config: GmModeSwitch (0=false/CLOSE, 1=true/OPEN)
    int gm_mode_switch;
    
    // config: SslMinVersion / SslMaxVersion
    // 存储映射后的 CURL_SSLVERSION 常量值
    long ssl_min_version;
    long ssl_max_version;

    // config: MutualSslSwitch (0=false/CLOSE, 1=true/OPEN)
    int mutual_ssl_switch;
    
    // config: ClientCertPath
    char client_cert_path[256];
    // config: ClientKeyPath
    char client_key_path[256];
    // config: ClientKeyPassword
    char client_key_password[256];

} Config;

typedef struct {
    long long success_count;    
    long long fail_403_count;   
    long long fail_404_count;   
    long long fail_409_count;   
    long long fail_4xx_other_count; 
    long long fail_5xx_count;   
    long long fail_other_count; 
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

    // 线程专属的生效配置
    char effective_ak[128];
    char effective_sk[128];
    char effective_bucket[128];
    char username[64];
} WorkerArgs;

// 函数声明
int load_config(const char *filename, Config *cfg);
void *worker_routine(void *arg);

obs_status run_put_benchmark(WorkerArgs *args, char *key);
obs_status run_get_benchmark(WorkerArgs *args, char *key);
obs_status run_delete_benchmark(WorkerArgs *args, char *key);
obs_status run_list_benchmark(WorkerArgs *args);
obs_status run_multipart_benchmark(WorkerArgs *args, char *key);
obs_status run_upload_file_benchmark(WorkerArgs *args, char *key);

#endif

