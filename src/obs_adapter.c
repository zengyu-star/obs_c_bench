#include "bench.h"
#include <strings.h> 
#include <libgen.h>  
#include <string.h> 

// [Scheme 3] 传输上下文
typedef struct {
    WorkerArgs *args;           
    long long total_processed;  
    int validation_failed;      
    obs_status ret_status;
    char error_msg[256];
    char returned_upload_id[256]; 
    char returned_etag[256];
} transfer_context;

obs_status response_properties_callback(const obs_response_properties *properties, void *callback_data) {
    transfer_context *ctx = (transfer_context *)callback_data;
    if (ctx && properties && properties->etag) {
        snprintf(ctx->returned_etag, sizeof(ctx->returned_etag), "%s", properties->etag);
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

// 环形上传数据生产逻辑
int put_buffer_callback_optimized(int buffer_size, char *buffer, void *callback_data) {
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
        bytes_copied += to_copy;
        ctx->total_processed += to_copy;
        offset = ctx->total_processed & args->pattern_mask; 
    }
    return bytes_copied;
}

// 环形下载校验逻辑 (增加实时日志)
obs_status get_buffer_callback_optimized(int buffer_size, const char *buffer, void *callback_data) {
    transfer_context *ctx = (transfer_context *)callback_data;
    WorkerArgs *args = ctx->args;

    if (!args->config->enable_data_validation) {
        ctx->total_processed += buffer_size;
        return OBS_STATUS_OK;
    }
    
    if (ctx->validation_failed) return OBS_STATUS_OK;
    if (!args->pattern_buffer || !buffer) return OBS_STATUS_InternalError;

    long long offset = ctx->total_processed & args->pattern_mask;
    int bytes_checked = 0;
    
    while (bytes_checked < buffer_size) {
        int available = args->pattern_size - offset;
        int to_check = (buffer_size - bytes_checked < available) ? 
                       (buffer_size - bytes_checked) : available;
        
        // 执行内存比对
        if (memcmp(buffer + bytes_checked, args->pattern_buffer + offset, to_check) != 0) {
             ctx->validation_failed = 1;
             
             // [新增] 核心日志输出
             LOG_ERROR("[DATA_CORRUPTION] Object: %s, Global Offset: %lld, Pattern Offset: %lld, Check Len: %d", 
                       args->username, ctx->total_processed + bytes_checked, offset, to_check);
             
             // 发现损坏立即报错，由 SDK 终止会话
             return OBS_STATUS_InternalError; 
        }
        
        bytes_checked += to_check;
        ctx->total_processed += to_check;
        offset = ctx->total_processed & args->pattern_mask;
    }
    return OBS_STATUS_OK;
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
    }
}

static void setup_options(obs_options *option, WorkerArgs *args) {
    memset(option, 0, sizeof(obs_options));

    init_obs_options(option);
    option->bucket_options.host_name = args->config->endpoint;
    option->bucket_options.bucket_name = args->effective_bucket;
    option->bucket_options.access_key = args->effective_ak;
    option->bucket_options.secret_access_key = args->effective_sk;
    option->bucket_options.protocol = (strcasecmp(args->config->protocol, "http") == 0) ? OBS_PROTOCOL_HTTP : OBS_PROTOCOL_HTTPS;
    option->request_options.keep_alive = (args->config->keep_alive != 0);
    option->request_options.gm_mode_switch = args->config->gm_mode_switch ? 1 : 0;
    option->request_options.ssl_min_version = args->config->ssl_min_version;
    option->request_options.ssl_max_version = args->config->ssl_max_version;
    if (args->config->mutual_ssl_switch) {
        option->request_options.mutual_ssl_switch = 1; 
        option->request_options.client_cert_path = args->config->client_cert_path;
        option->request_options.client_key_path = args->config->client_key_path;
        if (strlen(args->config->client_key_password) > 0) {
            option->request_options.client_key_password = args->config->client_key_password;
        }
    }
}

obs_status run_put_benchmark(WorkerArgs *args, char *key) {
    obs_options option;
    setup_options(&option, args);
    transfer_context ctx = {args, 0, 0, OBS_STATUS_BUTT, {0}, {0}, {0}};

    obs_put_properties put_props;
    init_put_properties(&put_props);

    obs_put_object_handler handler = {0};
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.put_object_data_callback = &put_buffer_callback_optimized;

    put_object(&option, key, args->config->object_size, &put_props, NULL, &handler, &ctx);
    return ctx.ret_status;
}

obs_status run_get_benchmark(WorkerArgs *args, char *key) {
    obs_options option;
    setup_options(&option, args);
    obs_object_info obj_info = {0};
    obj_info.key = key;
    obs_get_conditions conditions;
    init_get_properties(&conditions);

    transfer_context ctx = {args, 0, 0, OBS_STATUS_BUTT, {0}};
    obs_get_object_handler handler = {0};
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.get_object_data_callback = &get_buffer_callback_optimized;

    get_object(&option, &obj_info, &conditions, NULL, &handler, &ctx);
    
    // 如果 SDK 内部没报错，但逻辑层发现了数据损坏，返回 InternalError 并记录统计
    if (ctx.ret_status == OBS_STATUS_OK && ctx.validation_failed) {
        args->stats.fail_validation_count++; 
        return OBS_STATUS_InternalError; 
    }
    return ctx.ret_status;
}

