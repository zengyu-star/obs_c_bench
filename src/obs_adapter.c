#include "bench.h"
#include <limits.h>
#include <strings.h> 
#include <libgen.h>  
#include <string.h> 

typedef struct {
    WorkerArgs *args;           
    long long total_processed;
    long long expected_content_length;
    int validation_failed;      
    obs_status ret_status;
    char error_msg[256];
    char returned_upload_id[256]; 
    char returned_etag[256];

    long long pattern_start_offset; 
    int skip_validation;            
    
    char request_id[64];
    uint64_t last_reported_bytes; 
} transfer_context;

obs_status response_properties_callback(const obs_response_properties *properties, void *callback_data) {
    transfer_context *ctx = (transfer_context *)callback_data;
    if (ctx && properties) {
        if (properties->content_length > 0) {
            ctx->expected_content_length = properties->content_length;
        }

        if (properties->etag) {
            snprintf(ctx->returned_etag, sizeof(ctx->returned_etag), "%s", properties->etag);
        }
        
        if (properties->request_id) {
            snprintf(ctx->request_id, sizeof(ctx->request_id), "%s", properties->request_id);
        }
    }
    return OBS_STATUS_OK;
}

void response_complete_callback(obs_status status, const obs_error_details *error, void *callback_data) {
    transfer_context *ctx = (transfer_context *)callback_data;
    if (ctx) {
        ctx->ret_status = status;
        if (status != OBS_STATUS_OK && error && error->message) {
             snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", error->message);
        }
    }
}

int put_buffer_callback_optimized(int buffer_size, char *buffer, void *callback_data) {
    if (g_graceful_stop) return -1; 
    
    transfer_context *ctx = (transfer_context *)callback_data;
    WorkerArgs *args = ctx->args;
    
    if (!args->pattern_buffer || !buffer) return 0;

    long long offset = ctx->total_processed & args->pattern_mask;
    int bytes_copied = 0;
    while (bytes_copied < buffer_size) {
        int available = args->pattern_size - offset;
        int to_copy = (buffer_size - bytes_copied < available) ? 
                      (buffer_size - bytes_copied) : available;
        memcpy(buffer + bytes_copied, args->pattern_buffer + offset, to_copy);
        
        // 实时累加成功处理的字节数，确保即便大请求未结束也能看到带宽
        args->stats.total_success_bytes += to_copy;

        bytes_copied += to_copy;
        ctx->total_processed += to_copy;
        offset = ctx->total_processed & args->pattern_mask; 
    }
    return bytes_copied;
}

obs_status get_buffer_callback_optimized(int buffer_size, const char *buffer, void *callback_data) {
    if (g_graceful_stop) return OBS_STATUS_InternalError; 

    transfer_context *ctx = (transfer_context *)callback_data;
    WorkerArgs *args = ctx->args;

    if (!args->config->enable_data_validation || ctx->skip_validation) {
        ctx->total_processed += buffer_size;
        args->stats.total_success_bytes += buffer_size; // 实时累加
        return OBS_STATUS_OK;
    }
    
    if (ctx->validation_failed) return OBS_STATUS_OK;
    if (!args->pattern_buffer || !buffer) return OBS_STATUS_InternalError;

    long long absolute_pos = ctx->pattern_start_offset + ctx->total_processed;
    long long offset = absolute_pos & args->pattern_mask;

    int bytes_checked = 0;
    
    while (bytes_checked < buffer_size) {
        int available = args->pattern_size - offset;
        int to_check = (buffer_size - bytes_checked < available) ? 
                       (buffer_size - bytes_checked) : available;
        
        if (memcmp(buffer + bytes_checked, args->pattern_buffer + offset, to_check) != 0) {
             ctx->validation_failed = 1;
             LOG_ERROR("[DATA_CORRUPTION] ReqID: %s, ObjectCtx: %s, Abs Offset: %lld, Pattern Offset: %lld, Check Len: %d", 
                       (strlen(ctx->request_id) > 0) ? ctx->request_id : "UNKNOWN_REQ_ID",
                       args->username, absolute_pos + bytes_checked, offset, to_check);
             return OBS_STATUS_InternalError; 
        }
        
        // 验证成功后实时累加
        args->stats.total_success_bytes += to_check;

        bytes_checked += to_check;
        ctx->total_processed += to_check;
        absolute_pos += to_check;
        offset = absolute_pos & args->pattern_mask;
    }
    return OBS_STATUS_OK;
}

