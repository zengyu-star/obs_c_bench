#include "bench.h"
#include <strings.h> 
#include <libgen.h>  
#include <string.h> 

typedef struct {
    char *buffer;
    long long buffer_size;
    long long cur_offset;
} put_buffer_data;

typedef struct {
    long long total_downloaded;
} get_buffer_data;

typedef struct {
    obs_status ret_status;
    char error_msg[256];
    char returned_upload_id[256]; 
    char returned_etag[256];
} status_holder;

typedef struct {
    void *data_ctx;
    status_holder *status_ctx;
} callback_context;

obs_status response_properties_callback(const obs_response_properties *properties, void *callback_data) {
    callback_context *ctx = (callback_context *)callback_data;
    if (ctx && ctx->status_ctx && properties) {
        if (properties->etag) {
            snprintf(ctx->status_ctx->returned_etag, sizeof(ctx->status_ctx->returned_etag), "%s", properties->etag);
        }
    }
    return OBS_STATUS_OK;
}

void response_complete_callback(obs_status status, const obs_error_details *error, void *callback_data) {
    callback_context *ctx = (callback_context *)callback_data;
    if (ctx && ctx->status_ctx) {
        ctx->status_ctx->ret_status = status;
        if (status != OBS_STATUS_OK && error && error->message) {
             snprintf(ctx->status_ctx->error_msg, sizeof(ctx->status_ctx->error_msg), "%s", error->message);
        }
    }
}

int put_buffer_callback(int buffer_size, char *buffer, void *callback_data) {
    callback_context *ctx = (callback_context *)callback_data;
    put_buffer_data *pdata = (put_buffer_data *)ctx->data_ctx;
    if (!pdata) return 0;

    long long remaining = pdata->buffer_size - pdata->cur_offset;
    int to_read = (remaining > buffer_size) ? buffer_size : (int)remaining;

    if (to_read > 0) {
        memcpy(buffer, pdata->buffer + pdata->cur_offset, to_read);
        pdata->cur_offset += to_read;
    }
    return to_read;
}

obs_status get_buffer_callback(int buffer_size, const char *buffer, void *callback_data) {
    callback_context *ctx = (callback_context *)callback_data;
    get_buffer_data *gdata = (get_buffer_data *)ctx->data_ctx;
    if (gdata) gdata->total_downloaded += buffer_size;
    return OBS_STATUS_OK;
}

obs_status complete_multipart_upload_callback(const char *location, const char *bucket,
                                              const char *key, const char* etag,
                                              void *callback_data) {
    return OBS_STATUS_OK;
}

obs_status list_objects_callback(int is_truncated, const char *next_marker,
                                 int contents_count,  const obs_list_objects_content *contents,
                                 int common_prefixes_count, const char **common_prefixes,
                                 void *callback_data) {
    return OBS_STATUS_OK;
}

void upload_file_complete_callback(obs_status status, char *result_message, int part_count_return,
                                   obs_upload_file_part_info * upload_info_list, void *callback_data) {
    callback_context *ctx = (callback_context *)callback_data;
    if (ctx && ctx->status_ctx) {
        ctx->status_ctx->ret_status = status;
        if (status != OBS_STATUS_OK && result_message) {
             snprintf(ctx->status_ctx->error_msg, sizeof(ctx->status_ctx->error_msg), "%s", result_message);
        }
    }
}

