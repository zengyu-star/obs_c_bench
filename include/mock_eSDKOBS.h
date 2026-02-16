#ifndef MOCK_ESDKOBS_H
#define MOCK_ESDKOBS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define OBS_INIT_WINSOCK 1
#define OBS_INIT_ALL     (OBS_INIT_WINSOCK)

typedef enum { OBS_PROTOCOL_HTTPS = 0, OBS_PROTOCOL_HTTP = 1 } obs_protocol;

typedef enum {
    OBS_STATUS_OK = 0,
    OBS_STATUS_AccessDenied,
    OBS_STATUS_NoSuchBucket,
    OBS_STATUS_NoSuchKey,
    OBS_STATUS_NoSuchUpload,
    OBS_STATUS_BucketAlreadyExists,
    OBS_STATUS_BucketNotEmpty,
    OBS_STATUS_InvalidParameter,
    OBS_STATUS_EntityTooSmall,
    OBS_STATUS_EntityTooLarge,
    OBS_STATUS_MetadataTooLarge, 
    OBS_STATUS_InvalidTargetBucketForLogging,
    OBS_STATUS_InlineDataTooLarge,
    OBS_STATUS_InvalidPart,
    OBS_STATUS_InvalidPartOrder,
    OBS_STATUS_InternalError,
    OBS_STATUS_ServiceUnavailable,
    OBS_STATUS_ConnectionFailed,
    OBS_STATUS_OutOfMemory,
    OBS_STATUS_InitCurlFailed,
    OBS_STATUS_BUTT
} obs_status;

typedef enum { OBS_GM_MODE_CLOSE = 0, OBS_GM_MODE_OPEN = 1 } obs_gm_mode_switch;
typedef enum { OBS_MUTUAL_SSL_CLOSE = 0, OBS_MUTUAL_SSL_OPEN = 1 } obs_mutual_ssl_switch;

typedef struct {
    char *host_name;
    char *bucket_name;
    char *access_key;
    char *secret_access_key;
    obs_protocol protocol;
} obs_bucket_context;

typedef struct {
    int connect_time;
    int max_connected_time;
    bool keep_alive;
    obs_gm_mode_switch gm_mode_switch;   
    long ssl_min_version;                
    long ssl_max_version;                
    obs_mutual_ssl_switch mutual_ssl_switch;
    char* client_cert_path;
    char* client_key_path;
    char* client_key_password;
} obs_http_request_option;

typedef struct {
    obs_bucket_context bucket_options;
    obs_http_request_option request_options;
} obs_options;

typedef struct { char *key; } obs_object_info;

// [修改] 增加 byte_count 支持 Range
typedef struct { 
    uint64_t start_byte; 
    uint64_t byte_count; 
} obs_get_conditions;

typedef struct { char *content_type; } obs_put_properties;

typedef struct {
    const char *key;
    int64_t last_modified;
    const char *etag;
    uint64_t size;
    const char *owner_id;
    const char *owner_display_name;
    const char *storage_class;
} obs_list_objects_content;

typedef struct {
    char *upload_file;
    uint64_t part_size;
    char *check_point_file;
    int enable_check_point;
    int task_num;
    int *pause_upload_flag;
    obs_put_properties *put_properties;
} obs_upload_file_configuration;

typedef struct {
    int part_num;
} obs_upload_file_part_info;

typedef struct {
    unsigned int part_number;
    char *upload_id;
} obs_upload_part_info;

typedef struct {
    unsigned int part_number;
    char *etag;
} obs_complete_upload_Info;

typedef struct {
    // dummy
} server_side_encryption_params;

// --- Callbacks ---
typedef struct { 
    const char *etag; 
    uint64_t content_length;
} obs_response_properties;

typedef struct { const char *message; } obs_error_details;

