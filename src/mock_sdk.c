#include "../include/mock_eSDKOBS.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static long long mock_put_calls = 0;
static long long mock_get_calls = 0;
static long long mock_del_calls = 0;
static long long mock_list_calls = 0;
static long long mock_init_calls = 0;
static long long mock_part_calls = 0;
static long long mock_complete_calls = 0;
static long long mock_upload_file_calls = 0;

obs_status obs_initialize(int flags) { return OBS_STATUS_OK; }
void obs_deinitialize() {}
const char* obs_get_status_name(obs_status status) { return "OK"; }
void init_obs_options(obs_options *options) { if(options) memset(options, 0, sizeof(obs_options)); }
void init_put_properties(obs_put_properties *options) { if(options) memset(options, 0, sizeof(obs_put_properties)); }
void init_get_properties(obs_get_conditions *options) { if(options) memset(options, 0, sizeof(obs_get_conditions)); }

void put_object(const obs_options *options, char *key, uint64_t content_length,
                obs_put_properties *put_properties, 
                server_side_encryption_params *encryption_params,
                obs_put_object_handler *handler, void *callback_data)
{
    __sync_fetch_and_add(&mock_put_calls, 1);
    
    if (handler->put_object_data_callback) {
        char buf[8192];
        uint64_t remaining = content_length;
        while (remaining > 0) {
            int to_read = (remaining > sizeof(buf)) ? sizeof(buf) : (int)remaining;
            int read = handler->put_object_data_callback(to_read, buf, callback_data);
            if (read <= 0) break;
            remaining -= read;
        }
    }
    if (handler->response_handler.properties_callback) {
        obs_response_properties props;
        memset(&props, 0, sizeof(props));
        props.etag = "mock-etag-12345";
        handler->response_handler.properties_callback(&props, callback_data);
    }
    if (handler->response_handler.complete_callback) {
        handler->response_handler.complete_callback(OBS_STATUS_OK, NULL, callback_data);
    }
}

void get_object(const obs_options *options, obs_object_info *object_info,
                obs_get_conditions *get_conditions, 
                server_side_encryption_params *encryption_params,
                obs_get_object_handler *handler, void *callback_data)
{
    __sync_fetch_and_add(&mock_get_calls, 1);
    
    if (handler->response_handler.properties_callback) {
        obs_response_properties props;
        memset(&props, 0, sizeof(props));
        props.etag = "mock-etag-download";
        // [新增] Mock 默认传输 8192 字节
        props.content_length = 8192; 
        handler->response_handler.properties_callback(&props, callback_data);
    }

    if (handler->get_object_data_callback) {
        char buf[8192];
        memset(buf, 'A', sizeof(buf));
        handler->get_object_data_callback(sizeof(buf), buf, callback_data);
    }
    if (handler->response_handler.complete_callback) {
        handler->response_handler.complete_callback(OBS_STATUS_OK, NULL, callback_data);
    }
}

void delete_object(const obs_options *options, obs_object_info *object_info,
                   obs_response_handler *handler, void *callback_data)
{
    __sync_fetch_and_add(&mock_del_calls, 1);
    if (handler->complete_callback) {
        handler->complete_callback(OBS_STATUS_OK, NULL, callback_data);
    }
}

void list_bucket_objects(const obs_options *options, const char *prefix, const char *marker, 
                         const char *delimiter, int maxkeys, 
                         obs_list_objects_handler *handler, void *callback_data)
{
    __sync_fetch_and_add(&mock_list_calls, 1);
    if (handler->list_Objects_callback) {
        obs_list_objects_content content;
        memset(&content, 0, sizeof(content));
        content.key = "mock-obj";
        content.size = 123;
        content.etag = "mock-etag";
        handler->list_Objects_callback(0, NULL, 1, &content, 0, NULL, callback_data);
    }
    if (handler->response_handler.complete_callback) {
        handler->response_handler.complete_callback(OBS_STATUS_OK, NULL, callback_data);
    }
}

void initiate_multi_part_upload(const obs_options *options, char *key, 
                                int upload_id_return_size, char *upload_id_return,
                                obs_put_properties *put_properties, 
                                server_side_encryption_params *encryption_params,
                                obs_response_handler *handler, void *callback_data)
{
    __sync_fetch_and_add(&mock_init_calls, 1);
    if (upload_id_return) snprintf(upload_id_return, upload_id_return_size, "mock-upload-id");
    if (handler->complete_callback) handler->complete_callback(OBS_STATUS_OK, NULL, callback_data);
}

void upload_part(const obs_options *options, char *key, obs_upload_part_info *part_info,
                 uint64_t content_length, obs_put_properties *put_properties, 
                 server_side_encryption_params *encryption_params,
                 obs_upload_handler *handler, void *callback_data)
{
    __sync_fetch_and_add(&mock_part_calls, 1);
    if (handler->upload_data_callback) {
        char buf[8192];
        uint64_t remaining = content_length;
        while (remaining > 0) {
            int to_read = (remaining > sizeof(buf)) ? sizeof(buf) : (int)remaining;
            int read = handler->upload_data_callback(to_read, buf, callback_data);
            if (read <= 0) break;
            remaining -= read;
        }
    }
    if (handler->response_handler.properties_callback) {
        obs_response_properties props;
        memset(&props, 0, sizeof(props));
        char etag_buf[64];
        snprintf(etag_buf, sizeof(etag_buf), "mock-etag-%d", part_info ? part_info->part_number : 0);
        props.etag = etag_buf;
        handler->response_handler.properties_callback(&props, callback_data);
    }
    if (handler->response_handler.complete_callback) {
        handler->response_handler.complete_callback(OBS_STATUS_OK, NULL, callback_data);
    }
}

void complete_multi_part_upload(const obs_options *options, char *key, char *upload_id, 
                                unsigned int part_count, obs_complete_upload_Info *parts_info,
                                obs_put_properties *put_properties, 
                                obs_complete_multi_part_upload_handler *handler, void *callback_data)
{
    __sync_fetch_and_add(&mock_complete_calls, 1);
    if (handler->response_handler.complete_callback) {
        handler->response_handler.complete_callback(OBS_STATUS_OK, NULL, callback_data);
    }
}

void upload_file(const obs_options *options, char *key, server_side_encryption_params *encryption_params, 
                 obs_upload_file_configuration *upload_file_config, 
                 obs_upload_file_server_callback server_callback, 
                 obs_upload_file_response_handler *handler,
                 void *callback_data)
{
    __sync_fetch_and_add(&mock_upload_file_calls, 1);
    if (handler->upload_file_callback) {
        handler->upload_file_callback(OBS_STATUS_OK, "Mock Success", 0, NULL, callback_data);
    }
}

void initialize_break_point_lock() {}
void deinitialize_break_point_lock() {}