// ----------------------------------------------------------------------------
// 断点续传进度回调：用于实时统计带宽
// ----------------------------------------------------------------------------
void resumable_progress_callback(double progress, uint64_t uploadedSize, uint64_t fileTotalSize, void *callback_data) {
    transfer_context *ctx = (transfer_context *)callback_data;
    if (ctx && ctx->args) {
        if (uploadedSize > ctx->last_reported_bytes) {
            uint64_t increment = uploadedSize - ctx->last_reported_bytes;
            ctx->args->stats.total_success_bytes += increment;
            ctx->last_reported_bytes = uploadedSize;
        }
    }
}

obs_status complete_multipart_upload_callback(const char *location, const char *bucket,
                                              const char *key, const char* etag,
                                              void *callback_data) {
    return OBS_STATUS_OK;
}

obs_status list_objects_callback(int is_truncated, const char *next_marker,
                                 int contents_count, const obs_list_objects_content *contents,
                                 int common_prefixes_count, const char **common_prefixes,
                                 void *callback_data) {
    return OBS_STATUS_OK;
}

void upload_file_complete_callback(obs_status status, char *result_message, int part_count_return,
                                   obs_upload_file_part_info * upload_info_list, void *callback_data) {
    transfer_context *ctx = (transfer_context *)callback_data;
    if (ctx) {
        ctx->ret_status = status;
        if (status != OBS_STATUS_OK && result_message) {
             snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", result_message);
        }
        
        // 增加详尽的分段上传日志，仅在有分段信息时打印
        if (part_count_return > 0 && (ctx->args->config->enable_detail_log || status != OBS_STATUS_OK)) {
            printf("\n--- Resumable Upload Part Details (Total: %d) ---\n", part_count_return);
            for (int i = 0; i < part_count_return; i++) {
                printf("Part %d: StartByte=%llu, Size=%llu, Status=%s\n", 
                       upload_info_list[i].part_num,
                       (unsigned long long)upload_info_list[i].start_byte,
                       (unsigned long long)upload_info_list[i].part_size,
                       obs_get_status_name(upload_info_list[i].status_return));
            }
            printf("--------------------------------------------------\n");
        }
    }
}

static int client_sign_password_callback(void *context, char *buf, int buf_len) {
    char *password = (char *)context;
    if (!password || !buf) return 0;
    int len = (int)strlen(password);
    if (len >= buf_len) {
        return 0;
    }
    strcpy(buf, password);
    return len;
}