obs_status run_delete_benchmark(WorkerArgs *args, char *key) {
    obs_options option;
    setup_options(&option, args);
    transfer_context ctx = {args, 0, 0, OBS_STATUS_BUTT, {0}};
    obs_object_info obj_info = {0};
    obj_info.key = key;
    obs_response_handler handler = {0};
    handler.properties_callback = &response_properties_callback;
    handler.complete_callback = &response_complete_callback;
    delete_object(&option, &obj_info, &handler, &ctx);
    return ctx.ret_status;
}

obs_status run_list_benchmark(WorkerArgs *args) {
    obs_options option;
    setup_options(&option, args);
    transfer_context ctx = {args, 0, 0, OBS_STATUS_BUTT, {0}};
    obs_list_objects_handler handler = {0};
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.list_Objects_callback = &list_objects_callback;
    list_bucket_objects(&option, args->config->key_prefix, NULL, NULL, 100, &handler, &ctx);
    return ctx.ret_status;
}

obs_status run_multipart_benchmark(WorkerArgs *args, char *key) {
    obs_options option;
    setup_options(&option, args);

    transfer_context init_ctx = {args, 0, 0, OBS_STATUS_BUTT, {0}};
    obs_put_properties put_props;
    init_put_properties(&put_props);
    obs_response_handler init_handler = {0};
    init_handler.properties_callback = &response_properties_callback;
    init_handler.complete_callback = &response_complete_callback;
    
    char upload_id[256] = {0};
    initiate_multi_part_upload(&option, key, sizeof(upload_id), upload_id, 
                               &put_props, NULL, &init_handler, &init_ctx);
    if (init_ctx.ret_status != OBS_STATUS_OK) return init_ctx.ret_status;
    if (strlen(upload_id) == 0 && strlen(init_ctx.returned_upload_id) > 0) {
        strcpy(upload_id, init_ctx.returned_upload_id);
    }
    if (strlen(upload_id) == 0) return OBS_STATUS_BUTT;

    long long total_size = args->config->object_size;
    long long part_size = args->config->part_size;
    if (part_size <= 0) part_size = 5 * 1024 * 1024; 
    int part_count = (total_size + part_size - 1) / part_size;

    obs_complete_upload_Info *part_infos = malloc(sizeof(obs_complete_upload_Info) * part_count);
    if (!part_infos) return OBS_STATUS_BUTT;
    memset(part_infos, 0, sizeof(obs_complete_upload_Info) * part_count);

    for (int i = 0; i < part_count; i++) {
        long long offset = (long long)i * part_size;
        long long current_part_size = (total_size - offset > part_size) ? part_size : (total_size - offset);

        transfer_context part_ctx = {args, offset, 0, OBS_STATUS_BUTT, {0}, {0}, {0}};

        obs_upload_part_info part_info = {0};
        part_info.part_number = i + 1;
        part_info.upload_id = upload_id;

        obs_upload_handler part_handler = {0};
        part_handler.response_handler.properties_callback = &response_properties_callback;
        part_handler.response_handler.complete_callback = &response_complete_callback;
        part_handler.upload_data_callback = &put_buffer_callback_optimized;

        upload_part(&option, key, &part_info, current_part_size, 
                    &put_props, NULL, &part_handler, &part_ctx);
        
        if (part_ctx.ret_status != OBS_STATUS_OK) {
            free(part_infos);
            for(int k=0; k<i; k++) if(part_infos[k].etag) free(part_infos[k].etag);
            return part_ctx.ret_status;
        }
        part_infos[i].part_number = i + 1;
        part_infos[i].etag = strdup(part_ctx.returned_etag);
    }

    transfer_context complete_ctx = {args, 0, 0, OBS_STATUS_BUTT, {0}};
    obs_complete_multi_part_upload_handler complete_handler = {0};
    complete_handler.response_handler.properties_callback = &response_properties_callback;
    complete_handler.response_handler.complete_callback = &response_complete_callback;
    complete_handler.complete_multipart_upload_callback = &complete_multipart_upload_callback;

    complete_multi_part_upload(&option, key, upload_id, part_count, part_infos, 
                               &put_props, &complete_handler, &complete_ctx);
    
    for(int i=0; i<part_count; i++) if(part_infos[i].etag) free(part_infos[i].etag);
    free(part_infos);
    return complete_ctx.ret_status;
}

obs_status run_upload_file_benchmark(WorkerArgs *args, char *key) {
    obs_options option;
    setup_options(&option, args);
    transfer_context ctx = {args, 0, 0, OBS_STATUS_BUTT, {0}};
    obs_put_properties put_props;
    init_put_properties(&put_props);
    int pause_flag = 0;
    obs_upload_file_configuration upload_conf = {0};
    upload_conf.upload_file = args->config->upload_file_path; 
    upload_conf.part_size = args->config->part_size;
    upload_conf.check_point_file = NULL; 
    upload_conf.enable_check_point = 0; 
    upload_conf.task_num = 1; 
    upload_conf.put_properties = &put_props;
    upload_conf.pause_upload_flag = &pause_flag; 

    obs_upload_file_server_callback server_cb; 
    memset(&server_cb, 0, sizeof(server_cb));
    
    obs_upload_file_response_handler handler;
    memset(&handler, 0, sizeof(handler));
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.upload_file_callback = &upload_file_complete_callback;

    upload_file(&option, key, NULL, &upload_conf, server_cb, &handler, &ctx);
    return ctx.ret_status;
}

