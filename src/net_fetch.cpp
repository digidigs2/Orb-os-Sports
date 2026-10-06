// Shared streamed-download helper (see net_fetch.h). Mirrors the proven pattern in
// photo_client.cpp: Content-Length -> stream into PSRAM; chunked -> getString decode.
#include "net_fetch.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <memory>

bool net_fetch_psram(const char *url, const char *userAgent,
                     uint8_t **out, size_t *outLen, size_t maxLen,
                     int connectTimeoutMs, int totalTimeoutMs) {
    *out = nullptr; *outLen = 0;
    if (WiFi.status() != WL_CONNECTED) return false;

    // The transport follows the URL, which it did not used to. This built a
    // WiFiClientSecure for every call, and a secure client handshakes on connect whatever
    // the scheme says, so an http:// URL still did TLS and still failed -32512 on a board
    // that cannot raise the two contiguous ~16 KB internal blocks a handshake needs.
    //
    // Every caller of this helper was therefore dead for the same reason and it looked
    // like several separate faults: the weather radar's tiles, the photo feed and the
    // cloud imagery were each written off as "that service forces HTTPS" when the force
    // was here. An https:// URL still gets a secure client and will still fail, which is
    // honest, and the log below says which one it was.
    //
    // ...and the secure client is only BUILT when the URL asks for one, which is the half of
    // that reasoning this originally missed. Declaring it here cost internal RAM on every
    // call, plain downloads included, which on this board is all of them: the constructor
    // does `new sslclient_context` and ssl_init() straight away, standing up an mbedTLS ssl
    // context, its config and a DRBG before anybody has asked to connect. A couple of KB,
    // taken and given back per fetch, out of the one heap the weather tiles and the cloud
    // imagery are already competing for, and whose largest free block has been measured in
    // single KB. Not what kills TLS, which is the 16 KB handshake buffers later, just a tax
    // nobody was getting anything for.
    //
    // Found by Techtobi83, in their fork, 2026-10.
    const bool secure = !strncmp(url, "https://", 8);
    WiFiClient plain;
    std::unique_ptr<WiFiClientSecure> tls(secure ? new WiFiClientSecure() : nullptr);
    if (tls) tls->setInsecure();              // hobby device (matches the other clients)
    WiFiClient &cli = tls ? static_cast<WiFiClient &>(*tls) : plain;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(connectTimeoutMs);
    http.setTimeout(totalTimeoutMs);
    if (!http.begin(cli, url)) return false;
    if (userAgent) http.setUserAgent(userAgent);

    const int code = http.GET();
    if (code != 200) {
        Serial.printf("[net] HTTP %d over %s: %s\n", code, secure ? "TLS" : "plain", url);
        http.end(); return false;
    }

    const int len = http.getSize();           // >0 = Content-Length; -1 = chunked/unknown
    uint8_t *buf = nullptr;
    size_t got = 0;

    if (len > 0) {
        // Known length: stream the body straight into a PSRAM buffer.
        const size_t cap = ((size_t)len <= maxLen) ? (size_t)len : maxLen;
        buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!buf) { http.end(); return false; }
        WiFiClient *stream = http.getStreamPtr();
        uint32_t last = millis();
        while (got < cap && (millis() - last) < (uint32_t)totalTimeoutMs) {
            const size_t avail = stream->available();
            if (avail) {
                const size_t want = (cap - got < avail) ? (cap - got) : avail;
                const int r = stream->readBytes(buf + got, want);
                if (r > 0) { got += r; last = millis(); }
            } else if (!http.connected()) {
                break;
            } else {
                delay(5);
            }
        }
    } else {
        // Chunked/unknown: getString() performs the chunk decode; only small payloads
        // are expected here (image CDNs send Content-Length), so the String is cheap.
        String body = http.getString();
        got = body.length();
        if (got > maxLen) got = maxLen;
        if (got > 0) {
            buf = (uint8_t *)heap_caps_malloc(got, MALLOC_CAP_SPIRAM);
            if (buf) memcpy(buf, body.c_str(), got);
            else got = 0;
        }
    }
    http.end();
    if (got == 0) {
        // SAY SO. This failed in silence, and "the service is not answering" on the glass was
        // then the only clue, at the times when the service had answered perfectly well and
        // the Orb could not take the reply. A socket's buffers come out of internal heap, so
        // print what there was of it: the difference between a server problem and a memory
        // one is the first thing anybody diagnosing this needs and could not get.
        Serial.printf("[net] no body (len %d): internal heap %u free, largest block %u: %s\n",
                      len, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), url);
        if (buf) heap_caps_free(buf);
        return false;
    }
    *out = buf; *outLen = got;
    return true;
}

#else
// Desktop/native build: no PSRAM, no fragmented-heap concerns — a single libcurl GET
// straight into a malloc'd buffer covers it. The maxLen cap is preserved so a runaway
// response still aborts instead of growing unbounded.
#include <curl/curl.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace {
    struct NetBuf { uint8_t *data; size_t len, cap; };
    size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
        NetBuf *b = (NetBuf *)userdata;
        const size_t n = size * nmemb;
        if (b->len + n > b->cap) return 0;   // over budget -> libcurl aborts the transfer
        memcpy(b->data + b->len, ptr, n);
        b->len += n;
        return n;
    }
}

bool net_fetch_psram(const char *url, const char *userAgent,
                     uint8_t **out, size_t *outLen, size_t maxLen,
                     int connectTimeoutMs, int totalTimeoutMs) {
    *out = nullptr; *outLen = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    NetBuf buf = { (uint8_t *)malloc(maxLen), 0, maxLen };
    if (!buf.data) { curl_easy_cleanup(curl); return false; }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    if (userAgent) curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)connectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)totalTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    const CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || code != 200 || buf.len == 0) {
        fprintf(stderr, "[net] HTTP %ld (curl: %s) %s\n", code, curl_easy_strerror(res), url);
        free(buf.data);
        return false;
    }
    *out = buf.data; *outLen = buf.len;
    return true;
}
#endif