// [修改] setup_options: 注入国密与双向认证配置
static void setup_options(obs_options *option, WorkerArgs *args) {
    init_obs_options(option);
    
    // 基础配置
    option->bucket_options.host_name = args->config->endpoint;
    option->bucket_options.bucket_name = args->effective_bucket;
    option->bucket_options.access_key = args->effective_ak;
    option->bucket_options.secret_access_key = args->effective_sk;

    if (strcasecmp(args->config->protocol, "http") == 0) {
        option->bucket_options.protocol = OBS_PROTOCOL_HTTP;
    } else {
        option->bucket_options.protocol = OBS_PROTOCOL_HTTPS; 
    }
    option->request_options.keep_alive = (args->config->keep_alive != 0);

    // [新增] 规则1: 国密模式开关
    if (args->config->gm_mode_switch) {
        option->request_options.gm_mode_switch = OBS_GM_MODE_OPEN;
    } else {
        option->request_options.gm_mode_switch = OBS_GM_MODE_CLOSE;
    }

    // [新增] 规则4: SSL 版本注入 (config_loader 已完成解析和校验)
    option->request_options.ssl_min_version = args->config->ssl_min_version;
    option->request_options.ssl_max_version = args->config->ssl_max_version;

    // [新增] 规则5: 双向认证开关
    if (args->config->mutual_ssl_switch) {
        option->request_options.mutual_ssl_switch = OBS_MUTUAL_SSL_OPEN;
    } else {
        option->request_options.mutual_ssl_switch = OBS_MUTUAL_SSL_CLOSE;
    }

    // [新增] 规则6: 双向认证证书配置
    // 仅当开启时才注入，防止空指针或无效路径影响 SDK
    if (args->config->mutual_ssl_switch) {
        option->request_options.client_cert_path = args->config->client_cert_path;
        option->request_options.client_key_path = args->config->client_key_path;
        
        // 密码允许为空
        if (strlen(args->config->client_key_password) > 0) {
            option->request_options.client_key_password = args->config->client_key_password;
        }
    }
}

obs_status run_put_benchmark(WorkerArgs *args, char *key) {
    obs_options option;
    setup_options(&option, args);
    
    put_buffer_data p_data = {args->data_buffer, args->config->object_size, 0};
    status_holder s_holder = {OBS_STATUS_BUTT, {0}, {0}, {0}}; 
    callback_context ctx = {&p_data, &s_holder};

    obs_put_properties put_props;
    init_put_properties(&put_props);

    obs_put_object_handler handler = {0};
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.put_object_data_callback = &put_buffer_callback;

    put_object(&option, key, args->config->object_size, &put_props, NULL, &handler, &ctx);
    return s_holder.ret_status;
}

obs_status run_get_benchmark(WorkerArgs *args, char *key) {
    obs_options option;
    setup_options(&option, args);

    obs_object_info obj_info = {0};
    obj_info.key = key;

    obs_get_conditions conditions;
    init_get_properties(&conditions);

    get_buffer_data g_data = {0};
    status_holder s_holder = {OBS_STATUS_BUTT, {0}};
    callback_context ctx = {&g_data, &s_holder};

    obs_get_object_handler handler = {0};
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.get_object_data_callback = &get_buffer_callback;

    get_object(&option, &obj_info, &conditions, NULL, &handler, &ctx);
    return s_holder.ret_status;
}

obs_status run_delete_benchmark(WorkerArgs *args, char *key) {
    obs_options option;
    setup_options(&option, args);

    status_holder s_holder = {OBS_STATUS_BUTT, {0}};
    callback_context ctx = {NULL, &s_holder};

    obs_response_handler handler = {0};
    handler.properties_callback = &response_properties_callback;
    handler.complete_callback = &response_complete_callback;

    obs_object_info obj_info = {0};
    obj_info.key = key;
    
    delete_object(&option, &obj_info, &handler, &ctx);
    return s_holder.ret_status;
}

obs_status run_list_benchmark(WorkerArgs *args) {
    obs_options option;
    setup_options(&option, args);

    status_holder s_holder = {OBS_STATUS_BUTT, {0}};
    callback_context ctx = {NULL, &s_holder};

    obs_list_objects_handler handler = {0};
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.list_Objects_callback = &list_objects_callback;

    list_bucket_objects(&option, args->config->key_prefix, NULL, NULL, 100, &handler, &ctx);
    return s_holder.ret_status;
}

