#include "adsb_source.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

#include "config.h"
#include "core/aircraftdb.h"
#include "core/geo.h"

namespace adsb {
namespace {

// Every provider rejects a larger radius than this.
constexpr uint16_t kMaxRadiusNm = 250;

uint32_t gLastAttemptMs = 0;
uint32_t gLastSuccessMs = 0;
FetchStats gLastStats;

AdsbProvider gProvider = ADSB_PROVIDER_DEFAULT;

uint32_t httpTimeoutMs(AdsbProvider p) {
    return (p == AdsbProvider::LOCAL) ? ADSB_LOCAL_HTTP_TIMEOUT_MS
                                      : ADSB_REMOTE_HTTP_TIMEOUT_MS;
}

// ArduinoJson allocates the parse tree in one arena. A busy 250 nm query can
// run to a couple of hundred kilobytes, which would starve the Wi-Fi stack if
// it came from internal RAM -- so put it in the 8 MB PSRAM instead.
struct PsramAllocator : ArduinoJson::Allocator {
    void *allocate(size_t size) override {
        void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        return p ? p : malloc(size);  // fall back if PSRAM is absent
    }
    void deallocate(void *pointer) override { free(pointer); }
    void *reallocate(void *ptr, size_t new_size) override {
        void *p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
        return p ? p : realloc(ptr, new_size);
    }
};

// Taken as a parameter rather than read from gProvider: the fetch runs on
// another task, and the 'p' key can land in the middle of one. Whichever feed
// the request was aimed at is the one whose response shape it has to parse.
const char *arrayKey(AdsbProvider p) {
    // readsb and adsb.fi both name it "aircraft"; adsb.lol and airplanes.live
    // shorten it to "ac".
    return (p == AdsbProvider::ADSB_FI || p == AdsbProvider::LOCAL) ? "aircraft"
                                                                   : "ac";
}

void buildUrl(char *out, size_t len, AdsbProvider p, double lat, double lon,
              int radiusNm) {
    switch (p) {
        case AdsbProvider::LOCAL:
            // A receiver's aircraft.json is a fixed path holding everything it
            // currently hears -- there is nothing to parameterise, and the
            // radius is applied client-side after parsing instead.
            (void)lat;
            (void)lon;
            (void)radiusNm;
            snprintf(out, len, "%s", ADSB_LOCAL_URL);
            break;
        case AdsbProvider::ADSB_FI:
            snprintf(out, len,
                     "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d",
                     lat, lon, radiusNm);
            break;
        case AdsbProvider::AIRPLANES_LIVE:
            snprintf(out, len, "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
                     lat, lon, radiusNm);
            break;
        case AdsbProvider::ADSB_LOL:
        default:
            snprintf(out, len, "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", lat,
                     lon, radiusNm);
            break;
    }
}

// Only these fields survive parsing; everything else in the response is
// discarded as it streams past, which keeps the arena an order of magnitude
// smaller than the raw JSON.
void buildFilter(JsonDocument &filter, AdsbProvider p) {
    JsonObject item = filter[arrayKey(p)].add<JsonObject>();
    item["hex"] = true;
    item["flight"] = true;
    item["r"] = true;
    item["t"] = true;
    item["lat"] = true;
    item["lon"] = true;
    item["alt_baro"] = true;
    item["gs"] = true;
    item["track"] = true;
    item["baro_rate"] = true;
    item["geom_rate"] = true;
    item["squawk"] = true;
    item["emergency"] = true;
    item["seen_pos"] = true;
}

// ArduinoJson pulls its input one byte at a time, and it does so through
// Stream::readBytes() -- which per byte calls millis(), available() and
// read(). Against a TLS socket each of those is a trip into mbedtls, and a
// busy query is a couple of hundred kilobytes, so the parse was spending most
// of its time in call overhead rather than on JSON.
//
// The source is held as a Client rather than a Stream deliberately:
// Stream::readBytes() is itself a per-byte loop, so buffering through it would
// have changed nothing. Client::read(buf, n) is the one entry point that hands
// a whole TLS record over in a single mbedtls_ssl_read.
class BufferedStream : public Stream {
   public:
    BufferedStream(Client &source, uint8_t *buffer, size_t capacity,
                   uint32_t timeoutMs)
        : source_(source), buf_(buffer), cap_(capacity), timeoutMs_(timeoutMs) {}