static void setup_options(obs_options *option, WorkerArgs *args) {
    memset(option, 0, sizeof(obs_options));
    init_obs_options(option);
    
    option->bucket_options.host_name = args->config->endpoint;
    option->bucket_options.bucket_name = args->effective_bucket;
    option->bucket_options.access_key = args->effective_ak;
    option->bucket_options.secret_access_key = args->effective_sk;

    if (args->config->is_temporary_token && strlen(args->effective_token) > 0) {
        option->bucket_options.token = args->effective_token;
    }

    option->bucket_options.protocol = (strcasecmp(args->config->protocol, "http") == 0) ? OBS_PROTOCOL_HTTP : OBS_PROTOCOL_HTTPS;
    
    option->request_options.connect_time = args->config->connect_timeout_sec * 1000;
    option->request_options.max_connected_time = args->config->request_timeout_sec * 1000;
    option->request_options.keep_alive = (args->config->keep_alive != 0);

    // Default values
    option->bucket_options.useCname = false;
    option->bucket_options.uri_style = OBS_URI_STYLE_VIRTUALHOST;

    if (strlen(args->config->gm_auth_mode) > 0) {
        option->bucket_options.useCname = true;
        option->bucket_options.uri_style = OBS_URI_STYLE_PATH;
        option->request_options.ssl_verify_peer = OBS_SSL_VERIFYPEER_OPEN;
        option->request_options.server_cert_path = args->config->server_cert_path;

        if (strcmp(args->config->gm_auth_mode, "GM_Mutual") == 0) {
            option->request_options.client_auth_switch = OBS_CLIENT_AUTH_OPEN;
            option->request_options.gm_mode_switch = OBS_GM_MODE_OPEN;
            option->request_options.client_sign_cert_path = args->config->client_sign_cert_path;
            option->request_options.client_sign_key_path = args->config->client_sign_key_path;
            option->request_options.client_enc_cert_path = args->config->client_enc_cert_path;
            option->request_options.client_enc_key_path = args->config->client_enc_key_path;
            option->request_options.ssl_version = 8;
        } else if (strcmp(args->config->gm_auth_mode, "GM_Oneway") == 0) {
            option->request_options.client_auth_switch = OBS_CLIENT_AUTH_CLOSE;
            option->request_options.gm_mode_switch = OBS_GM_MODE_OPEN;
            option->request_options.ssl_version = 8;
        } else if (strcmp(args->config->gm_auth_mode, "Inter_Mutual") == 0) {
            option->request_options.client_auth_switch = OBS_CLIENT_AUTH_OPEN;
            option->request_options.gm_mode_switch = OBS_GM_MODE_CLOSE;
            option->request_options.client_sign_cert_path = args->config->client_sign_cert_path;
            option->request_options.client_sign_key_path = args->config->client_sign_key_path;
        } else if (strcmp(args->config->gm_auth_mode, "Inter_Oneway") == 0) {
            option->request_options.client_auth_switch = OBS_CLIENT_AUTH_CLOSE;
            option->request_options.gm_mode_switch = OBS_GM_MODE_CLOSE;
        }

        if (strlen(args->config->client_sign_key_password) > 0) {
            option->request_options.password_callback = client_sign_password_callback;
            option->request_options.password_callback_context = args->config->client_sign_key_password;
        }
    } else {
        // Fallback for non-mode based SSL settings if needed
        if (strlen(args->config->server_cert_path) > 0) {
            option->request_options.server_cert_path = args->config->server_cert_path;
        }
    }
}

obs_status run_put_benchmark(WorkerArgs *args, char *key, long long object_size, char *out_req_id) {
    obs_options option;
    setup_options(&option, args);
    transfer_context ctx = {args, 0, 0, 0, OBS_STATUS_BUTT, {0}, {0}, {0}, 0, 0, {0}};

    obs_put_properties put_props;
    init_put_properties(&put_props);

    obs_put_object_handler handler = {0};
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.put_object_data_callback = &put_buffer_callback_optimized;

    put_object(&option, key, object_size, &put_props, NULL, &handler, &ctx);
    
    if (out_req_id && strlen(ctx.request_id) > 0) {
        strcpy(out_req_id, ctx.request_id);
    }
    
    return ctx.ret_status;
}

static void apply_range_conditions(const char *range_str, obs_get_conditions *cond, transfer_context *ctx) {
    if (!range_str || strlen(range_str) == 0) {
        cond->start_byte = 0;
        cond->byte_count = 0;
        ctx->pattern_start_offset = 0;
        return;
    }

    long long start = 0, end = 0;
    char *hyphen = strchr(range_str, '-');
    
    if (hyphen) {
        if (hyphen == range_str) {
            end = atoll(hyphen + 1);
            cond->start_byte = 0;
            cond->byte_count = end + 1; 
            ctx->pattern_start_offset = 0;
            ctx->skip_validation = 0; 
            
        } else if (*(hyphen + 1) == '\0') {
            *hyphen = '\0';
            start = atoll(range_str);
            cond->start_byte = start;
            cond->byte_count = 0; 
            ctx->pattern_start_offset = start;
            *hyphen = '-'; 
        } else {
            *hyphen = '\0';
            start = atoll(range_str);
            end = atoll(hyphen + 1);
            cond->start_byte = start;
            cond->byte_count = end - start + 1;
            ctx->pattern_start_offset = start;
            *hyphen = '-';
        }
    }
}

