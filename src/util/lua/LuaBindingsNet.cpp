#include <Arduino.h>
#include <BuildScratch.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <ScratchHeap.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "Logging.h"
#include "WifiCredentialStore.h"
#include "network/HttpDownloader.h"
#include "network/TlsScratch.h"
#include "util/LuaManager.h"
#include "util/lua/LuaBindings.h"

extern "C" {
#include <lauxlib.h>
}

using freeink::SecureHttpClient;
using luabindings::addFunction;
using luabindings::getManager;
using luabindings::getRenderer;

namespace {
constexpr size_t MAX_HTTP_RESPONSE_SIZE = 50000;
constexpr size_t MAX_DOWNLOAD_SIZE = 16 * 1024 * 1024;

enum class WifiState { Idle, Connecting, Connected, Failed };
WifiState wifiState = WifiState::Idle;
size_t credentialIndex = 0;
bool ownsWifi = false;
unsigned long wifiAttemptStarted = 0;
constexpr unsigned long WIFI_ATTEMPT_TIMEOUT_MS = 15000;
void startWifiCredential(size_t index) {
  const auto& credentials = WIFI_STORE.getCredentials();
  if (index >= credentials.size()) {
    wifiState = WifiState::Failed;
    return;
  }
  const auto& credential = credentials[index];
  WiFi.begin(credential.ssid.c_str(), credential.password.empty() ? nullptr : credential.password.c_str());
  wifiAttemptStarted = millis();
  LOG_INF("LUA", "Connecting WiFi: %s", credential.ssid.c_str());
}

// --- Starts connecting to a stored Wi-Fi network (credentials from the reader settings). Poll wifi.status().
// -- @within wifi
int netWifiConnect(lua_State*) {
  WIFI_STORE.loadFromFile();
  const auto& credentials = WIFI_STORE.getCredentials();
  if (credentials.empty()) {
    wifiState = WifiState::Failed;
    return 0;
  }
  WiFi.mode(WIFI_STA);
  credentialIndex = 0;
  ownsWifi = true;
  const auto& lastSsid = WIFI_STORE.getLastConnectedSsid();
  for (size_t i = 0; i < credentials.size(); ++i) {
    if (credentials[i].ssid == lastSsid) {
      credentialIndex = i;
      break;
    }
  }
  wifiState = WifiState::Connecting;
  startWifiCredential(credentialIndex);
  return 0;
}

// --- Returns the Wi-Fi connection state, advancing any in-progress attempt.
// -- @return table status Fields state, ssid, ip and rssi. state is "disconnected", "connecting", "connected" or "failed".
// -- @within wifi
int netWifiStatus(lua_State* state) {
  if (wifiState == WifiState::Connecting) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
      wifiState = WifiState::Connected;
      WIFI_STORE.setLastConnectedSsid(WiFi.SSID().c_str());
    } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL ||
               millis() - wifiAttemptStarted >= WIFI_ATTEMPT_TIMEOUT_MS) {
      startWifiCredential(++credentialIndex);
    }
  }
  const char* value = "disconnected";
  if (wifiState == WifiState::Connecting) value = "connecting";
  if (wifiState == WifiState::Connected) value = "connected";
  if (wifiState == WifiState::Failed) value = "failed";

  // A table rather than a bare string: the connected case has an address and a signal
  // strength to report, and a caller that only wants the state reads one field.
  const bool connected = wifiState == WifiState::Connected;
  lua_newtable(state);
  lua_pushstring(state, value);
  lua_setfield(state, -2, "state");
  lua_pushstring(state, connected ? WiFi.SSID().c_str() : WIFI_STORE.getLastConnectedSsid().c_str());
  lua_setfield(state, -2, "ssid");
  lua_pushstring(state, connected ? WiFi.localIP().toString().c_str() : "");
  lua_setfield(state, -2, "ip");
  lua_pushinteger(state, connected ? WiFi.RSSI() : 0);
  lua_setfield(state, -2, "rssi");
  return 1;
}

// --- Disconnects Wi-Fi if the app started it.
// -- @within wifi
int netWifiDisconnect(lua_State*) {
  if (ownsWifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    ownsWifi = false;
  }
  wifiState = WifiState::Idle;
  return 0;
}

// --- Returns whether Wi-Fi is currently connected to an access point.
// -- @return bool connected
// -- @within wifi
int netWifiIsConnected(lua_State* state) {
  lua_pushboolean(state, WiFi.status() == WL_CONNECTED);
  return 1;
}