    int available() override {
        return static_cast<int>(len_ - pos_) + source_.available();
    }

    int read() override {
        if (pos_ >= len_ && !fill()) return -1;
        return buf_[pos_++];
    }

    int peek() override {
        if (pos_ >= len_ && !fill()) return -1;
        return buf_[pos_];
    }

    size_t readBytes(char *dst, size_t length) override {
        size_t done = 0;
        while (done < length) {
            if (pos_ >= len_ && !fill()) break;
            size_t take = len_ - pos_;
            if (take > length - done) take = length - done;
            memcpy(dst + done, buf_ + pos_, take);
            pos_ += take;
            done += take;
        }
        return done;
    }

    size_t write(uint8_t) override { return 0; }  // read-only
    void flush() override {}

   private:
    // Only ever asks for what has already been decrypted, so the read returns
    // straight away; the wait for more is done explicitly, yielding so the
    // Wi-Fi task can actually deliver it.
    bool fill() {
        pos_ = 0;
        len_ = 0;
        const uint32_t deadline = millis() + timeoutMs_;
        for (;;) {
            const int pending = source_.available();
            if (pending > 0) {
                size_t want = static_cast<size_t>(pending);
                if (want > cap_) want = cap_;
                const int got = source_.read(buf_, want);
                if (got <= 0) return false;  // socket went away under us
                len_ = static_cast<size_t>(got);
                return true;
            }
            // Checked only once the buffered bytes are drained, so a body that
            // arrives complete and is then closed still parses.
            if (!source_.connected()) return false;
            if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
            delay(1);
        }
    }