obs_status run_get_benchmark(WorkerArgs *args, char *key, char *range_str, char *out_req_id) {
    obs_options option;
    setup_options(&option, args);
    obs_object_info obj_info = {0};
    obj_info.key = key;
    obs_get_conditions conditions;
    init_get_properties(&conditions);

    transfer_context ctx = {args, 0, 0, 0, OBS_STATUS_BUTT, {0}, {0}, {0}, 0, 0, {0}};
    
    if (range_str) {
        char *temp_range = strdup(range_str);
        apply_range_conditions(temp_range, &conditions, &ctx);
        free(temp_range);
    }

    obs_get_object_handler handler = {0};
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.get_object_data_callback = &get_buffer_callback_optimized;

    get_object(&option, &obj_info, &conditions, NULL, &handler, &ctx);
    
    if (out_req_id && strlen(ctx.request_id) > 0) {
        strcpy(out_req_id, ctx.request_id);
    }

    if (ctx.ret_status == OBS_STATUS_OK && ctx.expected_content_length > 0) {
        if (ctx.total_processed != ctx.expected_content_length) {
            LOG_ERROR("[DATA_INCOMPLETE] ReqID: %s, Key: %s, Expected: %lld, Got: %lld", 
                      (strlen(ctx.request_id) > 0) ? ctx.request_id : "UNKNOWN_REQ_ID",
                      key, ctx.expected_content_length, ctx.total_processed);
            args->stats.fail_validation_count++;
            return OBS_STATUS_InternalError;
        }
    }
    
    if (ctx.validation_failed) {
        args->stats.fail_validation_count++; 
        return OBS_STATUS_InternalError; 
    }

    return ctx.ret_status;
}

obs_status run_delete_benchmark(WorkerArgs *args, char *key, char *out_req_id) {
    obs_options option;
    setup_options(&option, args);
    transfer_context ctx = {args, 0, 0, 0, OBS_STATUS_BUTT, {0}, {0}, {0}, 0, 0, {0}};
    obs_object_info obj_info = {0};
    obj_info.key = key;
    obs_response_handler handler = {0};
    handler.properties_callback = &response_properties_callback;
    handler.complete_callback = &response_complete_callback;
    
    delete_object(&option, &obj_info, &handler, &ctx);

    if (out_req_id && strlen(ctx.request_id) > 0) {
        strcpy(out_req_id, ctx.request_id);
    }
    
    return ctx.ret_status;
}

obs_status run_list_benchmark(WorkerArgs *args, char *out_req_id) {
    obs_options option;
    setup_options(&option, args);
    transfer_context ctx = {args, 0, 0, 0, OBS_STATUS_BUTT, {0}, {0}, {0}, 0, 0, {0}};
    obs_list_objects_handler handler = {0};
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.list_Objects_callback = &list_objects_callback;
    
    list_bucket_objects(&option, args->config->key_prefix, NULL, NULL, 100, &handler, &ctx);
    
    if (out_req_id && strlen(ctx.request_id) > 0) {
        strcpy(out_req_id, ctx.request_id);
    }
    
    return ctx.ret_status;
}