// --- Returns the local IP address of the Wi-Fi interface.
// -- @return string ip "0.0.0.0" if not connected
// -- @within wifi
int netWifiLocalIp(lua_State* state) {
  lua_pushstring(state, WiFi.localIP().toString().c_str());
  return 1;
}
// Empirical floors, not measured peaks: downloads are observed working at 31 KB free, and the
// panic seen at 36 KB free was a 5 KB request failing against a fragmented heap, so a contiguous
// block is required alongside the total. wolfSSL panics on a failed allocation and the Arduino
// HTTP stack allocates with throwing new, so the check has to happen here while a Lua error is
// still possible.
constexpr uint32_t TLS_HEAP_FLOOR = 24 * 1024;
constexpr uint32_t TLS_BLOCK_FLOOR = 8 * 1024;

// Only wolfSSL's small allocations still land on the heap once the framebuffer is lent.
constexpr uint32_t TLS_SCRATCH_HEAP_FLOOR = 10 * 1024;

// Lend The Framebuffer To wolfSSL - by the time an app transfers, Wi-Fi and the Lua VM have left
// the heap's largest block (~22 KB) barely above wolfSSL's ~17 KB record buffer. The framebuffer's
// bytes are lent in place and never freed, so the panel keeps showing its last frame and the block
// cannot fail to come back. Lua is blocked inside the transfer call, so nothing can draw meanwhile.
class TlsScratchLoan {
 public:
  TlsScratchLoan(lua_State* state, freeink::SecureHttpClient* client) : client_(client) {
    auto* renderer = getRenderer(state);
    if (!renderer || !client_) return;

    loan_.emplace(*renderer);
    size_t length = 0;
    scratch_ = buildscratch::claim(MIN_TLS_SCRATCH, &length);
    if (!scratch_ || !tlsscratch::activate(scratch_, length)) {
      if (scratch_) buildscratch::release(scratch_);
      scratch_ = nullptr;
      loan_.reset();  // hand the framebuffer straight back; the transfer falls back to the heap
      return;
    }
    LOG_DBG("LUA", "TLS scratch: %u bytes lent from the framebuffer", (unsigned)length);
  }

  ~TlsScratchLoan() {
    if (scratch_) {
      // Close The Session First - a reusable client keeps the connection (and wolfSSL's buffers)
      // alive after a transfer, and those buffers live in the block we are about to hand back.
      client_->end();
      tlsscratch::deactivate();
      buildscratch::release(scratch_);
    }
    loan_.reset();
  }

  TlsScratchLoan(const TlsScratchLoan&) = delete;
  TlsScratchLoan& operator=(const TlsScratchLoan&) = delete;

 private:
  static constexpr size_t MIN_TLS_SCRATCH = 24 * 1024;

  freeink::SecureHttpClient* client_;
  std::optional<GfxRenderer::FrameBufferLoan> loan_;
  uint8_t* scratch_ = nullptr;
};

bool hasTlsHeadroom() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxAllocHeap();
  const bool scratchActive = scratchheap::isActive();
  LOG_INF("LUA", "Heap before TLS: free %u, largest %u%s", freeHeap, largestBlock,
          scratchActive ? ", framebuffer lent" : "");

  // With the framebuffer lent, wolfSSL's big buffers no longer come from the heap, so the floors
  // that exist to keep the handshake off a fragmented heap would refuse transfers that now fit.
  if (scratchActive) return freeHeap >= TLS_SCRATCH_HEAP_FLOOR;
  return freeHeap >= TLS_HEAP_FLOOR && largestBlock >= TLS_BLOCK_FLOOR;
}

int netRequest(lua_State* state, const char* method, bool bodyExpected) {
  auto* manager = getManager(state);
  auto* client = manager ? manager->getHttpClient() : nullptr;
  if (!client) {
    lua_pushnil(state);
    lua_pushinteger(state, -1);
    return 2;
  }

  const char* url = luaL_checkstring(state, 1);
  const char* body = "";
  size_t bodySize = 0;
  int headersIndex = 2;
  if (bodyExpected || lua_isstring(state, 2)) {
    body = luaL_optlstring(state, 2, "", &bodySize);
    headersIndex = 3;
  }

  const TlsScratchLoan scratch(state, client);

  // Same Pre-flight As net.download - the Arduino HTTP stack allocates with throwing new, so an
  // OOM here terminates the firmware instead of returning an error to the app.
  if (!hasTlsHeadroom() || !client->begin(url)) {
    lua_pushnil(state);
    lua_pushinteger(state, -1);
    return 2;
  }
  client->setFollowRedirects(5);
  client->setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (lua_istable(state, headersIndex)) {
    lua_pushnil(state);
    while (lua_next(state, headersIndex)) {
      const char* name = lua_tostring(state, -2);
      const char* value = lua_tostring(state, -1);
      if (name && value) client->addHeader(name, value);
      lua_pop(state, 1);
    }
  }

  std::string response;
  bool responseTooLarge = false;
  const int status = client->sendRequest(method, reinterpret_cast<const uint8_t*>(body), bodySize,
                                         [&response, &responseTooLarge](const uint8_t* data, size_t size) {
                                           if (response.size() + size > MAX_HTTP_RESPONSE_SIZE) {
                                             responseTooLarge = true;
                                             return false;
                                           }
                                           response.append(reinterpret_cast<const char*>(data), size);
                                           return true;
                                         });
  if (status >= 200 && status < 300 && !responseTooLarge && client->responseComplete()) {
    lua_pushlstring(state, response.data(), response.size());
  } else {
    LOG_ERR("LUA", "%s %s failed: %d%s", method, url, status, responseTooLarge ? " (response too large)" : "");
    lua_pushnil(state);
  }
  lua_pushinteger(state, status);
  return 2;
}

