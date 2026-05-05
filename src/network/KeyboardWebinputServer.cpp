#include "KeyboardWebInputServer.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "NetworkConstants.h"
#include "WifiCredentialStore.h"
#include "html/TextInputPageHtml.generated.h"

#include <algorithm>
#include <vector>

namespace {
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 5000;
constexpr int MAX_STA_CONNECT_ATTEMPTS = 3;
}  // namespace

KeyboardWebInputServer::~KeyboardWebInputServer() { stop(); }

bool KeyboardWebInputServer::start() {
  if (running) {
    return true;
  }

  Serial.printf("[%lu] [KB-WEB] Starting keyboard web input server...\n", millis());

  // Save current WiFi sleep mode to restore later
  wifi_ps_type_t psType;
  if (esp_wifi_get_ps(&psType) == ESP_OK) {
    previousSleepMode = psType;
  }

  // Check if WiFi is already connected in STA mode
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);

  staModeStarted = false;

  if (isStaConnected) {
    // Reuse existing WiFi connection
    apModeStarted = false;
    ipAddress = WiFi.localIP().toString().c_str();
    Serial.printf("[%lu] [KB-WEB] Using existing STA connection, IP: %s\n", millis(), ipAddress.c_str());
  } else if (tryConnectSavedNetwork()) {
    // Connected to an existing network using saved credentials
    apModeStarted = false;
    staModeStarted = true;
    ipAddress = WiFi.localIP().toString().c_str();
    Serial.printf("[%lu] [KB-WEB] Connected using saved WiFi, IP: %s\n", millis(), ipAddress.c_str());
  } else {
    // Start our own Access Point
    Serial.printf("[%lu] [KB-WEB] No WiFi connection, starting AP...\n", millis());

    WiFi.mode(WIFI_AP);
    delay(100);

    if (NetworkConstants::AP_PASSWORD && strlen(NetworkConstants::AP_PASSWORD) >= 8) {
      WiFi.softAP(NetworkConstants::AP_SSID, NetworkConstants::AP_PASSWORD, NetworkConstants::AP_CHANNEL, false,
                  NetworkConstants::AP_MAX_CONNECTIONS);
    } else {
      WiFi.softAP(NetworkConstants::AP_SSID, nullptr, NetworkConstants::AP_CHANNEL, false,
                  NetworkConstants::AP_MAX_CONNECTIONS);
    }

    // Wait for AP to fully initialize
    delay(100);

    const IPAddress apIP = WiFi.softAPIP();
    ipAddress = apIP.toString().c_str();
    apModeStarted = true;

    Serial.printf("[%lu] [KB-WEB] AP started - SSID: %s, IP: %s\n", millis(), NetworkConstants::AP_SSID,
                  ipAddress.c_str());

    // Start DNS server for captive portal behavior
    // This redirects all DNS queries to our IP, making any domain resolve to us
    dnsServer.reset(new DNSServer());
    dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer->start(NetworkConstants::DNS_PORT, "*", apIP);
    Serial.printf("[%lu] [KB-WEB] DNS server started for captive portal\n", millis());
  }

  // Start mDNS
  if (MDNS.begin(NetworkConstants::AP_HOSTNAME)) {
    MDNS.addService("http", "tcp", NetworkConstants::HTTP_PORT);
    Serial.printf("[%lu] [KB-WEB] mDNS started: http://%s.local/\n", millis(), NetworkConstants::AP_HOSTNAME);
  }

  // Disable WiFi sleep for responsiveness
  WiFi.setSleep(false);

  // Create and start web server
  server.reset(new WebServer(NetworkConstants::HTTP_PORT));
  setupRoutes();
  server->begin();

  running = true;
  textReceived = false;
  receivedText.clear();

  Serial.printf("[%lu] [KB-WEB] Server started on port %d\n", millis(), NetworkConstants::HTTP_PORT);
  return true;
}

void KeyboardWebInputServer::stop() {
  if (!running) {
    return;
  }

  Serial.printf("[%lu] [KB-WEB] Stopping keyboard web input server...\n", millis());

  if (server) {
    server->stop();
    server.reset();
  }

  MDNS.end();

  // Stop DNS server if running (AP mode captive portal)
  if (dnsServer) {
    dnsServer->stop();
    dnsServer.reset();
    Serial.printf("[%lu] [KB-WEB] DNS server stopped\n", millis());
  }

  // Brief wait for LWIP stack to flush pending packets
  delay(50);

  if (apModeStarted) {
    WiFi.softAPdisconnect(true);
    delay(30);
    WiFi.mode(WIFI_OFF);
    apModeStarted = false;
    Serial.printf("[%lu] [KB-WEB] AP stopped\n", millis());
  } else if (staModeStarted) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
    staModeStarted = false;
    Serial.printf("[%lu] [KB-WEB] STA connection stopped\n", millis());
  } else {
    // Restore previous WiFi sleep mode
    esp_wifi_set_ps(previousSleepMode);
  }

  running = false;
  Serial.printf("[%lu] [KB-WEB] Server stopped\n", millis());
}

