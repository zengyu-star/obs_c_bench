#include "bench.h"
#include <strings.h>
#include <ctype.h>

// 为了确保在任何环境下都能编译，定义 CURL SSL 版本的常量
// 对应 SDK 要求的宏值
#ifndef CURL_SSLVERSION_TLSv1_0
#define CURL_SSLVERSION_TLSv1_0 4
#endif
#ifndef CURL_SSLVERSION_TLSv1_1
#define CURL_SSLVERSION_TLSv1_1 5
#endif
#ifndef CURL_SSLVERSION_TLSv1_2
#define CURL_SSLVERSION_TLSv1_2 6
#endif
// TLS 1.3 特殊值
#define OBS_SSLVERSION_TLSv1_3 ((1 << 16) | 3)

static void trim(char *s) {
    char *p = s;
    int l = strlen(p);
    while(l > 0 && (p[l-1] == '\r' || p[l-1] == '\n' || p[l-1] == ' ')) p[--l] = 0;
}

static int parse_mix_ops(const char *val, int *ops, int max_ops) {
    int count = 0;
    char *temp = strdup(val);
    if (!temp) return 0;

    char *token = strtok(temp, ",");
    while (token != NULL && count < max_ops) {
        while(isspace((unsigned char)*token)) token++;
        int op = atoi(token);
        
        if (op == TEST_CASE_MIX) {
            printf("[Config Warning] MixOperation cannot contain %d. Ignored.\n", TEST_CASE_MIX);
            token = strtok(NULL, ",");
            continue;
        }

        if (op > 0) {
            ops[count++] = op;
        }
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
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        trim(line);

        char *token;
        token = strtok(line, ",");
        if(token) strcpy(cfg->user_list[count].username, token);
        else continue;

        token = strtok(NULL, ",");
        if(token) strcpy(cfg->user_list[count].ak, token);
        else continue;

        token = strtok(NULL, ",");
        if(token) strcpy(cfg->user_list[count].sk, token);
        else continue;

        count++;
    }
    fclose(fp);

    if (count < cfg->target_user_count) {
        printf("[Config Error] 'Users=%d' defined in config, but only found %d valid users in '%s'.\n", 
               cfg->target_user_count, count, filename);
        return -1;
    }

    cfg->loaded_user_count = count;
    return count;
}

/**
 * 解析 SSL 版本配置
 * val: 配置字符串 (1.0, 1.1, 1.2, 1.3)
 * is_min: 1 表示解析 MinVersion, 0 表示解析 MaxVersion
 * return: 对应的 CURL 常量值，如果非法返回 -1
 */
static long parse_ssl_version_str(const char *val, int is_min) {
    // 规则 5 & 10: 为空时的默认值
    if (val == NULL || strlen(val) == 0) {
        if (is_min) return CURL_SSLVERSION_TLSv1_0;    // Min 默认 1.0 (规则5)
        else return OBS_SSLVERSION_TLSv1_3;            // Max 默认 1.3 (规则10)
    }

    if (strcmp(val, "1.0") == 0) return CURL_SSLVERSION_TLSv1_0;
    if (strcmp(val, "1.1") == 0) return CURL_SSLVERSION_TLSv1_1;
    if (strcmp(val, "1.2") == 0) return CURL_SSLVERSION_TLSv1_2;
    if (strcmp(val, "1.3") == 0) return OBS_SSLVERSION_TLSv1_3;

    return -1; // 非法值
}