    Client &source_;
    uint8_t *buf_;
    size_t cap_;
    uint32_t timeoutMs_;
    size_t pos_ = 0;
    size_t len_ = 0;
};

void trimTrailingSpace(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

void copyField(char *dst, size_t dstLen, JsonVariantConst src) {
    dst[0] = 0;
    const char *v = src.as<const char *>();
    if (!v) return;
    strncpy(dst, v, dstLen - 1);
    dst[dstLen - 1] = 0;
    trimTrailingSpace(dst);
}

bool decodeAircraft(JsonObjectConst src, Aircraft *out) {
    *out = Aircraft{};

    copyField(out->hex, sizeof(out->hex), src["hex"]);
    if (!out->hex[0]) return false;

    copyField(out->flight, sizeof(out->flight), src["flight"]);
    copyField(out->reg, sizeof(out->reg), src["r"]);
    copyField(out->type, sizeof(out->type), src["t"]);
    copyField(out->squawk, sizeof(out->squawk), src["squawk"]);

    JsonVariantConst lat = src["lat"];
    JsonVariantConst lon = src["lon"];
    if (!lat.isNull() && !lon.isNull()) {
        out->lat = lat.as<double>();
        out->lon = lon.as<double>();
        out->hasPosition = true;
        out->seenPosSec = src["seen_pos"] | 0.0f;
    }

    // alt_baro is a number in feet, or the string "ground".
    JsonVariantConst alt = src["alt_baro"];
    if (alt.is<const char *>()) {
        out->altitudeFt = Aircraft::kAltitudeGround;
    } else if (!alt.isNull()) {
        out->altitudeFt = alt.as<int32_t>();
    } else {
        out->altitudeFt = Aircraft::kAltitudeUnknown;
    }

    JsonVariantConst track = src["track"];
    if (!track.isNull()) {
        out->trackDeg = track.as<float>();
        out->hasTrack = true;
    }
    out->groundSpeedKt = src["gs"] | 0.0f;

    // Prefer the barometric rate; fall back to the geometric one.
    JsonVariantConst rate = src["baro_rate"];
    if (rate.isNull()) rate = src["geom_rate"];
    out->verticalRateFpm = static_cast<int16_t>(rate | 0);

    const char *emergency = src["emergency"].as<const char *>();
    out->emergency = emergency && emergency[0] && strcmp(emergency, "none") != 0;

    // The emergency field is only there when the aircraft actually broadcasts
    // an emergency/priority status, which plenty never do -- but the Mode-A
    // code is in almost every message. 7500 hijack, 7600 radio failure, 7700
    // general emergency: the three that mean the same thing everywhere in the
    // world, and the reason to look up from the screen.
    if (!out->emergency && out->squawk[0]) {
        out->emergency = strcmp(out->squawk, "7500") == 0 ||
                         strcmp(out->squawk, "7600") == 0 ||
                         strcmp(out->squawk, "7700") == 0;
    }

    return true;
}

}  // namespace

AdsbProvider provider() { return gProvider; }

bool isLocal() { return gProvider == AdsbProvider::LOCAL; }

void setProvider(AdsbProvider p) {
    if (p == gProvider) return;
    gProvider = p;
    // The next poll is against a different endpoint with a different idea of
    // what is nearby, so nothing about the last one is worth reporting.
    gLastStats = FetchStats{};
    log_i("adsb: feed switched to %s", providerName());
}

void toggleProvider() {
    setProvider(isLocal() ? ADSB_PROVIDER_REMOTE : AdsbProvider::LOCAL);
}

uint32_t pollIntervalMs() {
    return isLocal() ? ADSB_LOCAL_POLL_INTERVAL_MS : ADSB_REMOTE_POLL_INTERVAL_MS;
}

uint32_t minIntervalMs() {
    return isLocal() ? ADSB_LOCAL_MIN_INTERVAL_MS : ADSB_REMOTE_MIN_INTERVAL_MS;
}

const char *providerName() {
    switch (gProvider) {
        case AdsbProvider::LOCAL: return "local rx";
        case AdsbProvider::ADSB_FI: return "adsb.fi";
        case AdsbProvider::AIRPLANES_LIVE: return "airplanes.live";
        case AdsbProvider::ADSB_LOL:
        default: return "adsb.lol";
    }
}

namespace {

// Everything from the GET to the merge, once a transport has been chosen.
// Split out so the plain-HTTP and TLS paths can share it without either one
// paying to construct the other's client.
FetchStats runFetch(WiFiClient &client, const char *url, AdsbProvider p,
                    Tracker &tracker, double lat, double lon, uint16_t radiusNm,
                    uint32_t started) {
    FetchStats stats;

    HTTPClient http;
    http.setTimeout(httpTimeoutMs(p));
    http.setConnectTimeout(httpTimeoutMs(p));
    // HTTP/1.0 asks the server not to chunk the body, which lets ArduinoJson
    // read straight off the socket.
    http.useHTTP10(true);

    if (!http.begin(client, url)) {
        stats.result = Result::HttpError;
        return stats;
    }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json");
    // The lighttpd that fronts tar1090 will gzip JSON if a client says it can
    // take it, and we have no decompressor -- so say we cannot.
    http.addHeader("Accept-Encoding", "identity");

    stats.httpStatus = http.GET();
    if (stats.httpStatus != HTTP_CODE_OK) {
        log_w("adsb: HTTP %d from %s", stats.httpStatus, providerName());
        stats.result = Result::HttpError;
        http.end();
        return stats;
    }
    const int contentLength = http.getSize();
    stats.bytes = contentLength > 0 ? static_cast<uint32_t>(contentLength) : 0;

    PsramAllocator allocator;
    JsonDocument filter(&allocator);
    buildFilter(filter, p);

    JsonDocument doc(&allocator);
    // Static rather than stack: this runs on the fetch task, whose stack is
    // already carrying an mbedtls session.
    static uint8_t rxBuffer[2048];
    BufferedStream buffered(http.getStream(), rxBuffer, sizeof(rxBuffer),
                            httpTimeoutMs(p));
    const DeserializationError err = deserializeJson(
        doc, buffered, DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        log_w("adsb: parse failed: %s", err.c_str());
        stats.result = Result::ParseError;
        return stats;
    }

    JsonArrayConst list = doc[arrayKey(p)].as<JsonArrayConst>();
    if (list.isNull()) {
        log_w("adsb: response carried no aircraft array");
        stats.result = Result::ParseError;
        return stats;
    }

    const uint32_t now = millis();
    const float filterRadiusNm = static_cast<float>(radiusNm);
    Aircraft parsed;
    for (JsonObjectConst item : list) {
        ++stats.received;
        if (!decodeAircraft(item, &parsed)) continue;

        // A local aircraft.json is unfiltered: it carries every target the
        // receiver hears, which at altitude reaches far past anything the plot
        // will show. Applying the radius here stops a distant one taking a
        // MAX_AIRCRAFT slot a nearby one needs. Targets with no position are
        // kept regardless -- they cannot be plotted, but they still belong in
        // the list, and that is what the aggregators return too.
        if (p == AdsbProvider::LOCAL && parsed.hasPosition &&
            geo::distanceNm(lat, lon, parsed.lat, parsed.lon) > filterRadiusNm) {
            ++stats.filtered;
            continue;
        }

        // A local aircraft.json carries no "r" or "t" at all, and even an
        // aggregator leaves them out for aircraft it has not identified. The
        // flashed table fills what it can; anything it misses stays empty and
        // may still arrive from a later poll, so this never overwrites.
        if (!parsed.reg[0] || !parsed.type[0]) {
            char reg[sizeof(parsed.reg)] = {0};
            char type[sizeof(parsed.type)] = {0};
            if (aircraftdb::lookup(parsed.hex, reg, sizeof(reg), type,
                                   sizeof(type))) {
                if (!parsed.reg[0]) {
                    strncpy(parsed.reg, reg, sizeof(parsed.reg) - 1);
                }
                if (!parsed.type[0]) {
                    strncpy(parsed.type, type, sizeof(parsed.type) - 1);
                }
            }
        }

        if (!tracker.upsert(parsed, now)) {
            // Full, and this one is no nearer than anything already held. Skip
            // it and keep going: the rest of the snapshot is mostly aircraft
            // we are already tracking, and abandoning it here would stop
            // refreshing them until they aged out -- a plot that freezes in
            // patches while the feed is perfectly healthy.
            ++stats.dropped;
            continue;
        }
        ++stats.stored;
    }
    if (stats.dropped > 0) {
        log_w("adsb: store full at %d aircraft; %u further targets refused",
              MAX_AIRCRAFT, static_cast<unsigned>(stats.dropped));
    }

    stats.result = Result::Ok;
    stats.durationMs = millis() - started;
    return stats;
}

FetchStats fetchBlocking(AdsbProvider p, double lat, double lon,
                         uint16_t radiusNm, Tracker &into) {
    const uint32_t started = millis();

    char url[160];
    buildUrl(url, sizeof(url), p, lat, lon, radiusNm);

    // Transport. A receiver on the LAN is plain HTTP: there is nothing secret
    // in the request, no certificate anybody could usefully check, and the
    // handshake we skip is the most expensive part of a poll -- it dominated
    // the 1.3-1.5 s an aggregator fetch used to take, and it churned tens of
    // kilobytes of mbedtls heap every time. Only the branch actually taken
    // constructs a client, so a local poll never touches mbedtls -- but both
    // branches are compiled now that the choice is a runtime one, which is
    // what the TLS stack costs us in flash.
    if (p == AdsbProvider::LOCAL) {
        WiFiClient client;
        return runFetch(client, url, p, into, lat, lon, radiusNm, started);
    }

    WiFiClientSecure client;
    // These are public, read-only, unauthenticated feeds and the board has no
    // way to refresh a pinned root as the providers rotate certificates. We
    // accept any certificate rather than ship a root that silently expires;
    // nothing secret is sent and the payload is sanity-checked after parsing.
    client.setInsecure();
    client.setTimeout(httpTimeoutMs(p) / 1000);
    return runFetch(client, url, p, into, lat, lon, radiusNm, started);
}

// ---------------------------------------------------------- the handoff ----
//
// Idle -> Running is written by the loop task and read by the fetch task;
// Running -> Done goes the other way. Acquire/release rather than plain
// volatile so the compiler cannot hoist the buffer accesses either side of it
// across the state change -- the whole safety of the arrangement is that
// exactly one task owns the staging store at a time, and the state word is
// what says which.
enum class State : uint8_t { Idle, Running, Done };
std::atomic<State> gState{State::Idle};

struct Request {
    double lat = 0.0;
    double lon = 0.0;
    uint16_t radiusNm = 0;
    AdsbProvider provider = ADSB_PROVIDER_DEFAULT;
};
Request gRequest;          // written while Idle, read while Running
FetchStats gResultStats;   // written while Running, read while Done
Tracker gStaging;          // same ownership rule as the two above

TaskHandle_t gTask = nullptr;
SemaphoreHandle_t gWake = nullptr;

// Deep enough for an mbedtls handshake, which is comfortably the deepest thing
// this firmware does. The JSON arena is in PSRAM and the receive buffer is
// static, so almost all of this is TLS. Watch it on the diagnostics page.
constexpr uint32_t kTaskStackBytes = 12288;
// Same priority as the Arduino loop task, and the same core it runs on --
// which is the whole point: this is exactly where the fetch already ran, so
// nothing about how it is scheduled changes. Only the blocking does. The two
// tasks share the core by round-robin while both are runnable, and the fetch
// spends nearly all of its time not runnable at all, parked on a socket.
//
// The other core is tempting and wrong. Its idle task is the one the task
// watchdog watches -- the Arduino core exempts core 1 precisely because
// loop() hogs it -- so a parse that ran long there would trip a reset that
// the same parse on this core does not. It is also where the Wi-Fi stack
// lives, at priorities far above this, feeding the very socket we would be
// waiting on.
constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = ARDUINO_RUNNING_CORE;

void fetchTask(void *) {
    for (;;) {
        xSemaphoreTake(gWake, portMAX_DELAY);

        gStaging.clear();
        // The staging store applies the same nearest-first eviction the live
        // one does, so what crosses back is already the nearest MAX_AIRCRAFT
        // of the snapshot rather than the first MAX_AIRCRAFT of it.
        gStaging.setCentre(gRequest.lat, gRequest.lon);

        gResultStats = fetchBlocking(gRequest.provider, gRequest.lat,
                                     gRequest.lon, gRequest.radiusNm, gStaging);

        gState.store(State::Done, std::memory_order_release);
    }
}

}  // namespace

void begin() {
    if (gTask) return;
    gWake = xSemaphoreCreateBinary();
    if (!gWake) {
        log_e("adsb: no semaphore for the fetch task");
        return;
    }
    if (xTaskCreatePinnedToCore(fetchTask, "adsb", kTaskStackBytes, nullptr,
                                kTaskPriority, &gTask, kTaskCore) != pdPASS) {
        gTask = nullptr;
        log_e("adsb: fetch task would not start");
    }
}

bool busy() { return gState.load(std::memory_order_acquire) != State::Idle; }

bool ready() { return gState.load(std::memory_order_acquire) == State::Done; }

bool request(double lat, double lon, uint16_t radiusNm) {
    if (!gTask) return false;
    if (gState.load(std::memory_order_acquire) != State::Idle) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    const uint32_t now = millis();
    if (gLastAttemptMs != 0 && now - gLastAttemptMs < minIntervalMs()) {
        return false;
    }
    gLastAttemptMs = now;

    if (radiusNm < 1) radiusNm = 1;
    if (radiusNm > kMaxRadiusNm) radiusNm = kMaxRadiusNm;

    gRequest.lat = lat;
    gRequest.lon = lon;
    gRequest.radiusNm = radiusNm;
    // Latched here rather than read on the task: the 'p' key can land while a
    // fetch is in flight, and a response parsed against the other provider's
    // array name would come back empty for no visible reason.
    gRequest.provider = gProvider;

    gState.store(State::Running, std::memory_order_release);
    xSemaphoreGive(gWake);
    return true;
}

bool collect(Tracker &tracker, FetchStats *stats) {
    if (gState.load(std::memory_order_acquire) != State::Done) return false;

    FetchStats result = gResultStats;

    // Bounded by MAX_AIRCRAFT and no strings are parsed, so this is a couple
    // of milliseconds on the loop task -- against the second and a half of
    // socket, handshake and JSON the fetch task has just spent on the other
    // core. The tracker is only ever touched from here, which is what keeps
    // the rest of the firmware free of locks.
    const uint32_t now = millis();
    for (size_t i = 0; i < gStaging.count(); ++i) {
        if (!tracker.upsert(gStaging.at(i), now)) ++result.dropped;
    }

    if (result.result == Result::Ok) {
        gLastSuccessMs = millis();
        log_i("adsb: %u aircraft in %ums (%u filtered)", result.stored,
              result.durationMs, result.filtered);
    }
    gLastStats = result;
    if (stats) *stats = result;

    gState.store(State::Idle, std::memory_order_release);
    return true;
}

uint32_t lastSuccessMs() { return gLastSuccessMs; }
const FetchStats &lastStats() { return gLastStats; }

uint32_t taskHeadroomBytes() {
    if (!gTask) return 0;
    return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(gTask));
}

}  // namespace adsb