void KeyboardWebInputServer::handleClient() {
  if (running && server) {
    server->handleClient();
  }
  // Process DNS requests for captive portal (AP mode)
  if (running && dnsServer) {
    dnsServer->processNextRequest();
  }
}

std::string KeyboardWebInputServer::consumeReceivedText() {
  textReceived = false;
  std::string result = std::move(receivedText);
  receivedText.clear();
  return result;
}

std::string KeyboardWebInputServer::getApSSID() const { return NetworkConstants::AP_SSID; }

std::string KeyboardWebInputServer::getUrl() const {
  if (apModeStarted) {
    return std::string("http://") + NetworkConstants::AP_HOSTNAME + ".local/";
  }
  return "http://" + ipAddress + "/";
}

std::string KeyboardWebInputServer::getWifiQRString() const {
  return std::string("WIFI:S:") + NetworkConstants::AP_SSID + ";;";
}

bool KeyboardWebInputServer::tryConnectSavedNetwork() {
  WIFI_STORE.loadFromFile();
  const auto& saved = WIFI_STORE.getCredentials();
  if (saved.empty()) {
    Serial.printf("[%lu] [KB-WEB] No saved WiFi credentials\n", millis());
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  const int16_t scanCount = WiFi.scanNetworks(false, true);
  if (scanCount <= 0) {
    Serial.printf("[%lu] [KB-WEB] WiFi scan failed or no networks found (%d)\n", millis(), scanCount);
    WiFi.scanDelete();
    return false;
  }

  struct Candidate {
    std::string ssid;
    std::string password;
    int32_t rssi;
  };

  std::vector<Candidate> candidates;
  candidates.reserve(static_cast<size_t>(scanCount));

  for (int i = 0; i < scanCount; i++) {
    const std::string ssid = WiFi.SSID(i).c_str();
    if (ssid.empty()) {
      continue;
    }

    for (const auto& cred : saved) {
      if (cred.ssid == ssid) {
        candidates.push_back({cred.ssid, cred.password, WiFi.RSSI(i)});
        break;
      }
    }
  }

  WiFi.scanDelete();

  if (candidates.empty()) {
    Serial.printf("[%lu] [KB-WEB] No saved WiFi networks are currently available\n", millis());
    return false;
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.rssi > b.rssi; });

  const size_t attemptCount = std::min(candidates.size(), static_cast<size_t>(MAX_STA_CONNECT_ATTEMPTS));
  for (size_t i = 0; i < attemptCount; i++) {
    const Candidate& c = candidates[i];
    Serial.printf("[%lu] [KB-WEB] Trying saved network: %s (RSSI=%ld)\n", millis(), c.ssid.c_str(),
                  static_cast<long>(c.rssi));

    WiFi.disconnect();
    delay(50);
    if (!c.password.empty()) {
      WiFi.begin(c.ssid.c_str(), c.password.c_str());
    } else {
      WiFi.begin(c.ssid.c_str());
    }

    const unsigned long startAt = millis();
    while (millis() - startAt < WIFI_CONNECT_TIMEOUT_MS) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        Serial.printf("[%lu] [KB-WEB] Connected to saved network: %s\n", millis(), c.ssid.c_str());
        return true;
      }
      delay(100);
    }
  }

  WiFi.disconnect();
  Serial.printf("[%lu] [KB-WEB] Failed to connect to any saved network\n", millis());
  return false;
}

void KeyboardWebInputServer::setupRoutes() {
  server->on("/", HTTP_GET, [this] { handleRootPage(); });
  server->on("/api/keyboard-input", HTTP_POST, [this] { handleTextSubmit(); });

  // Captive portal: redirect any unknown page to root
  server->onNotFound([this] {
    server->sendHeader("Location", "/", true);
    server->send(302, "text/plain", "Redirecting...");
  });
}

void KeyboardWebInputServer::handleRootPage() {
  server->send(200, "text/html", TextInputPageHtml);
  Serial.printf("[%lu] [KB-WEB] Served text input page\n", millis());
}

void KeyboardWebInputServer::handleTextSubmit() {
  if (!server->hasArg("text")) {
    server->send(400, "text/plain", "Missing 'text' parameter");
    return;
  }

  receivedText = server->arg("text").c_str();
  textReceived = true;

  Serial.printf("[%lu] [KB-WEB] Received text (%zu chars): %.40s%s\n", millis(), receivedText.length(),
                receivedText.c_str(), receivedText.length() > 40 ? "..." : "");

  server->send(200, "text/plain", "OK");
}