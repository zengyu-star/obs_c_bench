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

static char* trim_both(char *s) {
    if (!s) return NULL;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    return start; 
}

static int parse_mix_ops(const char *val, int *ops, int max_ops) {
    int count = 0;
    char *temp = strdup(val);
    if (!temp) return 0;
    char *token = strtok(temp, ",");
    while (token != NULL && count < max_ops) {
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

// [修改]: 支持是否为临时模式解析
int load_users_file(const char *filename, Config *cfg, int is_temp_mode) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("[Config Error] Cannot open users file: %s.\n", filename);
        return -1;
    }
    if (cfg->target_user_count <= 0) cfg->target_user_count = 1;

    cfg->user_list = (UserCredential *)malloc(sizeof(UserCredential) * cfg->target_user_count);
    memset(cfg->user_list, 0, sizeof(UserCredential) * cfg->target_user_count);
    
    char line[4096]; // 加大 buffer，应对超长 token
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

        if (is_temp_mode) {
            token = strtok(NULL, ",");
            if(token) strcpy(cfg->user_list[count].security_token, trim_both(token)); 
        } else {
            memset(cfg->user_list[count].security_token, 0, sizeof(cfg->user_list[count].security_token));
        }

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
    
    cfg->connect_timeout_sec = 10; 
    cfg->request_timeout_sec = 30; 
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
    cfg->is_temporary_token = 0;  // 默认关闭临时凭证
    cfg->gm_mode_switch = 0;
    cfg->mutual_ssl_switch = 0;
    cfg->client_cert_path[0] = '\0';
    cfg->client_key_path[0] = '\0';
    cfg->client_key_password[0] = '\0';
    cfg->enable_data_validation = 0;
    cfg->enable_detail_log = 0;
    
    cfg->object_size_min = cfg->object_size_max = 1024;
    cfg->is_dynamic_size = 0;
    
    cfg->range_count = 0;
    for(int i=0; i<MAX_RANGE_OPTIONS; i++) cfg->range_options[i] = NULL;

    char ssl_min_str[16] = {0};
    char ssl_max_str[16] = {0};
    char line[512];

    while (fgets(line, sizeof(line), fp)) {
        char *clean_line = trim_both(line);
        if (clean_line[0] == '#' || clean_line[0] == '[' || strlen(clean_line) == 0) continue;

        char *eq = strchr(clean_line, '=');
        if (!eq) continue;
        *eq = '\0';
        
        char *key = trim_both(clean_line);
        char *val = trim_both(eq + 1);

        if (strcmp(key, "Endpoint") == 0) strcpy(cfg->endpoint, val);
        else if (strcmp(key, "Protocol") == 0) strcpy(cfg->protocol, val);
        else if (strcmp(key, "KeepAlive") == 0) cfg->keep_alive = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "ConnectTimeoutSec") == 0) cfg->connect_timeout_sec = atoi(val);
        else if (strcmp(key, "RequestTimeoutSec") == 0) cfg->request_timeout_sec = atoi(val);
        else if (strcmp(key, "LogLevel") == 0) cfg->log_level = log_level_from_string(val);
        else if (strcmp(key, "ObjNamePatternHash") == 0) cfg->obj_name_pattern_hash = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "EnableCheckpoint") == 0) cfg->enable_checkpoint = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "UploadFilePath") == 0) strcpy(cfg->upload_file_path, val);
        else if (strcmp(key, "BucketNamePrefix") == 0) strcpy(cfg->bucket_name_prefix, val);
        else if (strcmp(key, "BucketNameFixed") == 0) strcpy(cfg->bucket_name_fixed, val);
        else if (strcmp(key, "IsTemporaryToken") == 0) cfg->is_temporary_token = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
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
                    printf("[Config Error] Invalid ObjectSize range.\n");
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
        else if (strcmp(key, "EnableDetailLog") == 0) cfg->enable_detail_log = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
    }
    
    if (cfg->part_size <= 0) cfg->part_size = 5 * 1024 * 1024; 
    if (cfg->target_user_count <= 0) {
        printf("[Config Error] 'Users' must be greater than 0.\n");
        fclose(fp); return -1;
    }

    cfg->ssl_min_version = parse_ssl_version_str(ssl_min_str, 1);
    cfg->ssl_max_version = parse_ssl_version_str(ssl_max_str, 0);

    if (cfg->test_case == TEST_CASE_MIX) {
        if (cfg->mix_op_count > 0) cfg->use_mix_mode = 1;
    } else {
        cfg->use_mix_mode = 0;
    }

    // [修改]: 去除此处读取文件和计算 threads 逻辑，将其移至 main.c 中进行时序控制
    fclose(fp);
    return 0;
}

