// JSON-RPC 2.0 request dispatch (see PROTOCOL.md).
#pragma once

#include <vector>

#include "cJSON.h"

namespace rpc {

enum ErrorCode : int {
    kParseError = -32700,
    kInvalidRequest = -32600,
    kMethodNotFound = -32601,
    kInvalidParams = -32602,
    kInternalError = -32603,
    kHardwareError = -32000,
    kNotAvailable = -32001,
    kServoUnavailable = -32002,
    kBusy = -32003,
};

struct Error {
    int code = 0;
    const char* message = nullptr;
};

// params: request params (nullptr if absent).
// Return a result object (ownership passes to the caller), or nullptr and fill err.
using Handler = cJSON* (*)(const cJSON* params, Error* err);

struct Method {
    const char* name;
    Handler handler;
    // Called after the response has been sent (e.g. reboot). May be nullptr.
    void (*after_response)();
};

void registerMethods(const Method* methods, size_t count);
const std::vector<Method>& methods();

// Handles one request line and sends the response, if any.
void handleLine(const char* line);
void sendError(const cJSON* id, int code, const char* message);

// Reads request lines forever.
void serve();

}  // namespace rpc
