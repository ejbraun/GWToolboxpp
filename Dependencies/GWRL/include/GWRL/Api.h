#pragma once
#include <stdint.h>

#if defined(_WIN32)
#define GWRL_CALL __cdecl
#else
#define GWRL_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t GwrlRegistration;
typedef uint64_t GwrlRequest;
typedef struct GwrlBytes { const char* data; uint32_t size; } GwrlBytes;

typedef enum GwrlResult {
    GWRL_QUEUED = 0, GWRL_INVALID_ARGUMENT, GWRL_DUPLICATE_ROUTE,
    GWRL_DISCONNECTED, GWRL_NOT_NEGOTIATED, GWRL_CLOSING,
    GWRL_OVERSIZED, GWRL_QUEUE_FULL, GWRL_EXPIRED, GWRL_INTERNAL_ERROR
} GwrlResult;
typedef enum GwrlKind { GWRL_REQUEST = 1, GWRL_RESPONSE = 2, GWRL_EVENT = 3 } GwrlKind;
typedef enum GwrlAvailability { GWRL_UNAVAILABLE = 0, GWRL_AVAILABLE = 1, GWRL_OFFLINE = 2 } GwrlAvailability;

typedef struct GwrlIncoming {
    uint32_t size;
    GwrlKind kind;
    GwrlRequest request;
    uint32_t route_version;
    GwrlBytes type;
    GwrlBytes request_id;
    GwrlBytes payload;
    GwrlBytes code;
    GwrlBytes detail;
} GwrlIncoming;

typedef struct GwrlRouteConfig {
    uint32_t size;
    GwrlBytes recipient;
    const uint32_t* versions;
    uint32_t version_count;
    uintptr_t owner_module;
    void* user;
    void (GWRL_CALL *on_message)(void* user, const GwrlIncoming* message);
    void (GWRL_CALL *on_availability)(void* user, GwrlAvailability availability, uint32_t route_version);
} GwrlRouteConfig;

typedef struct GwrlApi {
    uint32_t size;
    uint32_t abi;
    void* context;
    GwrlResult (GWRL_CALL *register_route)(void*, const GwrlRouteConfig*, GwrlRegistration*);
    GwrlResult (GWRL_CALL *send_request)(void*, GwrlRegistration, GwrlBytes type, GwrlBytes payload, GwrlRequest*);
    GwrlResult (GWRL_CALL *emit_event)(void*, GwrlRegistration, GwrlBytes type, GwrlBytes payload);
    GwrlResult (GWRL_CALL *reply)(void*, GwrlRequest, GwrlBytes type, GwrlBytes payload, GwrlBytes code, GwrlBytes detail);
    GwrlResult (GWRL_CALL *release_request)(void*, GwrlRequest);
    GwrlResult (GWRL_CALL *begin_unregister)(void*, GwrlRegistration);
    uint32_t (GWRL_CALL *is_drained)(void*, GwrlRegistration);
} GwrlApi;

typedef const GwrlApi* (GWRL_CALL *GwrlGetApiFn)(uint32_t abi);

#ifdef __cplusplus
}
#endif
