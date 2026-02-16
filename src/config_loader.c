#include "bench.h"
#include <strings.h>
#include <ctype.h>

#ifndef CURL_SSLVERSION_TLSv1_0
#define CURL_SSLVERSION_TLSv1_0 4
#endif
#ifndef CURL_SSLVERSION_TLSv1_1
#define CURL_SSLVERSION_TLSv1_1 5
#endif
#ifndef CURL_SSLVERSION_TLSv1_2
#define CURL_SSLVERSION_TLSv1_2 6
#endif
#define OBS_SSLVERSION_TLSv1_3 ((1 << 16) | 3)

// [修改] 辅助函数：去除字符串首尾的空白字符
static char* trim_both(char *s) {
    if (!s) return NULL;
    
    // 1. 去除尾部空白
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    
    // 2. 去除首部空白
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    
    return start; // 返回新的起始指针
}

static int parse_mix_ops(const char *val, int *ops, int max_ops) {
    int count = 0;
    char *temp = strdup(val);
    if (!temp) return 0;
    char *token = strtok(temp, ",");
    while (token != NULL && count < max_ops) {
        // [修改] 增加 token trim
        char *clean_token = trim_both(token);
        if (strlen(clean_token) == 0) {
             token = strtok(NULL, ",");
             continue;
        }
        int op = atoi(clean_token);
        if (op == TEST_CASE_MIX) {
            token = strtok(NULL, ",");
            continue;
        }
        if (op > 0) ops[count++] = op;
        token = strtok(NULL, ",");
    }
    free(temp);
    return count;
}

static int load_users_file(const char *filename, Config *cfg) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("[Config Error] Cannot open users file: %s.\n", filename);
        return -1;
    }
    if (cfg->target_user_count <= 0) cfg->target_user_count = 1;

    cfg->user_list = (UserCredential *)malloc(sizeof(UserCredential) * cfg->target_user_count);
    memset(cfg->user_list, 0, sizeof(UserCredential) * cfg->target_user_count);
    
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), fp) && count < cfg->target_user_count) {
        char *clean_line = trim_both(line);
        if (clean_line[0] == '#' || strlen(clean_line) == 0) continue;
        
        char *token = strtok(clean_line, ",");
        if(token) strcpy(cfg->user_list[count].username, trim_both(token)); 
        else continue;

        token = strtok(NULL, ",");
        if(token) strcpy(cfg->user_list[count].ak, trim_both(token)); 
        else continue;

        token = strtok(NULL, ",");
        if(token) strcpy(cfg->user_list[count].sk, trim_both(token)); 
        else continue;

        count++;
    }
    fclose(fp);
    cfg->loaded_user_count = count;
    return count;
}

static long parse_ssl_version_str(const char *val, int is_min) {
    if (val == NULL || strlen(val) == 0) {
        if (is_min) return CURL_SSLVERSION_TLSv1_0;
        else return OBS_SSLVERSION_TLSv1_3;
    }
    if (strcmp(val, "1.0") == 0) return CURL_SSLVERSION_TLSv1_0;
    if (strcmp(val, "1.1") == 0) return CURL_SSLVERSION_TLSv1_1;
    if (strcmp(val, "1.2") == 0) return CURL_SSLVERSION_TLSv1_2;
    if (strcmp(val, "1.3") == 0) return OBS_SSLVERSION_TLSv1_3;
    return -1;
}

