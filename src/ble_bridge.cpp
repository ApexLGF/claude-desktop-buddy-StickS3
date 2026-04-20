#include "ble_bridge.h"
#include <NimBLEDevice.h>
#include <Arduino.h>
#include <esp_mac.h>
#include <string.h>

// Nordic UART Service UUIDs — every BLE serial example uses these, so
// existing tools (nRF Connect, bluefy, Web Bluetooth examples) can talk to
// us without custom UUIDs.
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Incoming bytes are buffered in a simple ring for bleRead()/bleAvailable().
static const size_t RX_CAP = 2048;
static uint8_t  rxBuf[RX_CAP];
static volatile size_t rxHead = 0;
static volatile size_t rxTail = 0;

static NimBLEServer*         server = nullptr;
static NimBLECharacteristic* txChar = nullptr;
static NimBLECharacteristic* rxChar = nullptr;
static volatile bool      connected = false;
static volatile bool      secure = false;
static volatile uint32_t  passkey = 0;
static volatile uint16_t  mtu = 23;
static volatile uint16_t  connHandle = 0xFFFF;

static void rxPush(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t next = (rxHead + 1) % RX_CAP;
    if (next == rxTail) return;
    rxBuf[rxHead] = p[i];
    rxHead = next;
  }
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    if (v.length() > 0) rxPush((const uint8_t*)v.data(), v.length());
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    connected = true;
    connHandle = info.getConnHandle();
    mtu = 23;
    Serial.println("[ble] connected");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    connected = false;
    secure = false;
    passkey = 0;
    mtu = 23;
    connHandle = 0xFFFF;
    Serial.printf("[ble] disconnected reason=%d\n", reason);
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t newMtu, NimBLEConnInfo& info) override {
    mtu = newMtu;
  }
  uint32_t onPassKeyDisplay() override {
    uint32_t pk = (uint32_t)(esp_random() % 1000000);
    passkey = pk;
    Serial.printf("[ble] passkey %06lu\n", (unsigned long)pk);
    return pk;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    passkey = 0;
    secure = info.isEncrypted();
    Serial.printf("[ble] auth %s\n", secure ? "ok" : "FAIL");
    if (!secure && server) server->disconnect(info.getConnHandle());
  }
};

bool bleInit(const char* deviceName) {
  NimBLEDevice::init(deviceName);
  // Use a BLE Static Random Address derived from the eFuse BT MAC so the
  // device keeps the same address across reboots (pair once, reconnect
  // forever) while still differing from the public MAC — which dodges any
  // stale name cache hosts may have keyed on the public address.
  {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    // NimBLE wants little-endian (LSB first); mac[5] becomes addr[0].
    uint8_t addr[6] = { mac[5], mac[4], mac[3], mac[2], mac[1], mac[0] };
    // Static Random Address: top 2 bits of MSB must be 11.
    addr[5] |= 0xC0;
    NimBLEDevice::setOwnAddr(addr);
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
  }
  NimBLEDevice::setMTU(512);
  // LE Secure Connections + MITM + bonding; device is DisplayOnly so it
  // shows a 6-digit passkey the user types on the desktop.
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  Serial.printf("BLE name: %s\n", deviceName);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = server->createService(NUS_SERVICE_UUID);

  // _AUTHEN flags require an encrypted + MITM-authenticated link before
  // the central can subscribe (CCCD write) or send RX data.
  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_AUTHEN
  );

  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_AUTHEN
  );
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  // Primary adv packet: flags + 128-bit NUS UUID (already 21B, no room for name).
  // Scan response: full Local Name so phones and macOS see the real broadcast
  // name instead of falling back to a cached one.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->enableScanResponse(true);
  adv->addServiceUUID(svc->getUUID());
  adv->setName(deviceName);
  adv->setMinInterval(800);   // 800 * 0.625ms = 500ms
  adv->setMaxInterval(1280);  // 1280 * 0.625ms = 800ms
  adv->start();

  return true;
}

bool bleConnected() { return connected; }
bool bleSecure()    { return secure; }
uint32_t blePasskey() { return passkey; }

void bleClearBonds() {
  NimBLEDevice::deleteAllBonds();
  Serial.println("[ble] cleared all bonds");
}

size_t bleAvailable() {
  return (rxHead + RX_CAP - rxTail) % RX_CAP;
}

int bleRead() {
  if (rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  if (!connected || !txChar) return 0;
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  size_t sent = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue((uint8_t*)(data + sent), n);
    txChar->notify();
    sent += n;
    delay(4);
  }
  return sent;
}