obs_status run_multipart_benchmark(WorkerArgs *args, char *key) {
    obs_options option;
    setup_options(&option, args);
    obs_status status = OBS_STATUS_OK;

    status_holder init_holder = {OBS_STATUS_BUTT, {0}, {0}, {0}};
    callback_context init_ctx = {NULL, &init_holder};
    obs_put_properties put_props;
    init_put_properties(&put_props);

    obs_response_handler init_handler = {0};
    init_handler.properties_callback = &response_properties_callback;
    init_handler.complete_callback = &response_complete_callback;
    
    char upload_id[256] = {0};
    initiate_multi_part_upload(&option, key, sizeof(upload_id), upload_id, 
                               &put_props, NULL, &init_handler, &init_ctx);
    
    if (init_holder.ret_status != OBS_STATUS_OK) return init_holder.ret_status;

    if (strlen(upload_id) == 0 && strlen(init_holder.returned_upload_id) > 0) {
        strcpy(upload_id, init_holder.returned_upload_id);
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
        long long offset = i * part_size;
        long long current_part_size = (total_size - offset > part_size) ? part_size : (total_size - offset);

        obs_upload_part_info part_info = {0};
        part_info.part_number = i + 1;
        part_info.upload_id = upload_id;

        status_holder part_holder = {OBS_STATUS_BUTT, {0}, {0}, {0}};
        put_buffer_data part_data_ctx = {args->data_buffer, current_part_size, 0}; 
        callback_context part_ctx = {&part_data_ctx, &part_holder};
        
        obs_upload_handler part_handler = {0};
        part_handler.response_handler.properties_callback = &response_properties_callback;
        part_handler.response_handler.complete_callback = &response_complete_callback;
        part_handler.upload_data_callback = &put_buffer_callback;

        upload_part(&option, key, &part_info, current_part_size, 
                    &put_props, NULL, &part_handler, &part_ctx);
        
        if (part_holder.ret_status != OBS_STATUS_OK) {
            status = part_holder.ret_status;
            for(int k=0; k<i; k++) if(part_infos[k].etag) free(part_infos[k].etag);
            free(part_infos);
            return status;
        }
        part_infos[i].part_number = i + 1;
        part_infos[i].etag = strdup(part_holder.returned_etag);
    }

    status_holder complete_holder = {OBS_STATUS_BUTT, {0}, {0}, {0}};
    callback_context complete_ctx = {NULL, &complete_holder};
    
    obs_complete_multi_part_upload_handler complete_handler = {0};
    complete_handler.response_handler.properties_callback = &response_properties_callback;
    complete_handler.response_handler.complete_callback = &response_complete_callback;
    complete_handler.complete_multipart_upload_callback = &complete_multipart_upload_callback;

    complete_multi_part_upload(&option, key, upload_id, part_count, part_infos, 
                               &put_props, &complete_handler, &complete_ctx);
    
    for(int i=0; i<part_count; i++) {
        if(part_infos[i].etag) free(part_infos[i].etag);
    }
    free(part_infos);

    return complete_holder.ret_status;
}

obs_status run_upload_file_benchmark(WorkerArgs *args, char *key) {
    obs_options option;
    setup_options(&option, args);

    status_holder s_holder = {OBS_STATUS_BUTT, {0}, {0}, {0}};
    callback_context ctx = {NULL, &s_holder};

    obs_put_properties put_props;
    init_put_properties(&put_props);
    
    int pause_flag = 0;

    obs_upload_file_configuration upload_conf = {0};
    memset(&upload_conf, 0, sizeof(obs_upload_file_configuration));

    upload_conf.upload_file = args->config->upload_file_path; 
    upload_conf.part_size = args->config->part_size;
    
    char checkpoint_path[512] = {0};
    char *path_copy = strdup(args->config->upload_file_path);
    char *fname = basename(path_copy);
    snprintf(checkpoint_path, sizeof(checkpoint_path), "upload_checkpoint/%d-%s.xml", 
             args->thread_id, fname);
    free(path_copy); 

    upload_conf.check_point_file = checkpoint_path;
    upload_conf.enable_check_point = 1; 
    upload_conf.task_num = 10; 
    
    upload_conf.put_properties = &put_props;
    upload_conf.pause_upload_flag = &pause_flag; 

    obs_upload_file_server_callback server_cb;
    memset(&server_cb, 0, sizeof(obs_upload_file_server_callback));
    
    obs_upload_file_response_handler handler;
    memset(&handler, 0, sizeof(obs_upload_file_response_handler));
    
    handler.response_handler.properties_callback = &response_properties_callback;
    handler.response_handler.complete_callback = &response_complete_callback;
    handler.upload_file_callback = &upload_file_complete_callback;

    upload_file(&option, key, NULL, &upload_conf, server_cb, &handler, &ctx);

    return s_holder.ret_status;
}

