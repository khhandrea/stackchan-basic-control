#include "notify.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "link.h"

namespace notify {
namespace {

std::atomic<uint32_t> event_seq{0};

int64_t uptimeMs() {
    return esp_timer_get_time() / 1000;
}

// ---- notify.log ----
// The log path avoids the heap and keeps buffers off the caller's stack,
// because ESP_LOG may be called from tasks with small stacks.

constexpr size_t kLogTextSize = 512;
constexpr size_t kLogLineSize = 2 * kLogTextSize + 96;

SemaphoreHandle_t log_mutex = nullptr;
char log_text[kLogTextSize];
char log_line[kLogLineSize];

// Appends s[0..len) as a JSON string body. Returns the new position, or cap on overflow.
size_t appendEscaped(char* out, size_t pos, size_t cap, const char* s, size_t len) {
    for (size_t i = 0; i < len && pos < cap; i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        const char* esc = nullptr;
        char hex[7];
        switch (c) {
            case '"': esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            default:
                if (c < 0x20) {
                    snprintf(hex, sizeof(hex), "\\u%04x", c);
                    esc = hex;
                }
        }
        if (esc != nullptr) {
            size_t n = strlen(esc);
            if (pos + n >= cap) {
                return cap;
            }
            memcpy(out + pos, esc, n);
            pos += n;
        } else {
            out[pos++] = static_cast<char>(c);
        }
    }
    return pos;
}

// Splits an ESP_LOG line "L (ts) tag: msg" into level, tag and msg.
void parseLogLine(const char* text, char* level, const char** tag, size_t* tag_len, const char** msg) {
    *level = 'I';
    *tag = "";
    *tag_len = 0;
    *msg = text;
    if (strchr("EWIDV", text[0]) == nullptr || text[0] == '\0' || text[1] != ' ' || text[2] != '(') {
        return;
    }
    const char* ts_end = strstr(text, ") ");
    if (ts_end == nullptr) {
        return;
    }
    const char* tag_start = ts_end + 2;
    const char* tag_end = strstr(tag_start, ": ");
    if (tag_end == nullptr) {
        return;
    }
    *level = text[0];
    *tag = tag_start;
    *tag_len = static_cast<size_t>(tag_end - tag_start);
    *msg = tag_end + 2;
}

int logVprintf(const char* fmt, va_list args) {
    if (log_mutex == nullptr || xSemaphoreGetMutexHolder(log_mutex) == xTaskGetCurrentTaskHandle()) {
        return 0;  // not ready, or re-entered from inside the hook
    }
    xSemaphoreTake(log_mutex, portMAX_DELAY);

    int n = vsnprintf(log_text, sizeof(log_text), fmt, args);
    size_t len = strnlen(log_text, sizeof(log_text));
    while (len > 0 && (log_text[len - 1] == '\n' || log_text[len - 1] == '\r')) {
        log_text[--len] = '\0';
    }

    if (len > 0) {
        char level;
        const char* tag;
        size_t tag_len;
        const char* msg;
        parseLogLine(log_text, &level, &tag, &tag_len, &msg);

        const size_t cap = sizeof(log_line) - 4;  // room for closing "}}
        size_t pos = static_cast<size_t>(
            snprintf(log_line, cap, R"({"method":"notify.log","params":{"level":"%c","tag":")", level));
        pos = appendEscaped(log_line, pos, cap, tag, tag_len);
        if (pos + 9 < cap) {
            memcpy(log_line + pos, R"(","msg":")", 9);
            pos += 9;
        }
        pos = appendEscaped(log_line, pos, cap, msg, strlen(msg));
        memcpy(log_line + pos, "\"}}", 3);
        pos += 3;
        hostlink::send(log_line, pos);
    }

    xSemaphoreGive(log_mutex);
    return n;
}

}  // namespace

void sendMessage(cJSON* msg) {
    char* text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (text == nullptr) {
        return;
    }
    hostlink::send(text, strlen(text));
    cJSON_free(text);
}

void event(const char* name, cJSON* fields) {
    cJSON* params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "seq", event_seq.fetch_add(1));
    cJSON_AddNumberToObject(params, "ts_ms", static_cast<double>(uptimeMs()));
    cJSON_AddStringToObject(params, "name", name);
    if (fields != nullptr) {
        while (cJSON* item = fields->child) {
            cJSON_DetachItemViaPointer(fields, item);
            cJSON_AddItemToObject(params, item->string, item);
        }
        cJSON_Delete(fields);
    }

    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "method", "notify.event");
    cJSON_AddItemToObject(msg, "params", params);
    sendMessage(msg);
}

void installLogHook() {
    log_mutex = xSemaphoreCreateMutex();
    esp_log_set_vprintf(logVprintf);
}

}  // namespace notify