typedef obs_status (obs_response_properties_callback)(const obs_response_properties *properties, void *callback_data);
typedef void (obs_response_complete_callback)(obs_status status, const obs_error_details *error, void *callback_data);
typedef int (obs_put_object_data_callback)(int buffer_size, char *buffer, void *callback_data);
typedef obs_status (obs_get_object_data_callback)(int buffer_size, const char *buffer, void *callback_data);
typedef void (obs_upload_file_callback)(obs_status status, char *result_message, int part_count_return, obs_upload_file_part_info * upload_info_list, void *callback_data);
typedef int (obs_upload_data_callback)(int buffer_size, char *buffer, void *callback_data);
typedef obs_status (obs_complete_multi_part_upload_callback)(const char *location, const char *bucket, const char *key, const char* etag, void *callback_data);
typedef obs_status (obs_list_objects_callback)(int is_truncated, const char *next_marker, 
                                               int contents_count, const obs_list_objects_content *contents, 
                                               int common_prefixes_count, const char **common_prefixes, 
                                               void *callback_data);

typedef struct {
    obs_response_properties_callback *properties_callback;
    obs_response_complete_callback *complete_callback;
} obs_response_handler;

typedef struct {
    obs_response_handler response_handler;
    obs_put_object_data_callback *put_object_data_callback;
} obs_put_object_handler;

typedef struct {
    obs_response_handler response_handler;
    obs_get_object_data_callback *get_object_data_callback;
} obs_get_object_handler;

typedef struct {
    obs_response_handler response_handler;
    obs_upload_file_callback *upload_file_callback;
} obs_upload_file_response_handler;

typedef struct {
    obs_response_handler response_handler;
    obs_upload_data_callback *upload_data_callback;
} obs_upload_handler;

typedef struct {
    obs_response_handler response_handler;
    obs_complete_multi_part_upload_callback *complete_multipart_upload_callback;
} obs_complete_multi_part_upload_handler;

typedef struct {
    obs_response_handler response_handler;
    obs_list_objects_callback *list_Objects_callback;
} obs_list_objects_handler;

typedef struct {
    // dummy
} obs_upload_file_server_callback;

// --- Functions ---
obs_status obs_initialize(int flags);
void obs_deinitialize();
const char* obs_get_status_name(obs_status status);
void init_obs_options(obs_options *options);
void init_put_properties(obs_put_properties *options);
void init_get_properties(obs_get_conditions *options);

void put_object(const obs_options *options, char *key, uint64_t content_length,
                obs_put_properties *put_properties, 
                server_side_encryption_params *encryption_params,
                obs_put_object_handler *handler, void *callback_data);

void get_object(const obs_options *options, obs_object_info *object_info,
                obs_get_conditions *get_conditions, 
                server_side_encryption_params *encryption_params,
                obs_get_object_handler *handler, void *callback_data);

void delete_object(const obs_options *options, obs_object_info *object_info,
                   obs_response_handler *handler, void *callback_data);

void list_bucket_objects(const obs_options *options, const char *prefix, const char *marker, 
                         const char *delimiter, int maxkeys, 
                         obs_list_objects_handler *handler, void *callback_data);

void initiate_multi_part_upload(const obs_options *options, char *key, 
                                int upload_id_return_size, char *upload_id_return,
                                obs_put_properties *put_properties, 
                                server_side_encryption_params *encryption_params,
                                obs_response_handler *handler, void *callback_data);

void upload_part(const obs_options *options, char *key, obs_upload_part_info *part_info,
                 uint64_t content_length, obs_put_properties *put_properties, 
                 server_side_encryption_params *encryption_params,
                 obs_upload_handler *handler, void *callback_data);

void complete_multi_part_upload(const obs_options *options, char *key, char *upload_id, 
                                unsigned int part_count, obs_complete_upload_Info *parts_info,
                                obs_put_properties *put_properties, 
                                obs_complete_multi_part_upload_handler *handler, void *callback_data);

void upload_file(const obs_options *options, char *key, server_side_encryption_params *encryption_params, 
                 obs_upload_file_configuration *upload_file_config, 
                 obs_upload_file_server_callback server_callback, 
                 obs_upload_file_response_handler *handler,
                 void *callback_data);

void initialize_break_point_lock();
void deinitialize_break_point_lock();

#endif