int load_config(const char *filename, Config *cfg) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("Error: Cannot open config file: %s\n", filename);
        return -1;
    }
    
    // 初始化默认值
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

    // [新增] 安全相关配置默认初始化
    cfg->gm_mode_switch = 0;    // 默认 false
    cfg->mutual_ssl_switch = 0; // 默认 false
    cfg->client_cert_path[0] = '\0';
    cfg->client_key_path[0] = '\0';
    cfg->client_key_password[0] = '\0';

    // 临时存储 SSL 版本字符串，用于后续校验
    char ssl_min_str[16] = {0};
    char ssl_max_str[16] = {0};

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '[' || line[0] == '\r' || line[0] == '\n') continue;
        
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        
        char *key = line;
        char *val = eq + 1;
        trim(val);

        if (strcmp(key, "Endpoint") == 0) strcpy(cfg->endpoint, val);
        else if (strcmp(key, "Protocol") == 0) strcpy(cfg->protocol, val);
        else if (strcmp(key, "KeepAlive") == 0) cfg->keep_alive = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "LogLevel") == 0) cfg->log_level = log_level_from_string(val);
        else if (strcmp(key, "ObjNamePatternHash") == 0) cfg->obj_name_pattern_hash = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "EnableCheckpoint") == 0) cfg->enable_checkpoint = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "UploadFilePath") == 0) strcpy(cfg->upload_file_path, val);
        
        // 多用户配置
        else if (strcmp(key, "BucketNamePrefix") == 0) strcpy(cfg->bucket_name_prefix, val);
        else if (strcmp(key, "BucketNameFixed") == 0) strcpy(cfg->bucket_name_fixed, val);
        else if (strcmp(key, "Users") == 0) cfg->target_user_count = atoi(val);
        else if (strcmp(key, "ThreadsPerUser") == 0) cfg->threads_per_user = atoi(val);

        // 测试计划配置
        else if (strcmp(key, "RequestsPerThread") == 0) cfg->requests_per_thread = atoi(val);
        else if (strcmp(key, "TestCase") == 0) cfg->test_case = atoi(val);
        else if (strcmp(key, "ObjectSize") == 0) cfg->object_size = atoll(val);
        else if (strcmp(key, "PartSize") == 0) cfg->part_size = atoll(val);
        else if (strcmp(key, "KeyPrefix") == 0) strcpy(cfg->key_prefix, val);
        else if (strcmp(key, "MixOperation") == 0) cfg->mix_op_count = parse_mix_ops(val, cfg->mix_ops, MAX_MIX_OPS);
        else if (strcmp(key, "MixLoopCount") == 0) cfg->mix_loop_count = atoll(val);
        else if (strcmp(key, "RunSeconds") == 0) cfg->run_seconds = atoi(val);

        // [新增] 安全与国密配置解析
        else if (strcmp(key, "GmModeSwitch") == 0) {
            cfg->gm_mode_switch = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        }
        else if (strcmp(key, "SslMinVersion") == 0) strcpy(ssl_min_str, val);
        else if (strcmp(key, "SslMaxVersion") == 0) strcpy(ssl_max_str, val);
        else if (strcmp(key, "MutualSslSwitch") == 0) {
            cfg->mutual_ssl_switch = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        }
        else if (strcmp(key, "ClientCertPath") == 0) strcpy(cfg->client_cert_path, val);
        else if (strcmp(key, "ClientKeyPath") == 0) strcpy(cfg->client_key_path, val);
        else if (strcmp(key, "ClientKeyPassword") == 0) strcpy(cfg->client_key_password, val);
    }
    
    // 基础校验
    if (cfg->part_size <= 0) cfg->part_size = 5 * 1024 * 1024; 
    if (cfg->target_user_count <= 0) {
        printf("[Config Error] 'Users' must be greater than 0.\n");
        fclose(fp);
        return -1;
    }

    // [新增] 规则4: 解析 SSL 版本为 long
    cfg->ssl_min_version = parse_ssl_version_str(ssl_min_str, 1);
    cfg->ssl_max_version = parse_ssl_version_str(ssl_max_str, 0);

    // [新增] 规则2 & 3: 国密模式下的 SSL 校验
    if (cfg->gm_mode_switch) {
        if (cfg->ssl_min_version == -1) {
            printf("[Config Error] Invalid SslMinVersion: '%s'. Allowed: 1.0, 1.1, 1.2, 1.3\n", ssl_min_str);
            fclose(fp);
            return -1;
        }
        if (cfg->ssl_max_version == -1) {
            printf("[Config Error] Invalid SslMaxVersion: '%s'. Allowed: 1.0, 1.1, 1.2, 1.3\n", ssl_max_str);
            fclose(fp);
            return -1;
        }
        // 校验 Min <= Max
        if (cfg->ssl_min_version > cfg->ssl_max_version) {
            printf("[Config Error] SslMinVersion (%s) cannot be greater than SslMaxVersion (%s).\n", 
                   strlen(ssl_min_str) ? ssl_min_str : "1.0(Default)", 
                   strlen(ssl_max_str) ? ssl_max_str : "1.3(Default)");
            fclose(fp);
            return -1;
        }
    }

    // [新增] 规则7: 双向认证参数校验
    if (cfg->mutual_ssl_switch) {
        if (strlen(cfg->client_cert_path) == 0) {
            printf("[Config Error] ClientCertPath is required when MutualSslSwitch is true.\n");
            fclose(fp);
            return -1;
        }
        if (strlen(cfg->client_key_path) == 0) {
            printf("[Config Error] ClientKeyPath is required when MutualSslSwitch is true.\n");
            fclose(fp);
            return -1;
        }
    }

    // 加载用户文件
    if (load_users_file("users.dat", cfg) < 0) {
        fclose(fp);
        return -1;
    }
    
    if (cfg->threads_per_user <= 0) cfg->threads_per_user = 1;
    cfg->threads = cfg->loaded_user_count * cfg->threads_per_user;
        
    printf("[Config] Multi-User Mode: %d Users Loaded (Validated). %d Threads/User. Total Threads: %d\n", 
           cfg->loaded_user_count, cfg->threads_per_user, cfg->threads);

    // 混合模式判定
    if (cfg->test_case == TEST_CASE_MIX) {
        if (cfg->mix_op_count > 0) {
            cfg->use_mix_mode = 1;
            if (cfg->mix_loop_count <= 0) cfg->mix_loop_count = 1;
            printf("[Config] MixMode ENABLED.\n");
        } else {
            cfg->use_mix_mode = 0;
        }
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
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