// --- Performs an HTTP GET request.
// -- @param url string
// -- @param headers[opt] table<string,string> Optional request headers
// -- @return string|nil body Nil on failure (capped at ~50KB)
// -- @return int status HTTP status code, or -1 if the request never happened
// -- @within http
int netGet(lua_State* state) { return netRequest(state, "GET", false); }
// --- Performs an HTTP HEAD request.
// -- @param url string
// -- @param headers[opt] table<string,string> Optional request headers
// -- @return string|nil body Nil on failure
// -- @return int status HTTP status code, or -1 if the request never happened
// -- @within http
int netHead(lua_State* state) { return netRequest(state, "HEAD", false); }
// --- Performs an HTTP DELETE request.
// -- @param url string
// -- @param headers[opt] table<string,string> Optional request headers
// -- @return string|nil body Nil on failure
// -- @return int status HTTP status code, or -1 if the request never happened
// -- @within http
int netDelete(lua_State* state) { return netRequest(state, "DELETE", false); }
// --- Performs an HTTP POST request.
// -- @param url string
// -- @param body[opt] string Request body (default "")
// -- @param headers[opt] table<string,string> Optional request headers
// -- @return string|nil body Nil on failure (capped at ~50KB)
// -- @return int status HTTP status code, or -1 if the request never happened
// -- @within http
int netPost(lua_State* state) { return netRequest(state, "POST", true); }
// --- Performs an HTTP PATCH request.
// -- @param url string
// -- @param body[opt] string Request body (default "")
// -- @param headers[opt] table<string,string> Optional request headers
// -- @return string|nil body Nil on failure (capped at ~50KB)
// -- @return int status HTTP status code, or -1 if the request never happened
// -- @within http
int netPatch(lua_State* state) { return netRequest(state, "PATCH", true); }

int pushLuaError(lua_State* state, const char* error) {
  LOG_ERR("LUA", "%s", error);
  lua_pushnil(state);
  lua_pushstring(state, error);
  return 2;
}

int hexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

