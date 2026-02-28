#include "bench.h"
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
        
        bytes_checked += to_check;
        ctx->total_processed += to_check;
        absolute_pos += to_check;
        offset = absolute_pos & args->pattern_mask;
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
    
    if (args->config->is_temporary_token && strlen(args->effective_token) > 0) {
        option->bucket_options.token = args->effective_token;
    }

    option->bucket_options.protocol = (strcasecmp(args->config->protocol, "http") == 0) ? OBS_PROTOCOL_HTTP : OBS_PROTOCOL_HTTPS;
    
    option->request_options.connect_time = args->config->connect_timeout_sec;
    option->request_options.max_connected_time = args->config->request_timeout_sec;
    option->request_options.keep_alive = (args->config->keep_alive != 0);

    // --- 新版国密及双向认证参数透传 ---
    option->request_options.gm_mode_switch = args->config->gm_mode_switch ? OBS_GM_MODE_OPEN : OBS_GM_MODE_CLOSE;
    option->request_options.mutual_ssl_switch = args->config->mutual_ssl_switch ? OBS_MUTUAL_SSL_OPEN : OBS_MUTUAL_SSL_CLOSE;

    if (strlen(args->config->server_cert_path) > 0) {
        option->request_options.server_cert_path = args->config->server_cert_path;
    }

    if (args->config->mutual_ssl_switch) {
        option->request_options.client_sign_cert_path = args->config->client_sign_cert_path;
        option->request_options.client_sign_key_path = args->config->client_sign_key_path;
        
        if (strlen(args->config->client_sign_key_password) > 0) {
            option->request_options.client_sign_key_password = args->config->client_sign_key_password;
        }
    }

    if (args->config->gm_mode_switch) {
        option->request_options.client_enc_cert_path = args->config->client_enc_cert_path;
        option->request_options.client_enc_key_path = args->config->client_enc_key_path;
    }

    if (strlen(args->config->ssl_cipher_list) > 0) {
        option->request_options.ssl_cipher_list = args->config->ssl_cipher_list;
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

    if (ctx.ret_status == OBS_STATUS_OK) {
        args->stats.total_success_bytes += ctx.total_processed;
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
    return OBS_STATUS_OK; 
}

obs_status run_upload_file_benchmark(WorkerArgs *args, char *key, char *out_req_id) {
    obs_options option;
    setup_options(&option, args);
    transfer_context ctx = {args, 0, 0, 0, OBS_STATUS_BUTT, {0}, {0}, {0}, 0, 0, {0}};
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
    
    if (out_req_id && strlen(ctx.request_id) > 0) {
        strcpy(out_req_id, ctx.request_id);
    }
    
    return ctx.ret_status;
}

