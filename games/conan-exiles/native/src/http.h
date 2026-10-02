// Minimal blocking HTTP/1.1 client for the bridge's loopback /mod API. Poller thread only.
#pragma once

#include <string>

#include "proto.h"

struct HttpResult {
    bool ok = false;  // a complete response was received
    int status = 0;
    std::string body;
    std::string error;
};

// One request per connection (Connection: close). `body` empty = no request body.
HttpResult HttpRequest(const HttpUrl& url, const char* method, const std::string& path, const std::string& body,
                       int timeoutMs);
