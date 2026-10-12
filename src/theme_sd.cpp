#include "theme_sd.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include "sdcard.h"
#else
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#endif

namespace theme_sd {

#ifdef ARDUINO
// Created on first use rather than at init, because the first caller may be either task and
// there is no ordering to rely on.
static SemaphoreHandle_t s_lock = nullptr;
static SemaphoreHandle_t lock_handle() {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    return s_lock;
}
void lock()   { if (SemaphoreHandle_t h = lock_handle()) xSemaphoreTake(h, portMAX_DELAY); }
void unlock() { if (s_lock) xSemaphoreGive(s_lock); }

// Releases the card lock however the function leaves, which is six different ways. Taking it
// by hand and unlocking at each return is how one of those six ends up holding it forever.
struct Held {
    Held()  { lock(); }
    ~Held() { unlock(); }
};

uint8_t *read_whole(const char *path, size_t &outLen, size_t maxBytes) {
    outLen = 0;
    if (!sdcard::mounted()) return nullptr;
    Held held;
    File f = SD.open(path, "r");
    if (!f || f.isDirectory()) { if (f) f.close(); return nullptr; }
    const size_t sz = f.size();
    if (sz == 0 || sz > maxBytes) { f.close(); return nullptr; }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { f.close(); return nullptr; }
    // Timed separately from the PNG decode that follows it. The card sits on a 20 MHz SPI
    // bus (the board does not route 4-bit SD_MMC), so "read the file" and "unpack the file"
    // are different costs with different cures: a slow read is only fixed by moving the
    // asset off the card, a slow decode by pre-baking the pixels. Guessing which one
    // dominates is how the wrong fix gets built.
    const uint32_t t0 = millis();
    const size_t got = f.read(buf, sz);
    const uint32_t readMs = millis() - t0;
    f.close();
    if (got != sz) { heap_caps_free(buf); return nullptr; }
    Serial.printf("[theme_sd] %s: read %u KB in %u ms (%u KB/s)\n",
                  path, (unsigned)(sz / 1024), (unsigned)readMs,
                  (unsigned)(readMs ? (size_t)(sz / 1024) * 1000 / readMs : 0));
    outLen = sz;
    return buf;
}
void free(uint8_t *buf) { if (buf) heap_caps_free(buf); }
bool write_whole(const char *path, const uint8_t *data, size_t len) {
    if (!sdcard::mounted()) return false;
    Held held;
    File f = SD.open(path, "w");
    if (!f) return false;
    const size_t put = f.write(data, len);
    f.close();
    return put == len;
}

bool exists(const char *path) {
    if (!sdcard::mounted()) return false;
    Held held;
    return SD.exists(path);
}

bool remove(const char *path) {
    if (!sdcard::mounted()) return false;
    Held held;
    return !SD.exists(path) || SD.remove(path);
}

bool make_dir(const char *path) {
    if (!sdcard::mounted()) return false;
    Held held;
    return SD.exists(path) || SD.mkdir(path);
}
#else
namespace {
constexpr const char *SIM_SD_ROOT = "sim/sdcard";   // same stand-in root as roads_sd
}
uint8_t *read_whole(const char *path, size_t &outLen, size_t maxBytes) {
    outLen = 0;
    const std::string full = std::string(SIM_SD_ROOT) + path;
    FILE *f = fopen(full.c_str(), "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > maxBytes) { fclose(f); return nullptr; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return nullptr; }
    const size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { ::free(buf); return nullptr; }
    outLen = (size_t)sz;
    return buf;
}
void free(uint8_t *buf) { if (buf) ::free(buf); }
bool write_whole(const char *path, const uint8_t *data, size_t len) {
    const std::string full = std::string(SIM_SD_ROOT) + path;
    FILE *f = fopen(full.c_str(), "wb");
    if (!f) return false;
    const size_t put = fwrite(data, 1, len, f);
    fclose(f);
    return put == len;
}

bool exists(const char *path) {
    struct stat st;
    return ::stat((std::string(SIM_SD_ROOT) + path).c_str(), &st) == 0;
}

bool remove(const char *path) {
    const std::string full = std::string(SIM_SD_ROOT) + path;
    ::remove(full.c_str());
    struct stat st;
    return ::stat(full.c_str(), &st) != 0;
}

bool make_dir(const char *path) {
    const std::string full = std::string(SIM_SD_ROOT) + path;
    ::mkdir(full.c_str(), 0755);
    struct stat st;
    return ::stat(full.c_str(), &st) == 0;
}
#endif

} // namespace theme_sd