int load_config(const char *filename, Config *cfg) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("Error: Cannot open config file: %s\n", filename);
        return -1;
    }
    
    // 默认值设置
    strcpy(cfg->protocol, "https");
    cfg->keep_alive = 1;
    cfg->part_size = 5 * 1024 * 1024;
    cfg->log_level = LOG_INFO; 
    cfg->obj_name_pattern_hash = 0;
    cfg->enable_checkpoint = 1; 
    cfg->upload_file_path[0] = '\0'; 
    cfg->requests_per_thread = 1; 
    cfg->mix_op_count = 0;
    cfg->mix_loop_count = 0; 
    cfg->use_mix_mode = 0;
    cfg->run_seconds = 0;
    cfg->target_user_count = 0;
    cfg->threads_per_user = 1;
    cfg->bucket_name_fixed[0] = '\0';
    cfg->bucket_name_prefix[0] = '\0';
    cfg->gm_mode_switch = 0;
    cfg->mutual_ssl_switch = 0;
    cfg->client_cert_path[0] = '\0';
    cfg->client_key_path[0] = '\0';
    cfg->client_key_password[0] = '\0';
    cfg->enable_data_validation = 0;
    
    cfg->object_size_min = cfg->object_size_max = 1024;
    cfg->is_dynamic_size = 0;
    
    cfg->range_count = 0;
    for(int i=0; i<MAX_RANGE_OPTIONS; i++) cfg->range_options[i] = NULL;

    char ssl_min_str[16] = {0};
    char ssl_max_str[16] = {0};
    char line[512];

    while (fgets(line, sizeof(line), fp)) {
        // [修改] 先清洗整行
        char *clean_line = trim_both(line);
        if (clean_line[0] == '#' || clean_line[0] == '[' || strlen(clean_line) == 0) continue;

        char *eq = strchr(clean_line, '=');
        if (!eq) continue;
        *eq = '\0';
        
        // [关键修改] 同时清洗 Key 和 Value，消除空格影响
        char *key = trim_both(clean_line);
        char *val = trim_both(eq + 1);

        if (strcmp(key, "Endpoint") == 0) strcpy(cfg->endpoint, val);
        else if (strcmp(key, "Protocol") == 0) strcpy(cfg->protocol, val);
        else if (strcmp(key, "KeepAlive") == 0) cfg->keep_alive = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "LogLevel") == 0) cfg->log_level = log_level_from_string(val);
        
        // [修正点] 现在 val 已经被 trim 过了，" true" 会变成 "true"，strcasecmp 能正确识别
        else if (strcmp(key, "ObjNamePatternHash") == 0) cfg->obj_name_pattern_hash = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        
        else if (strcmp(key, "EnableCheckpoint") == 0) cfg->enable_checkpoint = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "UploadFilePath") == 0) strcpy(cfg->upload_file_path, val);
        else if (strcmp(key, "BucketNamePrefix") == 0) strcpy(cfg->bucket_name_prefix, val);
        else if (strcmp(key, "BucketNameFixed") == 0) strcpy(cfg->bucket_name_fixed, val);
        else if (strcmp(key, "Users") == 0) cfg->target_user_count = atoi(val);
        else if (strcmp(key, "ThreadsPerUser") == 0) cfg->threads_per_user = atoi(val);
        else if (strcmp(key, "RequestsPerThread") == 0) cfg->requests_per_thread = atoi(val);
        else if (strcmp(key, "TestCase") == 0) cfg->test_case = atoi(val);
        
        else if (strcmp(key, "ObjectSize") == 0) {
            char *tilde = strchr(val, '~');
            if (tilde) {
                *tilde = '\0';
                cfg->object_size_min = atoll(val);
                cfg->object_size_max = atoll(tilde + 1);
                if (cfg->object_size_min > cfg->object_size_max) {
                    printf("[Config Error] Invalid ObjectSize range: min (%lld) > max (%lld).\n", 
                           cfg->object_size_min, cfg->object_size_max);
                    fclose(fp); return -1;
                }
                cfg->is_dynamic_size = 1;
                cfg->object_size = cfg->object_size_max; 
            } else {
                cfg->object_size_min = cfg->object_size_max = atoll(val);
                cfg->is_dynamic_size = 0;
                cfg->object_size = cfg->object_size_max;
            }
        }
        
        else if (strcmp(key, "Range") == 0) {
            char *temp = strdup(val);
            if (temp) {
                char *token = strtok(temp, ";");
                int idx = 0;
                while (token != NULL && idx < MAX_RANGE_OPTIONS) {
                    char *clean_token = trim_both(token);
                    if (strlen(clean_token) > 0) {
                        cfg->range_options[idx] = strdup(clean_token);
                        idx++;
                    }
                    token = strtok(NULL, ";");
                }
                cfg->range_count = idx;
                free(temp);
            }
        }

        else if (strcmp(key, "PartSize") == 0) cfg->part_size = atoll(val);
        else if (strcmp(key, "KeyPrefix") == 0) strcpy(cfg->key_prefix, val);
        else if (strcmp(key, "MixOperation") == 0) cfg->mix_op_count = parse_mix_ops(val, cfg->mix_ops, MAX_MIX_OPS);
        else if (strcmp(key, "MixLoopCount") == 0) cfg->mix_loop_count = atoll(val);
        else if (strcmp(key, "RunSeconds") == 0) cfg->run_seconds = atoi(val);
        else if (strcmp(key, "GmModeSwitch") == 0) cfg->gm_mode_switch = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "SslMinVersion") == 0) strcpy(ssl_min_str, val);
        else if (strcmp(key, "SslMaxVersion") == 0) strcpy(ssl_max_str, val);
        else if (strcmp(key, "MutualSslSwitch") == 0) cfg->mutual_ssl_switch = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "ClientCertPath") == 0) strcpy(cfg->client_cert_path, val);
        else if (strcmp(key, "ClientKeyPath") == 0) strcpy(cfg->client_key_path, val);
        else if (strcmp(key, "ClientKeyPassword") == 0) strcpy(cfg->client_key_password, val);
        else if (strcmp(key, "EnableDataValidation") == 0) cfg->enable_data_validation = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
    }
    
    if (cfg->part_size <= 0) cfg->part_size = 5 * 1024 * 1024; 
    if (cfg->target_user_count <= 0) {
        printf("[Config Error] 'Users' must be greater than 0.\n");
        fclose(fp); return -1;
    }

    cfg->ssl_min_version = parse_ssl_version_str(ssl_min_str, 1);
    cfg->ssl_max_version = parse_ssl_version_str(ssl_max_str, 0);

    if (cfg->gm_mode_switch) {
        if (cfg->ssl_min_version == -1 || cfg->ssl_max_version == -1) {
            printf("[Config Error] Invalid SslMin/MaxVersion.\n");
            fclose(fp); return -1;
        }
        if (cfg->ssl_min_version > cfg->ssl_max_version) {
            printf("[Config Error] SslMinVersion > SslMaxVersion.\n");
            fclose(fp); return -1;
        }
    }

    if (cfg->mutual_ssl_switch) {
        if (strlen(cfg->client_cert_path) == 0 || strlen(cfg->client_key_path) == 0) {
            printf("[Config Error] ClientCertPath/KeyPath required for MutualSsl.\n");
            fclose(fp); return -1;
        }
    }

    if (load_users_file("users.dat", cfg) < 0) {
        fclose(fp); return -1;
    }
    
    if (cfg->threads_per_user <= 0) cfg->threads_per_user = 1;
    cfg->threads = cfg->loaded_user_count * cfg->threads_per_user;
        
    printf("[Config] Multi-User Mode: %d Users Loaded. %d Threads/User. Total Threads: %d\n", 
           cfg->loaded_user_count, cfg->threads_per_user, cfg->threads);

    if (cfg->test_case == TEST_CASE_MIX) {
        if (cfg->mix_op_count > 0) cfg->use_mix_mode = 1;
    } else {
        cfg->use_mix_mode = 0;
    }
    
    int need_check_file = 0;
    if (cfg->test_case == TEST_CASE_RESUMABLE) need_check_file = 1;
    if (cfg->use_mix_mode) {
        for(int i=0; i<cfg->mix_op_count; i++) if(cfg->mix_ops[i] == TEST_CASE_RESUMABLE) need_check_file = 1;
    }

    if (need_check_file && strlen(cfg->upload_file_path) == 0) {
        printf("[ERROR] UploadFilePath cannot be empty for Resumable test.\n");
        fclose(fp); return -1;
    }

    if (cfg->enable_data_validation) {
        printf("[Config] Data Validation: ENABLED (Scheme 3: In-Memory/File Isomorphic Verification)\n");
    }
    
    if (cfg->is_dynamic_size) {
        printf("[Config] Dynamic Object Size: %lld ~ %lld bytes\n", cfg->object_size_min, cfg->object_size_max);
    }

    fclose(fp);
    return 0;
}