bool parseSha256(const char* value, size_t length, uint8_t* output) {
  if (!value || length != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int high = hexValue(value[i * 2]);
    const int low = hexValue(value[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    output[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

const char* downloadErrorMessage(HttpDownloader::DownloadError error) {
  switch (error) {
    case HttpDownloader::OK:
      return "";
    case HttpDownloader::HTTP_ERROR:
      return "HTTPS download failed";
    case HttpDownloader::FILE_ERROR:
      return "Download destination failed";
    case HttpDownloader::ABORTED:
      return "Download aborted";
    case HttpDownloader::INVALID_URL:
      return "Download requires a valid HTTPS URL";
    case HttpDownloader::LIMIT_EXCEEDED:
      return "Download exceeded maxBytes";
    case HttpDownloader::SIZE_MISMATCH:
      return "Downloaded size did not match expectedSize";
    case HttpDownloader::HASH_MISMATCH:
      return "Downloaded SHA-256 did not match";
  }
  return "Download failed";
}

// --- Downloads an HTTPS URL to a file with optional integrity checks.
// -- @param url string HTTPS URL
// -- @param destination string Absolute destination path on the SD card
// -- @param options table { maxBytes: int (required, up to 16MB), expectedSize?: int, sha256?: string (64 hex chars) }
// -- @return int|nil bytesWritten Nil on failure
// -- @return string|nil error Present when the download fails
// -- @within http
int netDownload(lua_State* state) {
  const char* url = luaL_checkstring(state, 1);
  const char* destination = luaL_checkstring(state, 2);
  if (!lua_istable(state, 3)) return pushLuaError(state, "net.download options table is required");

  lua_getfield(state, 3, "maxBytes");
  if (!lua_isinteger(state, -1)) return pushLuaError(state, "maxBytes must be an integer");
  const lua_Integer requestedMax = lua_tointeger(state, -1);
  lua_pop(state, 1);
  if (requestedMax <= 0 || static_cast<uint64_t>(requestedMax) > MAX_DOWNLOAD_SIZE) {
    return pushLuaError(state, "maxBytes is outside the allowed range");
  }
  const size_t maxBytes = static_cast<size_t>(requestedMax);

  size_t expectedSize = 0;
  lua_getfield(state, 3, "expectedSize");
  if (!lua_isnil(state, -1)) {
    if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) <= 0) {
      return pushLuaError(state, "expectedSize must be a positive integer");
    }
    expectedSize = static_cast<size_t>(lua_tointeger(state, -1));
    if (expectedSize > maxBytes) return pushLuaError(state, "expectedSize exceeds maxBytes");
  }
  lua_pop(state, 1);

  uint8_t expectedHash[32];
  const uint8_t* expectedHashPtr = nullptr;
  lua_getfield(state, 3, "sha256");
  if (!lua_isnil(state, -1)) {
    size_t hashLength = 0;
    const char* hash = lua_tolstring(state, -1, &hashLength);
    if (!parseSha256(hash, hashLength, expectedHash)) return pushLuaError(state, "sha256 must be 64 hex characters");
    expectedHashPtr = expectedHash;
  }
  lua_pop(state, 1);

  lua_pushnil(state);
  while (lua_next(state, 3)) {
    const char* key = lua_tostring(state, -2);
    if (!key || (strcmp(key, "maxBytes") != 0 && strcmp(key, "expectedSize") != 0 && strcmp(key, "sha256") != 0)) {
      return pushLuaError(state, "Unknown net.download option");
    }
    lua_pop(state, 1);
  }

  lua_gc(state, LUA_GCCOLLECT, 0);
  auto* manager = getManager(state);
  auto* client = manager ? manager->getHttpClient() : nullptr;
  if (!client) return pushLuaError(state, "Not enough memory for HTTPS client");

  // Pre-flight Heap Check - A failed allocation inside wolfSSL panics the device
  // (CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS), so refuse the handshake here while a Lua error
  // is still possible.
  // Lend Before Measuring - the loan is what makes the transfer affordable, so the pre-flight has
  // to see the heap as it will be during the handshake, not before.
  const TlsScratchLoan scratch(state, client);

  if (!hasTlsHeadroom()) return pushLuaError(state, "Not enough memory to start a secure download");

  size_t bytesWritten = 0;
  const auto result = HttpDownloader::downloadBoundedToFile(url, destination, maxBytes, expectedSize, expectedHashPtr,
                                                            bytesWritten, client);
  if (result != HttpDownloader::OK) return pushLuaError(state, downloadErrorMessage(result));

  lua_pushinteger(state, static_cast<lua_Integer>(bytesWritten));
  return 1;
}

// --- Percent-encodes a string for use in a URL query.
// -- @param input string
// -- @return string encoded
// -- @within http
int netUrlEncode(lua_State* state) {
  size_t size = 0;
  const auto* input = reinterpret_cast<const unsigned char*>(luaL_checklstring(state, 1, &size));
  luaL_Buffer output;
  luaL_buffinit(state, &output);
  char encoded[4];
  for (size_t i = 0; i < size; ++i) {
    const unsigned char c = input[i];
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      luaL_addchar(&output, c);
    } else {
      snprintf(encoded, sizeof(encoded), "%%%02X", c);
      luaL_addlstring(&output, encoded, 3);
    }
  }
  luaL_pushresult(&output);
  return 1;
}
}  // namespace

void luabindings::registerNet(lua_State* state) {
  lua_newtable(state);
  addFunction(state, "connect", netWifiConnect);
  addFunction(state, "status", netWifiStatus);
  addFunction(state, "disconnect", netWifiDisconnect);
  addFunction(state, "isConnected", netWifiIsConnected);
  addFunction(state, "localIP", netWifiLocalIp);
  lua_setglobal(state, "wifi");

  lua_newtable(state);
  addFunction(state, "get", netGet);
  addFunction(state, "head", netHead);
  addFunction(state, "delete", netDelete);
  addFunction(state, "post", netPost);
  addFunction(state, "patch", netPatch);
  addFunction(state, "download", netDownload);
  addFunction(state, "urlencode", netUrlEncode);
  lua_setglobal(state, "http");
}

void luabindings::shutdownNet() {
  if (ownsWifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    ownsWifi = false;
  }
  wifiState = WifiState::Idle;
}