obs_status run_multipart_benchmark(WorkerArgs *args, char *key, char *out_req_id) {
    obs_options option;
    setup_options(&option, args);
    transfer_context ctx = {args, 0, 0, 0, OBS_STATUS_BUTT, {0}, {0}, {0}, 0, 0, {0}};

    obs_put_properties put_props;
    init_put_properties(&put_props);

    // ==========================================
    // 第一步：初始化多段上传任务 (Initiate)
    // ==========================================
    char upload_id[256] = {0};
    obs_response_handler init_handler = {0};
    init_handler.properties_callback = &response_properties_callback;
    init_handler.complete_callback = &response_complete_callback;

    initiate_multi_part_upload(&option, key, sizeof(upload_id), upload_id, 
                               &put_props, NULL, &init_handler, &ctx);
                               
    if (ctx.ret_status != OBS_STATUS_OK) {
        return ctx.ret_status;
    }

    // ==========================================
    // 第二步：循环并发/串行上传分段 (Upload Part)
    // ==========================================
    long long part_size = args->config->part_size > 0 ? args->config->part_size : (5 * 1024 * 1024);
    int part_count = args->config->parts_for_each_upload_id;

    obs_complete_upload_Info *complete_infos = (obs_complete_upload_Info *)calloc(part_count, sizeof(obs_complete_upload_Info));
    if (!complete_infos) {
        LOG_ERROR("Failed to allocate memory for multipart upload info");
        return OBS_STATUS_InternalError;
    }

    for (int i = 0; i < part_count; i++) {
        long long current_part_size = part_size;

        obs_upload_part_info part_info = {0};
        part_info.part_number = i + 1;
        part_info.upload_id = upload_id;

        obs_upload_handler up_handler = {0};
        up_handler.response_handler.properties_callback = &response_properties_callback;
        up_handler.response_handler.complete_callback = &response_complete_callback;
        
        up_handler.upload_data_callback = (obs_upload_data_callback *)&put_buffer_callback_optimized; 

        upload_part(&option, key, &part_info, current_part_size, &put_props, NULL, &up_handler, &ctx);

        if (ctx.ret_status != OBS_STATUS_OK) {
            for (int j = 0; j < i; j++) {
                if (complete_infos[j].etag) free(complete_infos[j].etag);
            }
            free(complete_infos);
            return ctx.ret_status;
        }
        
        complete_infos[i].part_number = i + 1;
        complete_infos[i].etag = strdup(ctx.returned_etag);
    }

    // ==========================================
    // 第三步：合并分段 (Complete)
    // ==========================================
    obs_complete_multi_part_upload_handler comp_handler = {0};
    comp_handler.response_handler.properties_callback = &response_properties_callback;
    comp_handler.response_handler.complete_callback = &response_complete_callback;
    
    comp_handler.complete_multipart_upload_callback = &complete_multipart_upload_callback;

    complete_multi_part_upload(&option, key, upload_id, part_count, complete_infos, 
                               &put_props, &comp_handler, &ctx);

    if (out_req_id && strlen(ctx.request_id) > 0) {
        strcpy(out_req_id, ctx.request_id);
    }

    for (int i = 0; i < part_count; i++) {
        if (complete_infos[i].etag) free(complete_infos[i].etag);
    }
    free(complete_infos);

    return ctx.ret_status;
}

obs_status run_upload_file_benchmark(WorkerArgs *args, char *key, char *out_req_id) {
    obs_options option;
    setup_options(&option, args);
    transfer_context ctx = {args, 0, 0, 0, OBS_STATUS_BUTT, {0}, {0}, {0}, 0, 0, {0}};
    obs_put_properties put_props;
    init_put_properties(&put_props);
    char cp_file[PATH_MAX + 256] = {0};
    char abs_path[PATH_MAX] = {0};
    obs_upload_file_configuration upload_conf = {0};
    upload_conf.upload_file = args->config->upload_file_path; 
    upload_conf.part_size = args->config->part_size;
    
    if (args->config->enable_checkpoint) {
        if (realpath("upload_checkpoint", abs_path)) {
             // 为每个 Key 生成唯一的 .cp 文件，避免同一线程并发/先后操作不同 Key 时冲突
             unsigned int key_hash = 0;
             for (char *p = key; *p; p++) key_hash = key_hash * 31 + *p;
             snprintf(cp_file, sizeof(cp_file), "%s/%s_%u.cp", abs_path, args->username, key_hash);
             upload_conf.check_point_file = cp_file;
             upload_conf.enable_check_point = 1;
        } else {
             // Fallback to relative if realpath fails
             snprintf(cp_file, sizeof(cp_file), "upload_checkpoint/bench_thread_%d.cp", args->thread_id);
             upload_conf.check_point_file = cp_file;
             upload_conf.enable_check_point = 1;
        }
    } else {
        upload_conf.check_point_file = NULL;
        upload_conf.enable_check_point = 0;
    }
    
    upload_conf.task_num = args->config->resumable_task_num > 0 ? args->config->resumable_task_num : 1; 
    upload_conf.put_properties = &put_props;
    upload_conf.pause_upload_flag = (int *)&g_graceful_stop; 

    obs_upload_file_server_callback server_cb; 
    memset(&server_cb, 0, sizeof(server_cb));
    
    obs_upload_file_response_handler handler;
    memset(&handler, 0, sizeof(handler));
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.upload_file_callback = &upload_file_complete_callback;
    handler.progress_callback = &resumable_progress_callback;

    upload_file(&option, key, NULL, &upload_conf, server_cb, &handler, &ctx);
    
    if (out_req_id && strlen(ctx.request_id) > 0) {
        strcpy(out_req_id, ctx.request_id);
    }
    
    return ctx.ret_status;
}

