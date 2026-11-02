/* ======================================================================
   WEB UI, LOGIN, SETTINGS, DIAGNOSIS ENGINE, USB CONSOLE  (webTask, Core 0)
   ======================================================================
   Conventions copied from the Gateway sketch so both products behave the
   same for a field technician:
   - always-on Access Point (DEFAULT_AP_SSID / DEFAULT_AP_PASSWORD,
     http://192.168.4.1) + optional site WiFi (STA) with periodic retry
   - single admin session: salted SHA-256 password hash in NVS ("auth"),
     HttpOnly session cookie, 30 min sliding idle timeout, 5 failed logins
     lock the login page for 60 s, red banner while admin/admin is in use
   - every protected handler starts with requireAuth()
   - key events go to a short Status Log shown on the dashboard
   - credential recovery needs physical USB access: open the COM port at
     CONSOLE_BAUD (74880) and send "factory-reset-auth"
   Nothing in this file touches Serial1 or the bridge data path; it reads
   statistics through snapshots taken under statsMux.
   ====================================================================== */

WebServer server(80);

String webSessionToken = "";
unsigned long webSessionLastActivity = 0;
int webLoginFailCount = 0;
unsigned long webLoginLockStart = 0;

unsigned long lastWifiAttempt = 0;
bool lastWifiConnected = false;

int nvsWriteFailures = 0;

// ===================== SHA-256 (same implementation as the Gateway) =====================
static const uint32_t SHA256_K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

void sha256Transform(uint32_t *state, const uint8_t *data) {
  uint32_t m[64];
  for (int i = 0; i < 16; i++) {
    m[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) | ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROTR32(m[i - 15], 7) ^ ROTR32(m[i - 15], 18) ^ (m[i - 15] >> 3);
    uint32_t s1 = ROTR32(m[i - 2], 17) ^ ROTR32(m[i - 2], 19) ^ (m[i - 2] >> 10);
    m[i] = m[i - 16] + s0 + m[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + SHA256_K[i] + m[i];
    uint32_t S0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

String sha256Hex(const String &input) {
  Sha256Ctx ctx;
  ctx.state[0] = 0x6a09e667; ctx.state[1] = 0xbb67ae85; ctx.state[2] = 0x3c6ef372; ctx.state[3] = 0xa54ff53a;
  ctx.state[4] = 0x510e527f; ctx.state[5] = 0x9b05688c; ctx.state[6] = 0x1f83d9ab; ctx.state[7] = 0x5be0cd19;
  ctx.bitlen = 0;
  ctx.datalen = 0;

  for (unsigned int i = 0; i < input.length(); i++) {
    ctx.data[ctx.datalen++] = (uint8_t)input[i];
    if (ctx.datalen == 64) {
      sha256Transform(ctx.state, ctx.data);
      ctx.bitlen += 512;
      ctx.datalen = 0;
    }
  }

  uint32_t i = ctx.datalen;
  ctx.bitlen += (uint64_t)ctx.datalen * 8;
  ctx.data[i++] = 0x80;
  if (i > 56) {
    while (i < 64) ctx.data[i++] = 0x00;
    sha256Transform(ctx.state, ctx.data);
    i = 0;
  }
  while (i < 56) ctx.data[i++] = 0x00;
  for (int j = 7; j >= 0; j--) {
    ctx.data[i++] = (uint8_t)(ctx.bitlen >> (j * 8));
  }
  sha256Transform(ctx.state, ctx.data);

  String out = "";
  out.reserve(64);
  for (int w = 0; w < 8; w++) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      uint8_t nibble = (ctx.state[w] >> shift) & 0xF;
      out += (char)(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10));
    }
  }
  return out;
}

String makeRandomHex(int nBytes) {
  String out = "";
  out.reserve(nBytes * 2);
  const char *hex = "0123456789abcdef";
  for (int i = 0; i < nBytes; i++) {
    uint8_t b = (uint8_t)(esp_random() & 0xFF);
    out += hex[b >> 4];
    out += hex[b & 0xF];
  }
  return out;
}

// ===================== Escape helpers =====================
String htmlEscape(String text) {
  text.replace("&", "&amp;");
  text.replace("<", "&lt;");
  text.replace(">", "&gt;");
  text.replace("\"", "&quot;");
  text.replace("'", "&#39;");
  return text;
}

String jsonEscape(String text) {
  text.replace("\\", "\\\\");
  text.replace("\"", "\\\"");
  text.replace("\n", "\\n");
  text.replace("\r", "\\r");
  return text;
}

// ===================== NVS config =====================
void trackNvsString(const char *key, const String &value) {
  size_t written = preferences.putString(key, value);
  if (value.length() > 0 && written == 0) nvsWriteFailures++;
}

void trackNvsU32(const char *key, uint32_t value) {
  if (preferences.putUInt(key, value) == 0) nvsWriteFailures++;
}

bool saveAuthConfig() {
  nvsWriteFailures = 0;
  preferences.begin("auth", false);
  trackNvsString("user", authConfig.user);
  trackNvsString("salt", authConfig.salt);
  trackNvsString("hash", authConfig.passHash);
  preferences.end();
  if (nvsWriteFailures) logKeyEvent("NVS SAVE INCOMPLETE (auth): %d write(s) failed", nvsWriteFailures);
  return nvsWriteFailures == 0;
}

void loadAuthConfig() {
  preferences.begin("auth", true);
  authConfig.user = preferences.getString("user", WEB_AUTH_DEFAULT_USER);
  authConfig.salt = preferences.getString("salt", "");
  authConfig.passHash = preferences.getString("hash", "");
  preferences.end();
  if (authConfig.user.length() == 0) authConfig.user = WEB_AUTH_DEFAULT_USER;
}

bool saveDeviceConfig() {
  nvsWriteFailures = 0;
  preferences.begin("device", false);
  trackNvsString("apSsid", deviceConfig.apSsid);
  trackNvsString("apPass", deviceConfig.apPassword);
  preferences.end();
  if (nvsWriteFailures) logKeyEvent("NVS SAVE INCOMPLETE (device): %d write(s) failed", nvsWriteFailures);
  return nvsWriteFailures == 0;
}

void loadDeviceConfig() {
  preferences.begin("device", true);
  deviceConfig.apSsid = preferences.getString("apSsid", DEFAULT_AP_SSID);
  deviceConfig.apPassword = preferences.getString("apPass", DEFAULT_AP_PASSWORD);
  preferences.end();
  if (deviceConfig.apSsid.length() == 0) deviceConfig.apSsid = DEFAULT_AP_SSID;
  if (deviceConfig.apPassword.length() < AP_PASSWORD_MIN_LEN) deviceConfig.apPassword = DEFAULT_AP_PASSWORD;
}

bool saveWifiConfig() {
  nvsWriteFailures = 0;
  preferences.begin("wifi", false);
  trackNvsString("ssid", wifiConfig.ssid);
  trackNvsString("pass", wifiConfig.password);
  preferences.end();
  if (nvsWriteFailures) logKeyEvent("NVS SAVE INCOMPLETE (wifi): %d write(s) failed", nvsWriteFailures);
  return nvsWriteFailures == 0;
}

void loadWifiConfig() {
  preferences.begin("wifi", true);
  wifiConfig.ssid = preferences.getString("ssid", "");
  wifiConfig.password = preferences.getString("pass", "");
  preferences.end();
}

bool saveDiagConfig() {
  nvsWriteFailures = 0;
  preferences.begin("diag", false);
  trackNvsU32("rspTo", diagResponseTimeoutMs);
  trackNvsU32("deadTh", diagDeadSlaveThreshold);
  preferences.end();
  if (nvsWriteFailures) logKeyEvent("NVS SAVE INCOMPLETE (diag): %d write(s) failed", nvsWriteFailures);
  return nvsWriteFailures == 0;
}

void loadDiagConfig() {
  preferences.begin("diag", true);
  uint32_t to = preferences.getUInt("rspTo", DEFAULT_DIAG_RESPONSE_TIMEOUT_MS);
  uint32_t th = preferences.getUInt("deadTh", DEFAULT_DEAD_SLAVE_THRESHOLD);
  preferences.end();
  diagResponseTimeoutMs = constrain(to, (uint32_t)MIN_DIAG_RESPONSE_TIMEOUT_MS, (uint32_t)MAX_DIAG_RESPONSE_TIMEOUT_MS);
  diagDeadSlaveThreshold = (uint16_t)constrain(th, (uint32_t)1, (uint32_t)1000);
}

void loadAllConfig() {
  loadAuthConfig();
  loadDeviceConfig();
  loadWifiConfig();
  loadDiagConfig();
}

// ===================== Auth (same rules as the Gateway) =====================
bool webAuthUsingDefaults() {
  return authConfig.passHash.length() == 0;
}

bool checkWebCredentials(const String &user, const String &pass) {
  if (user != authConfig.user) return false;
  if (webAuthUsingDefaults()) return pass == WEB_AUTH_DEFAULT_PASS;
  return sha256Hex(authConfig.salt + pass) == authConfig.passHash;
}

bool isAuthedRequest() {
  if (webSessionToken.length() == 0) return false;

  if (millis() - webSessionLastActivity > WEB_SESSION_TIMEOUT_MS) {
    webSessionToken = "";
    return false;
  }

  String cookie = server.header("Cookie");
  int idx = cookie.indexOf("DGSESSION=");
  if (idx < 0) return false;

  int valStart = idx + 10;
  int valEnd = cookie.indexOf(';', valStart);
  String token = (valEnd < 0) ? cookie.substring(valStart) : cookie.substring(valStart, valEnd);
  token.trim();

  if (token != webSessionToken) return false;

  webSessionLastActivity = millis();
  return true;
}

// Page handlers: redirect to /login.
bool requireAuth() {
  if (isAuthedRequest()) return true;
  server.sendHeader("Location", "/login");
  server.send(302);
  return false;
}

// JSON API handlers: 401 so the page script can redirect itself.
bool requireAuthApi() {
  if (isAuthedRequest()) return true;
  server.send(401, "application/json", "{\"error\":\"login required\"}");
  return false;
}

bool webLoginLockedOut() {
  if (webLoginFailCount < WEB_LOGIN_MAX_FAILS) return false;
  if (millis() - webLoginLockStart >= WEB_LOGIN_LOCKOUT_MS) {
    webLoginFailCount = 0;
    return false;
  }
  return true;
}

// ===================== WiFi =====================
const char *wifiPassphraseOrNull() {
  return (wifiConfig.password.length() > 0) ? wifiConfig.password.c_str() : NULL;
}

void connectSiteWiFi() {
  if (wifiConfig.ssid.length() == 0) return;
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(wifiConfig.ssid.c_str(), wifiPassphraseOrNull());
}

void handleWifiRetry() {
  if (wifiConfig.ssid.length() == 0) return;

  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected != lastWifiConnected) {
    lastWifiConnected = connected;
    if (connected) logKeyEvent("WIFI CONNECTED: %s", WiFi.localIP().toString().c_str());
    else logKeyEvent("WIFI DISCONNECTED: %s", wifiConfig.ssid.c_str());
  }

  if (connected) return;
  if (millis() - lastWifiAttempt > WIFI_RETRY_INTERVAL_MS) {
    lastWifiAttempt = millis();
    WiFi.disconnect();
    delay(100);
    WiFi.begin(wifiConfig.ssid.c_str(), wifiPassphraseOrNull());
  }
}

String hostnameString() {
  uint64_t efuseMac = ESP.getEfuseMac();
  char hostname[24];
  snprintf(hostname, sizeof(hostname), HOSTNAME_PREFIX "%02X%02X%02X",
           (uint8_t)(efuseMac >> 24), (uint8_t)(efuseMac >> 32), (uint8_t)(efuseMac >> 40));
  return String(hostname);
}

// ===================== Snapshot + diagnosis engine =====================
SlaveStats snapSlaves[SLAVE_SLOTS];
GlobalStats snapG;
TxLogEntry snapLog[TXLOG_SIZE];
uint8_t snapLogHead = 0;
uint32_t snapLogTotal = 0;

Finding findings[MAX_FINDINGS];
int findingCount = 0;
uint8_t overallSev = SEV_OK;
uint32_t recommendedTimeoutMs = 100;

void takeSnapshot() {
  portENTER_CRITICAL(&statsMux);
  memcpy(snapSlaves, slaveStats, sizeof(snapSlaves));
  memcpy(&snapG, &gstats, sizeof(snapG));
  memcpy(snapLog, txLog, sizeof(snapLog));
  snapLogHead = txLogHead;
  snapLogTotal = txLogTotal;
  portEXIT_CRITICAL(&statsMux);
}

void resetStatistics() {
  portENTER_CRITICAL(&statsMux);
  memset(slaveStats, 0, sizeof(slaveStats));
  memset(&gstats, 0, sizeof(gstats));
  memset(txLog, 0, sizeof(txLog));
  txLogHead = 0;
  txLogTotal = 0;
  gstats.statsSinceMs = millis();
  portEXIT_CRITICAL(&statsMux);
  idleLevel = IDLE_UNKNOWN;
}

// cause: one or two sentences. remedy: steps separated by '\n', shown as a
// numbered checklist. action: optional button (Auto-scan, Reset statistics...).
// (No default argument: the Arduino builder copies signatures into
// auto-generated prototypes, and a default given twice fails to compile.)
void addFinding(uint8_t sev, const String &title, const String &cause, const String &remedy, uint8_t action) {
  if (findingCount < MAX_FINDINGS) {
    Finding &f = findings[findingCount++];
    f.sev = sev;
    f.action = action;
    f.title = title;
    f.cause = cause;
    f.remedy = remedy;
  }
  if (sev > overallSev) overallSev = sev;
}

void addFinding(uint8_t sev, const String &title, const String &cause, const String &remedy) {
  addFinding(sev, title, cause, remedy, ACT_NONE);
}

String msText(uint32_t us) {
  return String(us / 1000.0, 1);
}

// Master response timeout that comfortably covers the slowest reply seen:
// 2x worst latency + 20 ms, at least 100 ms, rounded up to 50 ms.
uint32_t recommendTimeoutMs(uint32_t worstLatUs) {
  uint32_t ms = (2 * worstLatUs) / 1000 + 20;
  if (ms < 100) ms = 100;
  return ((ms + 49) / 50) * 50;
}

const char *excName(uint8_t code) {
  switch (code) {
    case 1: return "Illegal Function";
    case 2: return "Illegal Data Address";
    case 3: return "Illegal Data Value";
    case 4: return "Slave Device Failure";
    case 5: return "Acknowledge";
    case 6: return "Slave Device Busy";
    case 8: return "Memory Parity Error";
    case 10: return "Gateway Path Unavailable";
    case 11: return "Gateway Target No Response";
    default: return "Exception";
  }
}

const char *resultName(uint8_t r) {
  switch (r) {
    case TR_OK: return "OK";
    case TR_EXCEPTION: return "EXCEPTION";
    case TR_TIMEOUT: return "TIMEOUT";
    case TR_CRC: return "CRC/LENGTH";
    case TR_WRONG_SLAVE: return "WRONG ID";
    case TR_BAD_FUNCTION: return "BAD FC";
    case TR_BROADCAST: return "BROADCAST";
    case TR_LATE: return "LATE REPLY";
    case TR_FOREIGN: return "FOREIGN";
    case TR_NON_RTU: return "NOT RTU";
    default: return "?";
  }
}

const char *sevName(uint8_t s) {
  switch (s) {
    case SEV_FAULT: return "FAULT";
    case SEV_WARN: return "WARN";
    case SEV_INFO: return "INFO";
    default: return "OK";
  }
}

// Common remedy step lists, reused by several findings.
#define STEPS_SETTINGS_MISMATCH(line) \
  String("Find the device's communication settings (manual, front panel or its configuration software) and compare them with the PC port: ") + line + ".\n" \
  "Set the Modbus master to the device's baud rate, parity and stop bits exactly (8E1 and 8N1 are the most common).\n" \
  "If the settings are unknown, close the master and run Auto-scan - it reports the baud rate, format and ID that answer.\n" \
  "Press Reset statistics and poll again to confirm."

// The dongle board already has its 120 ohm termination fitted, so the
// remedies send the user to the far (sensor) end of the cable.
#define STEPS_TERMINATION \
  "The dongle end is already terminated (120 \u03A9 on the board). Check the SENSOR end: the last device on the far end of the cable needs 120 \u03A9 across A-B - fit a resistor or switch on its termination DIP switch.\n" \
  "Make sure no device in the middle of the cable has termination switched on - only the dongle and the last sensor.\n" \
  "To verify with everything powered off: resistance A-B at the dongle is about 50-65 \u03A9 with both ends terminated, about 100-120 \u03A9 if the sensor end is missing, below 45 \u03A9 if too many devices are terminated.\n"

#define STEPS_NOISE \
  STEPS_TERMINATION \
  "Use shielded twisted pair, A and B on one pair; connect the shield to earth at one end only.\n" \
  "Connect the RS485 GND/COM of all devices together (signal reference).\n" \
  "Route the cable away from VFD, motor and contactor wiring; cross power cables at 90\u00B0.\n" \
  "Wire the bus as a daisy chain and keep stubs to each device short (< 1 m).\n" \
  "If the cable is long (> 300 m at 19200 baud and above), reduce the baud rate.\n" \
  "Press Reset statistics and poll again to confirm."

#define STEPS_BIAS \
  "Add fail-safe bias at ONE point on the bus (usually at the master end): 330 \u03A9 from A (D+) to +5 V and 330 \u03A9 from B (D-) to GND.\n" \
  "With the bus idle, measure A to B with a multimeter: +0.3 to +0.8 V is acceptable (typically about +0.7 V). +0.2 to +0.3 V is marginal, below +0.2 V = bias missing, negative = A/B swapped, above +1 V = termination missing.\n" \
  "Check whether a device already has switchable bias/termination (DIP switch) - enable it on one device only.\n" \
  "Press Reset statistics and check that the idle level reads HIGH."

void diagnoseExceptions(const String &sid, const SlaveStats &s) {
  for (uint8_t code = 1; code < 16; code++) {
    if (!(s.excMask & (1 << code))) continue;
    switch (code) {
      case 1:
        addFinding(SEV_WARN, sid + ": function not supported (exception 01)",
                   "The device answered but does not implement the function code that was requested.",
                   "Check the device manual for the supported function codes.\n"
                   "Measured values are often input registers: switch the master between FC03 (Read Holding Registers) and FC04 (Read Input Registers).\n"
                   "For coils/discrete inputs use FC01/FC02, for writes FC06 (single) or FC16 (multiple) - some devices support only one of the two.\n"
                   "Press Reset statistics and poll again to confirm.", ACT_RESET);
        break;
      case 2:
        addFinding(SEV_WARN, sid + ": illegal data address (exception 02)",
                   "The device answered, but the requested register (or part of the block) does not exist in its register map.",
                   "Look up the register in the device manual.\n"
                   "If the manual lists 40001-style numbers (holding) or 30001-style (input), remove the prefix and subtract 1: 40001 becomes address 0, 30010 becomes address 9.\n"
                   "If the manual is 1-based (register 1 = first register), subtract 1 from the address.\n"
                   "If you read a block, reduce the quantity - the block may cross a gap in the register map. Read the registers one at a time to find the gap.\n"
                   "Check whether the master software has a 'base 0 / base 1' or 'PLC addresses' option and set it to match the manual.\n"
                   "Press Reset statistics and poll again to confirm.", ACT_RESET);
        break;
      case 3:
        addFinding(SEV_WARN, sid + ": illegal data value (exception 03)",
                   "The device rejected the quantity in the request, or a value being written.",
                   "Read at most 125 registers (FC03/FC04) or 2000 coils (FC01/FC02) per request; many devices accept far less - try 10-20.\n"
                   "32-bit values need an even quantity starting at the right register (e.g. 2 registers).\n"
                   "For writes, check the allowed range and units in the manual (scaling, decimals).\n"
                   "Press Reset statistics and poll again to confirm.", ACT_RESET);
        break;
      case 4:
        addFinding(SEV_FAULT, sid + ": device failure (exception 04)",
                   "The device reported an internal error while processing the request.",
                   "Check the device display/LEDs for alarms or a configuration error.\n"
                   "Power-cycle the device.\n"
                   "Check that the request is valid for the device's current operating mode (some registers can only be written when stopped).\n"
                   "If it persists, contact the device manufacturer with the request bytes shown under Recent transactions.");
        break;
      case 5:
      case 6:
        addFinding(SEV_INFO, sid + ": device busy (exception 05/06)",
                   "The device is busy with a long-running operation and asks the master to retry later.",
                   "Increase the poll interval for this device.\n"
                   "Enable retries in the master software.\n"
                   "Avoid writing to the device while it is executing a previous command.");
        break;
      case 10:
      case 11:
        addFinding(SEV_WARN, sid + ": gateway error (exception 0A/0B)",
                   "This ID belongs to a Modbus gateway. 0A: it has no route to the target. 0B: the target behind it did not answer.",
                   "Check the gateway's routing table / unit-ID mapping for this ID.\n"
                   "Check that the downstream device is powered and connected to the gateway.\n"
                   "Increase the master timeout - requests through a gateway take longer.");
        break;
      default:
        addFinding(SEV_INFO, sid + ": exception " + String(code),
                   String(excName(code)) + " reported by the device.",
                   "Look up exception code " + String(code) + " in the device manual.");
        break;
    }
  }
}

// Settings the bus is being read at: the sniffer's while sniffing, else the PC's.
String busSettingsText() {
  if (bridgeMode == MODE_SNIFF) {
    ScanCombo c;
    portENTER_CRITICAL(&sniffMux);
    c = sniffStatus.combo;
    portEXIT_CRITICAL(&sniffMux);
    LineCoding lc = { c.baud, 8, c.parity, c.stopBits };
    return lineCodingText(lc);
  }
  return lineCodingText(activeCoding);
}

void runDiagnosis() {
  takeSnapshot();
  findingCount = 0;
  overallSev = SEV_OK;

  String line = busSettingsText();
  bool hostOpen = (bool)Serial;
  uint8_t mode = bridgeMode;
  uint8_t il = idleLevel;

  if (mode == MODE_SCAN) {
    addFinding(SEV_INFO, "Auto-scan in progress",
               "The dongle owns the bus. Bytes from the PC are discarded until the scan ends.",
               "Wait for the scan to finish, or stop it on the Auto-scan page.", ACT_SCAN);
  } else if (mode == MODE_CONSOLE) {
    addFinding(SEV_INFO, "USB console mode",
               "The PC opened the port at " + String(CONSOLE_BAUD) + " baud, so nothing is bridged.",
               "Close the serial terminal.\nOpen the COM port again at the bus baud rate in your Modbus master.");
  } else if (mode == MODE_SNIFF) {
    uint8_t sst;
    portENTER_CRITICAL(&sniffMux);
    sst = sniffStatus.state;
    portEXIT_CRITICAL(&sniffMux);
    if (sst == SNIFF_DETECTING) {
      addFinding(SEV_INFO, "Sniffer: detecting bus settings",
                 "Listening at each common baud rate and format until valid Modbus frames are seen (currently " + line + ").",
                 "Make sure the other master is polling - detection needs traffic.\n"
                 "If nothing is detected after a full pass, check the A/B wiring at the dongle, or start the sniffer with the settings chosen manually.", ACT_SNIFF);
    } else {
      addFinding(SEV_INFO, "Sniffer mode (listen-only) at " + line,
                 "The dongle never transmits. The statistics and findings on this page describe the traffic of the master already on the bus.",
                 "Open the Sniffer page to see the decoded frames.\n"
                 "Stop the sniffer there to use the dongle as a USB converter again.", ACT_SNIFF);
    }
  } else if (!hostOpen) {
    addFinding(SEV_INFO, "PC port not open",
               "No application has the COM port open.",
               "Open the dongle's COM port in your Modbus master software.\n"
               "If the port is missing on the PC, re-plug the USB cable and check Device Manager (Windows) or /dev/ttyACM* (Linux).");
  }

  // ---- physical layer ----
  if (il == IDLE_LOW) {
    addFinding(SEV_FAULT, "RS485 bus idles LOW",
               "Between frames the receiver reads a constant 0. Usual causes: A and B swapped, or no fail-safe bias on the bus.",
               "Swap the A and B wires at the dongle (labelling differs between vendors: A/B, D+/D-, +/-).\n"
               "Press Reset statistics and check the idle level - it should read HIGH within a few seconds.\n"
               "If it stays LOW with nothing connected except the dongle, check the dongle's transceiver wiring (RO to D2, DE/RE to D4).\n"
               "If it is LOW only with the bus connected: " STEPS_BIAS, ACT_RESET);
  } else if (il == IDLE_UNSTABLE) {
    addFinding(SEV_WARN, "Idle line level unstable",
               "The bus floats between frames and picks up noise, so receivers can see false start bits.",
               STEPS_BIAS "\n" STEPS_NOISE, ACT_RESET);
  }
  if (snapG.uartBreaks > 0 && il != IDLE_LOW) {
    addFinding(SEV_WARN, "Line held LOW (" + String(snapG.uartBreaks) + " break event(s))",
               "The bus was held at 0 for longer than a character: a swapped pair, a short, or a device whose transmitter stays on.",
               "Check A/B polarity at every device.\n"
               "Measure A to B with a multimeter while the bus is idle: +0.3 to +0.8 V is acceptable with the 330 \u03A9 bias (typically about +0.7 V). Below +0.2 V the receivers cannot tell idle from data; near 0 V means no bias; negative means A/B swapped or a device driving the bus.\n"
               "Disconnect devices one at a time to find one that holds the bus.\n"
               "Press Reset statistics and watch whether break events return.", ACT_RESET);
  }

  uint32_t total = snapG.requests;
  if (total == 0 && snapG.broadcasts == 0 && mode == MODE_SNIFF && snapG.rxBytes > 0) {
    addFinding(SEV_WARN, "Sniffer: no master requests decoded",
               "Bytes are arriving at " + line + ", but none of them form a valid Modbus RTU request.",
               "The baud rate or parity is probably wrong: restart the sniffer with Auto-detect.\n"
               "If the Sniffer page shows only GARBAGE rows at every setting, check A/B polarity at the dongle.\n"
               "The bus may carry Modbus ASCII or another protocol, which this sniffer does not decode.", ACT_SNIFF);
  }
  if (total == 0 && snapG.nonRtuRequests == 0 && snapG.broadcasts == 0 && mode == MODE_BRIDGE) {
    addFinding(SEV_INFO, "No Modbus traffic yet",
               "The PC hasn't sent any Modbus RTU request through the dongle since the statistics were reset.",
               "Start polling from your Modbus master on this COM port (currently " + line + ").\n"
               "Or run Auto-scan to find the devices on the bus first.", ACT_SCAN);
  }

  // ---- per slave ----
  uint32_t worstLatUs = 0;
  int slavesAnswering = 0;
  uint32_t answered = 0;
  bool garbledSeen = (snapG.uartFrameErrors + snapG.uartParityErrors) > 0;

  for (int id = 1; id < SLAVE_SLOTS; id++) {
    const SlaveStats &s = snapSlaves[id];
    if (s.requests == 0) continue;

    uint32_t replies = s.ok + s.exceptions + s.badFunction;
    answered += s.ok + s.exceptions;
    if (replies) slavesAnswering++;
    if (s.latMaxUs > worstLatUs) worstLatUs = s.latMaxUs;
    String sid = "Slave " + String(id);

    if (id > 247) {
      addFinding(SEV_WARN, sid + ": reserved address",
                 "IDs 248-255 are reserved by the Modbus specification; most devices cannot use them.",
                 "Configure the device and the master to use an ID between 1 and 247.");
    }

    if (replies == 0 && s.requests >= 3) {
      if (s.lateReplies > 0) {
        addFinding(SEV_FAULT, sid + ": every reply arrives too late",
                   "The device does answer (" + String(s.lateReplies) + " late reply(ies)), but only after the reply timeout of " + String(diagResponseTimeoutMs) + " ms, so every request counts as a timeout.",
                   "Increase the master's response timeout (try 1000 ms, then reduce it step by step).\n"
                   "Set the dongle's diagnostic reply timeout (Settings) to the same value.\n"
                   "Make the poll interval longer than the response timeout.\n"
                   "If the device is behind a gateway or radio link, allow for its extra delay.\n"
                   "Press Reset statistics and poll again: the finding should change to Bus healthy, with a recommended timeout.", ACT_SETTINGS);
      } else if (s.crcErrors > 0) {
        addFinding(SEV_FAULT, sid + ": every reply is corrupted",
                   "Bytes come back but never as a valid frame. This is almost always a baud rate / parity / stop-bit mismatch (the PC port is set to " + line + ").",
                   STEPS_SETTINGS_MISMATCH(line) + "\nIf the settings definitely match, treat it as noise: " STEPS_NOISE, ACT_SCAN);
      } else if (s.wrongSlave > 0) {
        addFinding(SEV_FAULT, sid + ": a different device answers",
                   "Valid replies come back, but from another slave ID - the device is configured with a different address than the master expects.",
                   "Look at Recent transactions: the first byte of the reply is the ID the device actually uses.\n"
                   "Either poll that ID, or change the device's address to " + String(id) + ".\n"
                   "Make sure no two devices on the bus share an ID.\n"
                   "Press Reset statistics and poll again to confirm.", ACT_RESET);
      } else if (garbledSeen) {
        addFinding(SEV_FAULT, sid + " never answers",
                   "No reply to " + String(s.requests) + " request(s), and garbled bytes were seen on the bus: a device is probably talking at different settings than " + line + ".",
                   STEPS_SETTINGS_MISMATCH(line), ACT_SCAN);
      } else {
        addFinding(SEV_FAULT, sid + " never answers",
                   "No reply at all to " + String(s.requests) + " request(s) at " + line + ".",
                   "Check the device is powered and its communication LED (if any) blinks when polled.\n"
                   "Check the slave ID configured in the device matches ID " + String(id) + ".\n"
                   "Check the device's baud rate, parity and stop bits match " + line + ".\n"
                   "Check the wiring: A to A, B to B (swap A/B if unsure - it does no harm), and a common GND.\n"
                   "Check the device's Modbus/RS485 port is enabled and set to Modbus RTU slave (not ASCII, not master).\n"
                   "If the ID or settings are unknown, close the master and run Auto-scan.", ACT_SCAN);
      }
      continue;
    }

    if (replies > 0 && s.consecTimeouts >= diagDeadSlaveThreshold) {
      uint32_t ago = (millis() - s.lastReplyMs) / 1000;
      addFinding(SEV_FAULT, sid + " stopped responding",
                 "It answered " + String(s.okBeforeOutage) + " request(s), but the last " + String(s.consecTimeouts) + " timed out (last good reply " + String(ago) + " s ago). Something changed at that device or its cable.",
                 "Check the device is still powered (supply, fuse, breaker) and not in an alarm/reboot loop.\n"
                 "Check the RS485 connector and cable spur to that device; follow the cable back to the last device that still answers.\n"
                 "Check nobody changed the device's ID or communication settings.\n"
                 "Power-cycle the device.\n"
                 "When it answers again, the finding clears; press Reset statistics to clear the history.", ACT_RESET);
    } else if (s.requests >= 20) {
      float toPct = 100.0f * s.timeouts / s.requests;
      if (toPct >= 2.0f) {
        if (s.lateReplies > 0) {
          addFinding(toPct >= 20.0f ? SEV_FAULT : SEV_WARN, sid + ": intermittent timeouts",
                     String(toPct, 1) + "% of requests got no reply in time, and " + String(s.lateReplies) + " reply(ies) arrived later: the device answers, but slower than the timeout.",
                     "Set the master's response timeout to at least " + String(recommendTimeoutMs(s.latMaxUs)) + " ms.\n"
                     "Set the dongle's diagnostic reply timeout (Settings) to the same value so both agree.\n"
                     "Make the poll interval longer than the response timeout.\n"
                     "Press Reset statistics and poll again to confirm.", ACT_SETTINGS);
        } else {
          addFinding(toPct >= 20.0f ? SEV_FAULT : SEV_WARN, sid + ": intermittent timeouts",
                     String(toPct, 1) + "% of requests got no reply: occasional lost frames from noise, a busy device, or polling too fast.",
                     "Increase the poll interval (some devices need 50-100 ms between requests).\n"
                     "Enable 1-2 retries in the master software.\n"
                     "Increase the master's response timeout to " + String(recommendTimeoutMs(s.latMaxUs)) + " ms or more.\n" STEPS_NOISE, ACT_RESET);
        }
      }
    }

    if (s.requests >= 20) {
      float crcPct = 100.0f * s.crcErrors / s.requests;
      if (crcPct >= 1.0f) {
        addFinding(crcPct >= 10.0f ? SEV_FAULT : SEV_WARN, sid + ": " + String(crcPct, 1) + "% corrupted replies",
                   "Some replies arrive damaged: electrical noise, signal reflections or a weak signal.",
                   STEPS_NOISE, ACT_RESET);
      }
    }

    if (s.wrongSlave > 0) {
      addFinding(SEV_WARN, sid + ": replies from another ID",
                 String(s.wrongSlave) + " reply(ies) came from a different slave ID: two devices share an address, or a slow device answered an earlier request after the master had moved on.",
                 "Check every device on the bus has a unique ID (run Auto-scan with the master stopped - duplicates show as garbled).\n"
                 "Increase the master's response timeout to " + String(recommendTimeoutMs(worstLatUs)) + " ms so late replies don't overlap the next request.\n"
                 "Press Reset statistics and poll again to confirm.", ACT_SCAN);
    }
    if (s.badFunction > 0) {
      addFinding(SEV_WARN, sid + ": unexpected function code in reply",
                 String(s.badFunction) + " reply(ies) carried a different function code than the request - usually a device or gateway firmware issue.",
                 "Check the device firmware version and manual for known Modbus issues.\n"
                 "If the device sits behind a gateway, check the gateway's protocol settings.");
    }

    diagnoseExceptions(sid, s);

    if (s.latCount >= 3 && s.latMaxUs > 200000) {
      addFinding(SEV_INFO, sid + ": slow responder",
                 "Replies take " + msText(s.latSumUs / s.latCount) + " ms on average, up to " + msText(s.latMaxUs) + " ms.",
                 "Set the master's response timeout to at least " + String(recommendTimeoutMs(s.latMaxUs)) + " ms.\n"
                 "Keep the poll interval above that value.\n"
                 "Read larger blocks less often instead of many small requests.", ACT_SETTINGS);
    }
  }

  // ---- bus-wide ----
  if (snapG.foreignFrames > 0 && mode != MODE_SNIFF) {
    addFinding(SEV_WARN, "Another device is talking on the bus",
               String(snapG.foreignFrames) + " valid Modbus frame(s) appeared that were not replies to requests sent through this dongle - another master (PLC, HMI, gateway, meter reader) is polling the same bus.",
               "Find the other master: look at Recent transactions (FOREIGN rows) to see which IDs it polls.\n"
               "Stop or disconnect it while you use this dongle - two masters on one RS485 segment cause collisions.\n"
               "If both must run, put the dongle on a separate segment or use the other master's pass-through/gateway function.\n"
               "Press Reset statistics and confirm FOREIGN frames stop.", ACT_RESET);
  }
  if (snapG.glitchBytes > 0) {
    addFinding(SEV_WARN, "Stray byte in front of replies",
               String(snapG.glitchBytes) + " reply(ies) started with an extra byte (often 0x00 or 0xFF) produced when a transceiver switches with no fail-safe bias. Many masters report these replies as CRC errors.",
               STEPS_BIAS, ACT_RESET);
  }
  if (snapG.noiseBytes >= 10 && (uint64_t)snapG.noiseBytes * 100 >= (uint64_t)max(snapG.rxBytes, (uint32_t)1) * 2) {
    addFinding(SEV_WARN, "Noise between frames",
               String(snapG.noiseBytes) + " received byte(s) belonged to no frame.",
               STEPS_NOISE "\nAlso check that no device on the bus is set to a different baud rate (run Auto-scan with the master stopped).", ACT_RESET);
  }
  if (garbledSeen) {
    addFinding(SEV_WARN, "UART framing/parity errors",
               String(snapG.uartFrameErrors) + " framing and " + String(snapG.uartParityErrors) + " parity error(s) at " + line + ": a device uses different settings, or the signal is distorted.",
               "If a device never answers, it is most likely a settings mismatch: " + STEPS_SETTINGS_MISMATCH(line) + "\nIf all devices answer, treat it as noise: " STEPS_NOISE, ACT_SCAN);
  }
  if (snapG.uartOverflows + snapG.usbOverflows > 0) {
    addFinding(SEV_WARN, "Data overflow",
               "Data arrived faster than it could be passed on (" + String(snapG.uartOverflows) + " RS485, " + String(snapG.usbOverflows) + " USB): the PC application is not reading the port quickly enough.",
               "Close other programs that may hold or poll the COM port.\n"
               "Avoid sending many requests back-to-back without waiting for replies.\n"
               "Try a different USB port/cable (avoid unpowered hubs).", ACT_RESET);
  }
  if (snapG.nonRtuRequests > 0) {
    if (total == 0) {
      addFinding(SEV_INFO, "PC traffic is not Modbus RTU",
                 String(snapG.nonRtuRequests) + " request(s) had no valid RTU CRC - probably Modbus ASCII or another protocol. Data still passes through unchanged; the diagnostics only cover Modbus RTU.",
                 "If you intended Modbus RTU, set the master's protocol to 'Modbus RTU' (not ASCII, not RTU-over-TCP).");
    } else {
      addFinding(SEV_WARN, "Some PC requests had a bad CRC",
                 String(snapG.nonRtuRequests) + " request(s) from the PC were not valid RTU frames.",
                 "Set the master's protocol to 'Modbus RTU' (not ASCII or RTU-over-TCP).\n"
                 "Close other programs that might write to the same COM port.\n"
                 "Press Reset statistics and poll again to confirm.", ACT_RESET);
    }
  }
  if (snapG.scanDroppedBytes > 0 && mode != MODE_SCAN) {
    addFinding(SEV_INFO, "PC data was discarded during a scan",
               String(snapG.scanDroppedBytes) + " byte(s) from the PC arrived while Auto-scan owned the bus and were dropped.",
               "Stop the Modbus master before starting Auto-scan.\nPress Reset statistics to clear this note.", ACT_RESET);
  }

  recommendedTimeoutMs = recommendTimeoutMs(worstLatUs);
  if (overallSev <= SEV_INFO && total > 0) {
    float pct = 100.0f * answered / total;
    addFinding(SEV_OK, "Bus healthy",
               String(pct, 1) + "% of " + String(total) + " request(s) answered by " + String(slavesAnswering) + " slave(s); slowest reply " + msText(worstLatUs) + " ms.",
               "No action needed.\nRecommended master response timeout: " + String(recommendedTimeoutMs) + " ms.");
  }

  healthLevel = (overallSev >= SEV_WARN) ? overallSev : (uint8_t)SEV_OK;
}

String hexBytes(const uint8_t *p, uint16_t len, uint16_t cap) {
  String s = "";
  uint16_t n = min(len, cap);
  for (uint16_t i = 0; i < n; i++) {
    char b[4];
    snprintf(b, sizeof(b), "%02X", p[i]);
    if (i) s += ' ';
    s += b;
  }
  if (len > cap) s += " ...";
  return s;
}

String idleLevelText(uint8_t il) {
  switch (il) {
    case IDLE_HIGH: return "HIGH (correct)";
    case IDLE_LOW: return "LOW - check A/B / bias";
    case IDLE_UNSTABLE: return "UNSTABLE - floating / noisy";
    default: return "not measured yet";
  }
}

String modeText(uint8_t m) {
  switch (m) {
    case MODE_SCAN: return "Auto-scan";
    case MODE_CONSOLE: return "USB console";
    case MODE_SNIFF: return "Sniffer (listen-only)";
    default: return "Bridging";
  }
}

// ===================== Shared page chrome =====================
const char PAGE_CSS[] PROGMEM = R"rawliteral(
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: Arial; background:#f5f7fa; margin:15px; }
h2 { color:#00a5df; margin:4px 0 10px; }
h3 { color:#087b7b; margin:4px 0 8px; font-size:16px; }
.box { background:white; padding:12px; margin-bottom:15px; box-shadow:0 1px 4px rgba(0,0,0,0.2); overflow-x:auto; }
table { width:100%; border-collapse:collapse; font-size:13px; }
th { background:#087b7b; color:white; padding:6px; }
td { border:1px solid #2999c5; padding:5px; text-align:center; }
td.l { text-align:left; }
button { padding:8px 14px; background:#0099cc; color:white; border:none; margin:2px; border-radius:4px; cursor:pointer; }
button:disabled { background:#9bbfcc; cursor:default; }
.nav { background:#6f42c1; }
.danger { background:#dc3545; }
input, select { padding:6px; margin:3px 0; }
input[type=text], input[type=password], input[type=number] { width:220px; max-width:95%; }
.banner { background:#f8d7da; color:#721c24; padding:10px; margin:10px 0; }
.ok { background:#d4edda; color:#155724; padding:8px; margin:8px 0; }
.err { background:#f8d7da; color:#721c24; padding:8px; margin:8px 0; }
.note { color:#666; font-size:12px; }
.grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(150px,1fr)); gap:6px; }
.kv { background:#f0f6f9; padding:6px; border-radius:4px; font-size:13px; }
.kv b { display:block; font-size:16px; color:#087b7b; }
.health { padding:12px; color:white; font-size:20px; font-weight:bold; border-radius:4px; }
.h-OK { background:#28a745; } .h-WARN { background:#e0a800; } .h-FAULT { background:#dc3545; } .h-IDLE { background:#6c757d; }
.f { border-left:6px solid #6c757d; padding:6px 10px; margin:6px 0; background:#fafafa; }
.f-FAULT { border-color:#dc3545; } .f-WARN { border-color:#e0a800; } .f-INFO { border-color:#17a2b8; } .f-OK { border-color:#28a745; }
.f .t { font-weight:bold; }
.f .d { font-size:13px; color:#333; margin-top:3px; }
.f ol { margin:4px 0 4px 18px; padding:0; } .f li { margin:2px 0; }
.f button { padding:5px 10px; font-size:12px; }
.r-OK { color:#155724; } .r-TIMEOUT, .r-CRCLENGTH, .r-FOREIGN, .r-NOTRTU { color:#dc3545; font-weight:bold; } .r-EXCEPTION, .r-WRONGID, .r-LATEREPLY, .r-BADFC { color:#b8860b; font-weight:bold; }
.mono { font-family:monospace; font-size:12px; text-align:left; word-break:break-all; }
.logbox { background:#111; color:#0f0; font-family:monospace; font-size:12px; padding:10px; max-height:220px; overflow-y:auto; border-radius:4px; }
.bar { background:#e9ecef; height:18px; border-radius:4px; overflow:hidden; }
.bar div { background:#0099cc; height:100%; width:0; }
</style>
)rawliteral";

String pageHead(const String &title) {
  String html = "<!DOCTYPE html><html><head><title>" + htmlEscape(title) + " - Modbus Dongle</title>";
  html += FPSTR(PAGE_CSS);
  html += "</head><body>";
  return html;
}

String navBar() {
  String html = "<h2>Modbus Dongle</h2>";
  html += "<button class='nav' type='button' onclick=\"location.href='/'\">Diagnostics</button>";
  html += "<button class='nav' type='button' onclick=\"location.href='/scan'\">Auto-scan</button>";
  html += "<button class='nav' type='button' onclick=\"location.href='/sniff'\">Sniffer</button>";
  html += "<button class='nav' type='button' onclick=\"location.href='/settings'\">Settings</button>";
  html += "<button class='danger' type='button' onclick=\"location.href='/logout'\">Log Out</button>";
  if (webAuthUsingDefaults()) {
    html += "<div class='banner'>Default web login in use - change it under Settings &rarr; Security.</div>";
  }
  return html;
}

// ===================== Login / Logout =====================
void handleLoginPage() {
  String html = pageHead("Login");
  html += "<div class='box' style='max-width:360px;margin:60px auto;padding:20px'><h2>Modbus Dongle</h2>";
  if (server.hasArg("locked")) {
    html += "<div class='err'>Too many failed attempts - locked for " + String(WEB_LOGIN_LOCKOUT_MS / 1000) + " seconds.</div>";
  } else if (server.hasArg("err")) {
    html += "<div class='err'>Invalid username or password.</div>";
  }
  html += R"rawliteral(
<form action="/login" method="POST">
<input type="text" name="user" placeholder="Username" autofocus style="width:95%">
<input type="password" name="pass" placeholder="Password" style="width:95%">
<button type="submit" style="width:100%">Log In</button>
</form>
<p class="note">Locked out? Connect USB, open the dongle's COM port at <b>74880</b> baud in a serial terminal, and send <b>factory-reset-auth</b> to restore the default login and AP password without losing other settings.</p>
</div></body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleLoginPost() {
  if (webLoginLockedOut()) {
    server.sendHeader("Location", "/login?locked=1");
    server.send(303);
    return;
  }

  String user = server.arg("user");
  String pass = server.arg("pass");

  if (checkWebCredentials(user, pass)) {
    webSessionToken = makeRandomHex(16);
    webSessionLastActivity = millis();
    webLoginFailCount = 0;
    logKeyEvent("WEB LOGIN OK: %s", user.c_str());
    server.sendHeader("Set-Cookie", "DGSESSION=" + webSessionToken + "; Path=/; HttpOnly");
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  webLoginFailCount++;
  if (webLoginFailCount >= WEB_LOGIN_MAX_FAILS) {
    webLoginLockStart = millis();
    logKeyEvent("WEB LOGIN LOCKED OUT (%lus after %d failures)", WEB_LOGIN_LOCKOUT_MS / 1000, webLoginFailCount);
    server.sendHeader("Location", "/login?locked=1");
  } else {
    logKeyEvent("WEB LOGIN FAILED (attempt %d/%d)", webLoginFailCount, WEB_LOGIN_MAX_FAILS);
    server.sendHeader("Location", "/login?err=1");
  }
  server.send(303);
}

void handleLogout() {
  webSessionToken = "";
  server.sendHeader("Set-Cookie", "DGSESSION=deleted; Path=/; Max-Age=0");
  server.sendHeader("Location", "/login");
  server.send(303);
}

// ===================== Diagnostics dashboard =====================
const char DASHBOARD_BODY[] PROGMEM = R"rawliteral(
<div class="box"><div id="health" class="health h-IDLE">Loading...</div><div id="findings"></div>
<p class="note" id="conn"></p></div>

<div class="box"><h3>Link</h3><div class="grid" id="link"></div></div>

<div class="box"><h3>Bus totals <span class="note" id="since"></span></h3><div class="grid" id="totals"></div>
<p><button class="danger" type="button" onclick="resetStats()">Reset statistics</button></p></div>

<div class="box"><h3>Slaves</h3>
<table><thead><tr><th>ID</th><th>Requests</th><th>OK</th><th>Success</th><th>Timeouts</th><th>CRC</th><th>Exceptions</th><th>Wrong ID</th><th>Late</th><th>Latency min / avg / max (ms)</th><th>Last reply</th><th>Status</th></tr></thead>
<tbody id="slaves"></tbody></table></div>

<div class="box"><h3>Recent transactions (newest first)</h3>
<table><thead><tr><th>Age</th><th>ID</th><th>FC</th><th>Result</th><th>Latency (ms)</th><th>Request</th><th>Reply</th></tr></thead>
<tbody id="txlog"></tbody></table></div>

<div class="box"><h3>Status Log</h3><div class="logbox" id="events"></div></div>

<p class="note" id="footer"></p>

<script>
const $=id=>document.getElementById(id);
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
const LBL={FAULT:['Likely cause','Remedy'],WARN:['Likely cause','Remedy'],INFO:['Details','What to do'],OK:['Summary','Notes']};
const ACT={sniff:['Open Sniffer',"location.href='/sniff'"],scan:['Open Auto-scan',"location.href='/scan'"],reset:['Reset statistics (after fixing)','resetStats()'],settings:['Open Settings',"location.href='/settings'"]};
function kv(label,val){return '<div class="kv">'+esc(label)+'<b>'+esc(val)+'</b></div>';}
function ago(s){if(s<0)return '-';if(s<60)return s+' s';if(s<3600)return Math.floor(s/60)+' min';return Math.floor(s/3600)+' h';}
function render(d){
  const h=$('health'); h.className='health h-'+d.health.level;
  h.textContent=({OK:'Bus healthy',WARN:'Warnings',FAULT:'Fault detected',IDLE:'Waiting for traffic'})[d.health.level]+'  ('+d.link.line+', '+d.link.mode+')';
  $('findings').innerHTML=d.health.findings.map(f=>'<div class="f f-'+f.sev+'"><div class="t">['+f.sev+'] '+esc(f.title)+'</div><div class="d"><b>'+LBL[f.sev][0]+':</b> '+esc(f.cause)+'</div>'+(f.remedy?'<div class="d"><b>'+LBL[f.sev][1]+':</b><ol>'+f.remedy.split('\n').map(x=>'<li>'+esc(x)+'</li>').join('')+'</ol></div>':'')+(ACT[f.action]?'<button type="button" onclick="'+ACT[f.action][1]+'">'+ACT[f.action][0]+'</button>':'')+'</div>').join('');
  $('conn').textContent='Recommended master response timeout: '+d.recTimeoutMs+' ms';
  const L=d.link;
  $('link').innerHTML=kv('PC port',L.usbOpen?'Open':'Closed')+kv('Port settings',L.line+(L.mode.indexOf('Sniffer')==0?' (sniffer)':(L.fromHost?' (from PC)':' (default)')))+kv('Mode',L.mode)+kv('Bus idle level',L.idle)+kv('Last bus activity',L.lastActivityS<0?'never':ago(L.lastActivityS)+' ago')+kv('Diag reply timeout',L.diagTimeoutMs+' ms')+kv('WiFi AP',d.dev.ap+' / '+d.dev.apIp)+kv('Site WiFi',d.dev.sta)+kv('Uptime',ago(d.uptimeS))+kv('Free heap',d.heap+' B');
  const T=d.totals;
  $('since').textContent='(since '+ago(T.sinceS)+' ago)';
  $('totals').innerHTML=[['Requests',T.requests],['Replies OK',T.ok],['Timeouts',T.timeouts],['CRC / length errors',T.crc],['Exceptions',T.exceptions],['Wrong slave ID',T.wrongSlave],['Late replies',T.late],['Broadcasts',T.broadcasts],['Foreign frames',T.foreign],['Noise bytes',T.noise],['Stray lead bytes',T.glitch],['Non-RTU requests',T.nonRtu],['UART framing err',T.frameErr],['UART parity err',T.parityErr],['Break events',T.breaks],['Overflows (RS485/USB)',T.uartOvf+' / '+T.usbOvf],['Bytes PC to bus',T.txBytes],['Bytes bus to PC',T.rxBytes]].map(x=>kv(x[0],x[1])).join('');
  $('slaves').innerHTML=d.slaves.length?d.slaves.map(s=>'<tr><td><b>'+s.id+'</b></td><td>'+s.req+'</td><td>'+s.ok+'</td><td>'+s.pct+'%</td><td>'+s.to+'</td><td>'+s.crc+'</td><td>'+s.exc+(s.excList?' ('+esc(s.excList)+')':'')+'</td><td>'+s.ws+'</td><td>'+s.late+'</td><td>'+(s.latN?s.latMin+' / '+s.latAvg+' / '+s.latMax:'-')+'</td><td>'+(s.lastS<0?'never':ago(s.lastS)+' ago')+'</td><td class="r-'+(s.status=='OK'?'OK':(s.status=='NO REPLY'||s.status=='DOWN'?'TIMEOUT':'EXCEPTION'))+'">'+esc(s.status)+'</td></tr>').join(''):'<tr><td colspan="12">No requests seen yet</td></tr>';
  $('txlog').innerHTML=d.log.length?d.log.map(t=>'<tr><td>'+ago(t.ageS)+'</td><td>'+t.id+'</td><td>'+t.fc+'</td><td class="r-'+t.res.replace(/[^A-Z]/g,'')+'">'+esc(t.res)+(t.exc?' '+t.exc:'')+'</td><td>'+(t.lat===''?'-':t.lat)+'</td><td class="mono">'+esc(t.req)+'</td><td class="mono">'+esc(t.rsp)+'</td></tr>').join(''):'<tr><td colspan="7">None yet</td></tr>';
  $('events').innerHTML=d.events.map(e=>'<div>['+ago(e.ageS)+' ago] '+esc(e.msg)+'</div>').join('');
  $('footer').textContent='Firmware '+d.fw+' - '+d.dev.host;
}
async function refresh(){
  try{
    const r=await fetch('/api/diag',{cache:'no-store'});
    if(r.status==401){location.href='/login';return;}
    render(await r.json());
  }catch(e){$('conn').textContent='Connection lost - retrying...';}
}
async function resetStats(){
  if(!confirm('Reset all statistics and the transaction log?'))return;
  const r=await fetch('/api/resetStats',{method:'POST'});
  if(r.status==401){location.href='/login';return;}
  refresh();
}
setInterval(refresh,2000);refresh();
</script>
</body></html>
)rawliteral";

void handleRoot() {
  if (!requireAuth()) return;
  String html = pageHead("Diagnostics");
  html += navBar();
  html += FPSTR(DASHBOARD_BODY);
  server.send(200, "text/html", html);
}

void handleApiDiag() {
  if (!requireAuthApi()) return;
  runDiagnosis();

  uint32_t nowMs = millis();
  String j;
  j.reserve(12000);

  j += "{\"fw\":\"" FW_VERSION "\",\"uptimeS\":" + String(nowMs / 1000);
  j += ",\"heap\":" + String(ESP.getFreeHeap());

  String sta = "not configured";
  if (wifiConfig.ssid.length()) {
    sta = (WiFi.status() == WL_CONNECTED) ? (wifiConfig.ssid + " / " + WiFi.localIP().toString() + " (" + String(WiFi.RSSI()) + " dBm)") : (wifiConfig.ssid + " - not connected");
  }
  j += ",\"dev\":{\"ap\":\"" + jsonEscape(deviceConfig.apSsid) + "\",\"apIp\":\"" + WiFi.softAPIP().toString() + "\",\"sta\":\"" + jsonEscape(sta) + "\",\"host\":\"" + hostnameString() + "\"}";

  int32_t lastAct = snapG.lastActivityMs ? (int32_t)((nowMs - snapG.lastActivityMs) / 1000) : -1;
  j += ",\"link\":{\"usbOpen\":" + String((bool)Serial ? "true" : "false");
  j += ",\"line\":\"" + busSettingsText() + "\",\"fromHost\":" + String((lineCodingFromHost && bridgeMode != MODE_SNIFF) ? "true" : "false");
  j += ",\"mode\":\"" + modeText(bridgeMode) + "\",\"idle\":\"" + idleLevelText(idleLevel) + "\"";
  j += ",\"lastActivityS\":" + String(lastAct) + ",\"diagTimeoutMs\":" + String(diagResponseTimeoutMs) + "}";

  // health
  const char *level = sevName(overallSev >= SEV_WARN ? overallSev : (uint8_t)SEV_OK);
  if (overallSev < SEV_WARN && snapG.requests == 0) level = "IDLE";
  j += ",\"health\":{\"level\":\"" + String(level) + "\",\"findings\":[";
  bool first = true;
  for (int sev = SEV_FAULT; sev >= SEV_OK; sev--) {
    for (int i = 0; i < findingCount; i++) {
      if (findings[i].sev != sev) continue;
      if (!first) j += ",";
      first = false;
      // Firmware-authored text; the page escapes it before display.
      static const char *actions[] = { "", "scan", "reset", "settings", "sniff" };
      j += "{\"sev\":\"" + String(sevName(sev)) + "\",\"title\":\"" + jsonEscape(findings[i].title) + "\",\"cause\":\"" + jsonEscape(findings[i].cause);
      j += "\",\"remedy\":\"" + jsonEscape(findings[i].remedy) + "\",\"action\":\"" + String(actions[findings[i].action < 5 ? findings[i].action : 0]) + "\"}";
    }
  }
  j += "]},\"recTimeoutMs\":" + String(recommendedTimeoutMs);

  // totals
  j += ",\"totals\":{\"requests\":" + String(snapG.requests) + ",\"ok\":" + String(snapG.ok) + ",\"timeouts\":" + String(snapG.timeouts);
  j += ",\"crc\":" + String(snapG.crcErrors) + ",\"exceptions\":" + String(snapG.exceptions) + ",\"wrongSlave\":" + String(snapG.wrongSlave);
  j += ",\"late\":" + String(snapG.lateReplies) + ",\"broadcasts\":" + String(snapG.broadcasts) + ",\"foreign\":" + String(snapG.foreignFrames);
  j += ",\"noise\":" + String(snapG.noiseBytes) + ",\"glitch\":" + String(snapG.glitchBytes) + ",\"nonRtu\":" + String(snapG.nonRtuRequests);
  j += ",\"frameErr\":" + String(snapG.uartFrameErrors) + ",\"parityErr\":" + String(snapG.uartParityErrors) + ",\"breaks\":" + String(snapG.uartBreaks);
  j += ",\"uartOvf\":" + String(snapG.uartOverflows) + ",\"usbOvf\":" + String(snapG.usbOverflows);
  j += ",\"txBytes\":" + String(snapG.txBytes) + ",\"rxBytes\":" + String(snapG.rxBytes);
  j += ",\"sinceS\":" + String((nowMs - snapG.statsSinceMs) / 1000) + "}";

  // slaves
  j += ",\"slaves\":[";
  first = true;
  for (int id = 1; id < SLAVE_SLOTS; id++) {
    const SlaveStats &s = snapSlaves[id];
    if (s.requests == 0 && s.lateReplies == 0) continue;
    if (!first) j += ",";
    first = false;
    uint32_t answered = s.ok + s.exceptions;
    String pct = s.requests ? String(100.0f * answered / s.requests, 1) : String("0");
    String status;
    if (answered + s.badFunction == 0) status = s.crcErrors ? "GARBLED" : "NO REPLY";
    else if (s.consecTimeouts >= diagDeadSlaveThreshold) status = "DOWN";
    else if (s.timeouts || s.crcErrors || s.wrongSlave) status = "UNSTABLE";
    else if (s.exceptions) status = "EXCEPTIONS";
    else status = "OK";
    String excList = "";
    for (uint8_t c = 1; c < 16; c++) {
      if (s.excMask & (1 << c)) {
        if (excList.length()) excList += ",";
        excList += String(c);
      }
    }
    j += "{\"id\":" + String(id) + ",\"req\":" + String(s.requests) + ",\"ok\":" + String(s.ok) + ",\"pct\":\"" + pct + "\"";
    j += ",\"to\":" + String(s.timeouts) + ",\"crc\":" + String(s.crcErrors) + ",\"exc\":" + String(s.exceptions) + ",\"excList\":\"" + excList + "\"";
    j += ",\"ws\":" + String(s.wrongSlave) + ",\"late\":" + String(s.lateReplies) + ",\"latN\":" + String(s.latCount);
    j += ",\"latMin\":\"" + msText(s.latMinUs) + "\",\"latAvg\":\"" + msText(s.latCount ? (uint32_t)(s.latSumUs / s.latCount) : 0) + "\",\"latMax\":\"" + msText(s.latMaxUs) + "\"";
    j += ",\"lastS\":" + String(s.lastReplyMs ? (int32_t)((nowMs - s.lastReplyMs) / 1000) : -1) + ",\"status\":\"" + status + "\"}";
  }
  j += "]";

  // transaction log, newest first
  j += ",\"log\":[";
  uint32_t n = min(snapLogTotal, (uint32_t)TXLOG_SIZE);
  for (uint32_t k = 0; k < n; k++) {
    const TxLogEntry &e = snapLog[(snapLogHead + TXLOG_SIZE - 1 - k) % TXLOG_SIZE];
    if (k) j += ",";
    bool hasLat = (e.result == TR_OK || e.result == TR_EXCEPTION || e.result == TR_BAD_FUNCTION);
    j += "{\"ageS\":" + String((nowMs - e.ms) / 1000) + ",\"id\":" + String(e.slave) + ",\"fc\":" + String(e.fc);
    j += ",\"res\":\"" + String(resultName(e.result)) + "\",\"exc\":" + String(e.excCode);
    j += ",\"lat\":\"" + (hasLat ? msText(e.latencyUs) : String("")) + "\"";
    j += ",\"req\":\"" + hexBytes(e.req, e.reqLen, TXLOG_REQ_BYTES) + "\",\"rsp\":\"" + hexBytes(e.rsp, e.rspLen, TXLOG_RSP_BYTES) + "\"}";
  }
  j += "]";

  // key event log, newest first
  j += ",\"events\":[";
  {
    char msgs[KEY_LOG_SIZE][KEY_LOG_MSG_LEN];
    uint32_t times[KEY_LOG_SIZE];
    uint8_t head, count;
    portENTER_CRITICAL(&keyLogMux);
    memcpy(msgs, keyLog, sizeof(msgs));
    memcpy(times, keyLogTime, sizeof(times));
    head = keyLogHead;
    count = keyLogCount;
    portEXIT_CRITICAL(&keyLogMux);
    for (uint8_t k = 0; k < count; k++) {
      uint8_t idx = (head + KEY_LOG_SIZE - 1 - k) % KEY_LOG_SIZE;
      if (k) j += ",";
      j += "{\"ageS\":" + String((nowMs - times[idx]) / 1000) + ",\"msg\":\"" + jsonEscape(String(msgs[idx])) + "\"}";
    }
  }
  j += "]}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleApiResetStats() {
  if (!requireAuthApi()) return;
  resetStatistics();
  logKeyEvent("STATISTICS RESET");
  server.send(200, "application/json", "{\"ok\":true}");
}

// ===================== Auto-scan page =====================
const char SCAN_BODY[] PROGMEM = R"rawliteral(
<div class="box"><h3>Auto-scan: find devices with unknown settings</h3>
<p class="note">The dongle takes over the bus and probes every slave ID with a "read 1 register" request at each selected baud rate and format. Any valid reply (data or exception) counts as a device found. <b>Stop your Modbus master software first</b> - data from the PC is discarded while the scan runs. The PC's port settings are restored afterwards.</p>
<table style="width:auto"><tr><td class="l">Slave IDs</td><td class="l"><input type="number" id="idFrom" min="1" max="247" value="1" style="width:70px"> to <input type="number" id="idTo" min="1" max="247" value="247" style="width:70px"></td></tr>
<tr><td class="l">Probe</td><td class="l"><select id="fc"><option value="3">FC03 Read Holding Register</option><option value="4">FC04 Read Input Register</option></select> address <input type="number" id="addr" min="0" max="65535" value="0" style="width:90px"></td></tr>
<tr><td class="l">Reply timeout per probe</td><td class="l"><input type="number" id="timeout" min="20" max="2000" value="100" style="width:80px"> ms</td></tr>
<tr><td class="l">Baud rates</td><td class="l" id="bauds"></td></tr>
<tr><td class="l">Formats</td><td class="l" id="fmts"></td></tr>
<tr><td class="l">Smart stop</td><td class="l"><label><input type="checkbox" id="stopFirst" checked> Stop after the first baud/format that finds devices (finishes that setting's ID range)</label></td></tr></table>
<p id="estimate" class="note"></p>
<button type="button" id="startBtn" onclick="startScan()">Start scan</button>
<button type="button" class="danger" id="stopBtn" onclick="stopScan()" disabled>Stop</button>
<div id="msg"></div></div>

<div class="box"><h3>Progress</h3><div class="bar"><div id="bar"></div></div><p id="prog" class="note"></p></div>

<div class="box"><h3>Devices found</h3>
<table><thead><tr><th>Slave ID</th><th>Baud</th><th>Format</th><th>Reply</th><th>Latency (ms)</th></tr></thead><tbody id="results"></tbody></table>
<div id="hints"></div></div>

<script>
const $=id=>document.getElementById(id);
const BAUDS=[9600,19200,38400,115200,57600,4800,2400,1200];
const BAUD_DEF=[9600,19200,38400,115200];
const FMTS=[['N1','8N1'],['E1','8E1'],['O1','8O1'],['N2','8N2']];
const FMT_DEF=['N1','E1'];
$('bauds').innerHTML=BAUDS.map(b=>'<label style="margin-right:8px"><input type="checkbox" name="b" value="'+b+'"'+(BAUD_DEF.includes(b)?' checked':'')+'> '+b+'</label>').join('');
$('fmts').innerHTML=FMTS.map(f=>'<label style="margin-right:8px"><input type="checkbox" name="f" value="'+f[0]+'"'+(FMT_DEF.includes(f[0])?' checked':'')+'> '+f[1]+'</label>').join('');
function sel(n){return [...document.querySelectorAll('input[name='+n+']:checked')].map(x=>x.value);}
function est(){
  const ids=Math.max(0,(+$('idTo').value)-(+$('idFrom').value)+1), c=sel('b').length*sel('f').length, t=+$('timeout').value;
  const s=Math.round(ids*c*(t+15)/1000);
  $('estimate').textContent=c+' setting(s) x '+ids+' ID(s): worst case about '+(s<120?s+' s':Math.round(s/60)+' min')+(c>32?' - too many settings (max 32)':'');
}
document.querySelectorAll('input').forEach(i=>i.addEventListener('change',est));est();
async function post(url,body){
  const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body||''});
  if(r.status==401){location.href='/login';throw 0;}
  return r.json();
}
async function startScan(){
  const p=new URLSearchParams({idFrom:$('idFrom').value,idTo:$('idTo').value,fc:$('fc').value,addr:$('addr').value,timeout:$('timeout').value,bauds:sel('b').join(','),fmts:sel('f').join(','),stopFirst:$('stopFirst').checked?1:0});
  const d=await post('/api/scan/start',p.toString());
  $('msg').innerHTML=d.ok?'':'<div class="err">'+d.error+'</div>';
  poll();
}
async function stopScan(){await post('/api/scan/stop');poll();}
function fmtName(f){return '8'+f;}
async function poll(){
  let d;
  try{const r=await fetch('/api/scan',{cache:'no-store'});if(r.status==401){location.href='/login';return;}d=await r.json();}catch(e){return;}
  const running=d.state=='RUNNING';
  $('startBtn').disabled=running;$('stopBtn').disabled=!running;
  const pct=d.total?Math.round(100*d.done/d.total):0;
  $('bar').style.width=(d.state=='DONE'?100:pct)+'%';
  $('prog').textContent=d.state=='IDLE'?'No scan run yet.':(d.state+': '+d.done+' / '+d.total+' probes'+(running?' - now ID '+d.id+' at '+d.combo:'')+' - '+d.elapsedS+' s'+(d.dropped?' - '+d.dropped+' PC byte(s) discarded':''));
  $('results').innerHTML=d.results.length?d.results.map(r=>'<tr><td><b>'+r.id+'</b></td><td>'+r.baud+'</td><td>'+r.fmt+'</td><td>'+(r.exc?'Exception '+r.exc+' (device present)':'Data')+'</td><td>'+r.lat+'</td></tr>').join(''):'<tr><td colspan="5">'+(d.state=='IDLE'?'-':'None yet')+'</td></tr>';
  let h='';
  if(d.garbled.length) h+='<div class="f f-WARN"><div class="t">Garbled replies at: '+d.garbled.join(', ')+'</div><div class="d">Something answered at these settings but no valid frame was decoded. A device is probably present at a nearby baud rate or a different parity/stop setting: include more baud rates/formats, or check for noise.</div></div>';
  if(!running&&d.state!='IDLE'&&d.results.length==0) h+='<div class="f f-FAULT"><div class="t">No device answered</div><div class="d">Check power and A/B wiring (try swapping A and B), make sure no other master is polling, widen the ID range and add the remaining baud rates and formats. Some devices only answer FC04 or reject address 0 - an exception reply still counts as found.</div></div>';
  if(d.results.length){const s=new Set(d.results.map(r=>r.baud+' '+r.fmt));h+='<div class="f f-OK"><div class="t">Use these port settings in your master: '+[...s].join(' / ')+'</div><div class="d">Open the COM port with these settings and poll the IDs listed above.</div></div>';}
  $('hints').innerHTML=h;
  if(running)setTimeout(poll,1000);
}
poll();
</script>
</body></html>
)rawliteral";

void handleScanPage() {
  if (!requireAuth()) return;
  String html = pageHead("Auto-scan");
  html += navBar();
  html += FPSTR(SCAN_BODY);
  server.send(200, "text/html", html);
}

String comboText(const ScanCombo &c) {
  String s = "8";
  s += (c.parity == 1) ? 'O' : (c.parity == 2 ? 'E' : 'N');
  s += (c.stopBits ? "2" : "1");
  return s;
}

void sendScanError(const String &msg) {
  server.send(200, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(msg) + "\"}");
}

void handleApiScanStart() {
  if (!requireAuthApi()) return;

  if (bridgeMode == MODE_SCAN || scanStartPending) return sendScanError("A scan is already running.");
  if (bridgeMode == MODE_CONSOLE) return sendScanError("The PC has the port open in console mode (74880 baud).");
  if (bridgeMode == MODE_SNIFF || sniffStartPending) return sendScanError("The sniffer is running - stop it on the Sniffer page first (auto-scan transmits on the bus).");

  long idFrom = server.arg("idFrom").toInt();
  long idTo = server.arg("idTo").toInt();
  long fc = server.arg("fc").toInt();
  long addr = server.arg("addr").toInt();
  long timeoutMs = server.arg("timeout").toInt();
  if (idFrom < 1 || idTo > 247 || idFrom > idTo) return sendScanError("Slave ID range must be within 1-247.");
  if (fc != 3 && fc != 4) return sendScanError("Probe function must be 3 or 4.");
  if (addr < 0 || addr > 65535) return sendScanError("Register address must be 0-65535.");
  if (timeoutMs < 20 || timeoutMs > 2000) return sendScanError("Timeout must be 20-2000 ms.");

  // Baud-major order, common settings first (the page sends them that way).
  ScanRequest req;
  memset(&req, 0, sizeof(req));
  String bauds = server.arg("bauds") + ",";
  String fmts = server.arg("fmts");
  int start = 0;
  while (true) {
    int comma = bauds.indexOf(',', start);
    if (comma < 0) break;
    long baud = bauds.substring(start, comma).toInt();
    start = comma + 1;
    if (baud < MIN_BAUD || baud > MAX_BAUD) continue;
    const char *codes[] = { "N1", "E1", "O1", "N2" };
    const uint8_t par[] = { 0, 2, 1, 0 };
    const uint8_t stop[] = { 0, 0, 0, 2 };
    for (int f = 0; f < 4; f++) {
      if (fmts.indexOf(codes[f]) < 0) continue;
      if (req.comboCount >= SCAN_MAX_COMBOS) return sendScanError("Too many baud/format combinations (max 32).");
      req.combos[req.comboCount].baud = baud;
      req.combos[req.comboCount].parity = par[f];
      req.combos[req.comboCount].stopBits = stop[f];
      req.comboCount++;
    }
  }
  if (req.comboCount == 0) return sendScanError("Select at least one baud rate and one format.");

  req.idFrom = (uint8_t)idFrom;
  req.idTo = (uint8_t)idTo;
  req.fc = (uint8_t)fc;
  req.addr = (uint16_t)addr;
  req.timeoutMs = (uint16_t)timeoutMs;
  req.stopAtFirstHit = server.arg("stopFirst") == "1";

  portENTER_CRITICAL(&scanMux);
  scanRequest = req;
  portEXIT_CRITICAL(&scanMux);
  scanStartPending = true;

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiScanStop() {
  if (!requireAuthApi()) return;
  if (bridgeMode == MODE_SCAN) scanStopPending = true;
  scanStartPending = false;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiScan() {
  if (!requireAuthApi()) return;

  static ScanStatus st;  // big-ish: keep it off the stack
  ScanRequest req;
  portENTER_CRITICAL(&scanMux);
  st = scanStatus;
  req = scanRequest;
  portEXIT_CRITICAL(&scanMux);

  const char *stateNames[] = { "IDLE", "RUNNING", "DONE", "STOPPED" };
  uint8_t state = st.state;
  if (scanStartPending && state != SCAN_RUNNING) state = SCAN_RUNNING;
  uint32_t endMs = (st.state == SCAN_RUNNING || st.finishedMs == 0) ? millis() : st.finishedMs;

  String j;
  j.reserve(4096);
  j += "{\"state\":\"" + String(stateNames[state < 4 ? state : 0]) + "\",\"done\":" + String(st.probesDone) + ",\"total\":" + String(st.probesTotal);
  j += ",\"id\":" + String(st.currentId) + ",\"dropped\":" + String(st.droppedHostBytes);
  j += ",\"elapsedS\":" + String(st.startedMs ? (endMs - st.startedMs) / 1000 : 0);
  String combo = (st.comboIndex < req.comboCount) ? (String(req.combos[st.comboIndex].baud) + " " + comboText(req.combos[st.comboIndex])) : String("-");
  j += ",\"combo\":\"" + combo + "\",\"results\":[";
  for (uint8_t i = 0; i < st.resultCount; i++) {
    const ScanResult &r = st.results[i];
    if (i) j += ",";
    j += "{\"id\":" + String(r.id) + ",\"baud\":" + String(r.combo.baud) + ",\"fmt\":\"" + comboText(r.combo) + "\",\"exc\":" + String(r.excCode) + ",\"lat\":\"" + msText(r.latencyUs) + "\"}";
  }
  j += "],\"garbled\":[";
  bool first = true;
  for (uint8_t c = 0; c < req.comboCount && c < 32; c++) {
    if (!(st.garbledMask & (1UL << c))) continue;
    if (!first) j += ",";
    first = false;
    j += "\"" + String(req.combos[c].baud) + " " + comboText(req.combos[c]) + "\"";
  }
  j += "]}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

// ===================== Sniffer page =====================
const char SNIFF_BODY[] PROGMEM = R"rawliteral(
<div class="box"><h3>Sniffer: listen to another master's bus</h3>
<p class="note">Listen-only: the dongle never transmits while sniffing (its RS485 driver stays off), so it can be connected to a working bus without disturbing it. Frames are decoded live below, and the <b>Diagnostics</b> page analyses the traffic (timeouts, exceptions, latency per slave) with the same remedies as for the dongle's own traffic. Data sent by the PC is discarded while sniffing.</p>
<table style="width:auto"><tr><td class="l">Bus settings</td><td class="l"><select id="combo"></select></td></tr>
<tr><td class="l">Copy to PC</td><td class="l"><label><input type="checkbox" id="fwd" checked> Copy the raw bus traffic to the dongle's COM port (for PC capture tools)</label></td></tr>
<tr><td class="l">Statistics</td><td class="l"><label><input type="checkbox" id="rst" checked> Reset statistics when starting</label></td></tr></table>
<button type="button" id="startBtn" onclick="start()">Start sniffing</button>
<button type="button" class="danger" id="stopBtn" onclick="stop()" disabled>Stop</button>
<div id="msg"></div><p id="status" class="note"></p></div>

<div class="box"><h3>Frames</h3>
<p>Slave ID filter <input type="number" id="flt" min="0" max="247" placeholder="all" style="width:70px">
<label><input type="checkbox" id="pause"> Pause view</label>
<button type="button" onclick="clearView()">Clear view</button>
<button type="button" onclick="csv()">Download CSV</button> <span class="note" id="kept"></span></p>
<table><thead><tr><th>#</th><th>Time (s)</th><th>Gap (ms)</th><th>Direction</th><th>ID</th><th>Function</th><th>Details</th><th>Raw bytes</th></tr></thead><tbody id="rows"></tbody></table></div>

<script>
const $=id=>document.getElementById(id);
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
const BAUDS=[9600,19200,38400,115200,57600,4800,2400,1200], FMTS=[['N1','8N1'],['E1','8E1'],['O1','8O1'],['N2','8N2']];
$('combo').innerHTML='<option value="auto">Auto-detect (listen at each common setting)</option>'+BAUDS.map(b=>FMTS.map(f=>'<option value="'+b+','+f[0]+'">'+b+' '+f[1]+'</option>').join('')).join('');
const FC={1:'Read Coils',2:'Read Discrete Inputs',3:'Read Holding Registers',4:'Read Input Registers',5:'Write Single Coil',6:'Write Single Register',7:'Read Exception Status',8:'Diagnostics',11:'Get Comm Event Counter',15:'Write Multiple Coils',16:'Write Multiple Registers',17:'Report Server ID',22:'Mask Write Register',23:'Read/Write Multiple Registers',43:'Encapsulated Interface'};
const EXC={1:'Illegal Function',2:'Illegal Data Address',3:'Illegal Data Value',4:'Slave Device Failure',5:'Acknowledge',6:'Slave Device Busy',8:'Memory Parity Error',10:'Gateway Path Unavailable',11:'Gateway Target No Response'};
const KIND=['REQUEST','RESPONSE','EXCEPTION','FRAME','GARBAGE'];
let since=0, frames=[], lastReq={}, prevT=null, running=false;
const MAXKEEP=20000, MAXROWS=500;
function w(b,i){return (b[i]<<8)|b[i+1];}
function decode(f){
  const b=f.b, k=KIND[f.k];
  if(k=='GARBAGE') return {dir:'?',id:'',fn:'',det:f.n+' byte(s) with no valid frame (noise, collision or wrong settings)'};
  const id=b[0], fc=b[1]&0x7f, name=FC[fc]||('FC '+fc);
  if(k=='EXCEPTION') return {dir:'SLAVE',id:id,fn:name,det:'Exception '+b[2]+' '+(EXC[b[2]]||'')};
  if(k=='REQUEST'){
    lastReq[id]={fc:fc,addr:w(b,2),qty:w(b,4)};
    let d='';
    if(fc>=1&&fc<=4) d='address '+w(b,2)+', quantity '+w(b,4);
    else if(fc==5) d='coil '+w(b,2)+' = '+(b[4]==0xff?'ON':'OFF');
    else if(fc==6) d='register '+w(b,2)+' = '+w(b,4);
    else if(fc==15) d=w(b,4)+' coil(s) from '+w(b,2);
    else if(fc==16){const n=w(b,4),v=[];for(let i=0;i<n&&i<10&&7+2*i+1<f.n-2;i++)v.push(w(b,7+2*i));d=n+' register(s) from '+w(b,2)+': '+v.join(', ')+(n>10?' ...':'');}
    return {dir:'MASTER',id:id,fn:name,det:d};
  }
  if(k=='RESPONSE'){
    const r=lastReq[id]; let d='';
    if((fc==3||fc==4)&&b[2]){const n=b[2]/2,v=[];for(let i=0;i<n&&i<12;i++)v.push((r&&r.fc==fc?'['+(r.addr+i)+']=':'')+w(b,3+2*i));d=v.join(' ')+(n>12?' ...':'');}
    else if(fc==1||fc==2){const bits=[];for(let i=0;i<b[2]*8&&i<32;i++)bits.push((b[3+(i>>3)]>>(i&7))&1);if(r)bits.length=Math.min(bits.length,r.qty);d=(r?'from '+r.addr+': ':'')+bits.join('');}
    else if(fc==5||fc==6||fc==15||fc==16) d='OK (write confirmed)';
    return {dir:'SLAVE',id:id,fn:name,det:d};
  }
  return {dir:'?',id:id,fn:name,det:'valid frame, no matching request seen'};
}
function hex(f){return f.b.map(x=>x.toString(16).padStart(2,'0').toUpperCase()).join(' ')+(f.n>f.b.length?' ... ('+f.n+' bytes)':'');}
function rowHtml(f){
  const flt=$('flt').value;
  if(flt!==''&&String(f.d.id)!==flt) return '';
  const cls=f.k==4?'r-CRCLENGTH':(f.k==2?'r-EXCEPTION':(f.k==1?'r-OK':''));
  return '<tr><td>'+f.s+'</td><td>'+(f.t/1e6).toFixed(4)+'</td><td>'+(f.g==null?'':f.g.toFixed(1))+'</td><td class="'+cls+'">'+f.d.dir+'</td><td>'+f.d.id+'</td><td>'+esc(f.d.fn)+'</td><td class="l">'+esc(f.d.det)+'</td><td class="mono">'+hex(f)+'</td></tr>';
}
function redraw(){
  if($('pause').checked) return;
  const out=[];for(let i=frames.length-1;i>=0&&out.length<MAXROWS;i--){const r=rowHtml(frames[i]);if(r)out.push(r);}
  $('rows').innerHTML=out.join('')||'<tr><td colspan="8">No frames yet</td></tr>';
  $('kept').textContent=frames.length+' frame(s) kept for CSV (max '+MAXKEEP+')';
}
function clearView(){frames=[];lastReq={};prevT=null;redraw();}
function csv(){
  const L=['seq,time_s,gap_ms,direction,slave_id,function,details,raw_hex'];
  frames.forEach(f=>L.push([f.s,(f.t/1e6).toFixed(6),f.g==null?'':f.g.toFixed(3),f.d.dir,f.d.id,'"'+f.d.fn+'"','"'+String(f.d.det).replace(/"/g,"'")+'"',hex(f)].join(',')));
  const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([L.join('\n')],{type:'text/csv'}));a.download='modbus_sniff.csv';a.click();
}
async function post(url,body){
  const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body||''});
  if(r.status==401){location.href='/login';throw 0;}
  return r.json();
}
async function start(){
  const v=$('combo').value, p=new URLSearchParams({fwd:$('fwd').checked?1:0,reset:$('rst').checked?1:0});
  if(v=='auto') p.set('auto','1'); else {const x=v.split(',');p.set('baud',x[0]);p.set('fmt',x[1]);}
  const d=await post('/api/sniff/start',p.toString());
  $('msg').innerHTML=d.ok?'':'<div class="err">'+esc(d.error)+'</div>';
  if(d.ok) clearView();
}
async function stop(){await post('/api/sniff/stop');}
async function poll(){
  try{
    const r=await fetch('/api/sniff?since='+since,{cache:'no-store'});
    if(r.status==401){location.href='/login';return;}
    const d=await r.json();
    running=d.state!='OFF';
    $('startBtn').disabled=running;$('stopBtn').disabled=!running;
    let st;
    if(d.state=='OFF') st='Stopped.'+(d.frames?' Last session: '+d.frames+' frame(s), '+d.garbage+' garbage.':'');
    else if(d.state=='DETECTING') st='Auto-detecting bus settings: listening at '+d.combo+' ('+(d.detectIdx+1)+'/'+d.detectTotal+(d.cycles?', pass '+(d.cycles+1):'')+'). Make sure the master is polling.';
    else st='Sniffing at '+d.combo+(d.auto?' (auto-detected)':'')+' - '+d.frames+' frame(s), '+d.garbage+' garbage, '+d.bytes+' byte(s)'+(d.fwd?', copied to the PC':'')+'. Running '+d.elapsedS+' s.';
    if(d.dropped) st+=' '+d.dropped+' byte(s) from the PC discarded (the dongle does not transmit while sniffing).';
    if(d.missed) st+=' '+d.missed+' frame(s) were not fetched in time (view was too slow).';
    $('status').textContent=st;
    for(const f of d.list){
      f.d=decode(f); f.g=prevT==null?null:(f.t-prevT)/1000; prevT=f.t; frames.push(f);
    }
    if(frames.length>MAXKEEP) frames.splice(0,frames.length-MAXKEEP);
    if(d.list.length) since=d.list[d.list.length-1].s; else since=Math.max(since,d.seq);
    if(d.list.length||!frames.length) redraw();
    setTimeout(poll,d.more?50:500);
  }catch(e){setTimeout(poll,2000);}
}
$('flt').addEventListener('input',redraw);$('pause').addEventListener('change',redraw);
poll();
</script>
</body></html>
)rawliteral";

void handleSniffPage() {
  if (!requireAuth()) return;
  String html = pageHead("Sniffer");
  html += navBar();
  html += FPSTR(SNIFF_BODY);
  server.send(200, "text/html", html);
}

void handleApiSniffStart() {
  if (!requireAuthApi()) return;
  if (bridgeMode == MODE_SNIFF || sniffStartPending) return sendScanError("The sniffer is already running.");
  if (bridgeMode == MODE_SCAN || scanStartPending) return sendScanError("An auto-scan is running - wait for it or stop it first.");

  SniffRequest req;
  memset(&req, 0, sizeof(req));
  req.autoDetect = server.arg("auto") == "1";
  req.forwardUsb = server.arg("fwd") == "1";
  if (!req.autoDetect) {
    long baud = server.arg("baud").toInt();
    String fmt = server.arg("fmt");
    if (baud < MIN_BAUD || baud > MAX_BAUD) return sendScanError("Choose a baud rate or Auto-detect.");
    req.combo.baud = baud;
    if (fmt == "N1") { req.combo.parity = 0; req.combo.stopBits = 0; }
    else if (fmt == "E1") { req.combo.parity = 2; req.combo.stopBits = 0; }
    else if (fmt == "O1") { req.combo.parity = 1; req.combo.stopBits = 0; }
    else if (fmt == "N2") { req.combo.parity = 0; req.combo.stopBits = 2; }
    else return sendScanError("Unknown format.");
  }
  if (server.arg("reset") == "1") resetStatistics();

  portENTER_CRITICAL(&sniffMux);
  sniffRequest = req;
  portEXIT_CRITICAL(&sniffMux);
  sniffStartPending = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiSniffStop() {
  if (!requireAuthApi()) return;
  if (bridgeMode == MODE_SNIFF) sniffStopPending = true;
  sniffStartPending = false;
  server.send(200, "application/json", "{\"ok\":true}");
}

#define SNIFF_API_MAX 100
SniffEntry sniffOut[SNIFF_API_MAX];  // webTask-only copy buffer

void handleApiSniff() {
  if (!requireAuthApi()) return;

  uint32_t since = (uint32_t)server.arg("since").toInt();
  SniffStatus st;
  uint32_t n = 0, missed = 0;
  bool more = false;

  portENTER_CRITICAL(&sniffMux);
  st = sniffStatus;
  uint32_t oldest = (st.lastSeq > SNIFF_LOG_SIZE) ? st.lastSeq - SNIFF_LOG_SIZE + 1 : 1;
  uint32_t first = since + 1;
  if (first < oldest) {
    if (since > 0) missed = oldest - first;
    first = oldest;
  }
  for (uint32_t q = first; q <= st.lastSeq && n < SNIFF_API_MAX; q++) {
    sniffOut[n++] = sniffLog[q % SNIFF_LOG_SIZE];
  }
  more = (first + n <= st.lastSeq);
  portEXIT_CRITICAL(&sniffMux);

  const char *states[] = { "OFF", "DETECTING", "RUNNING" };
  LineCoding lc = { st.combo.baud, 8, st.combo.parity, st.combo.stopBits };
  uint8_t state = st.state;
  if (sniffStartPending && state == SNIFF_OFF) state = SNIFF_DETECTING;

  String j;
  j.reserve(4096 + n * 120);
  j += "{\"state\":\"" + String(states[state < 3 ? state : 0]) + "\",\"combo\":\"" + (st.combo.baud ? lineCodingText(lc) : String("-")) + "\"";
  j += ",\"auto\":" + String(st.autoDetected ? "true" : "false") + ",\"fwd\":" + String(st.forwardUsb ? "true" : "false");
  j += ",\"detectIdx\":" + String(st.detectIdx) + ",\"detectTotal\":" + String(SNIFF_DETECT_COMBOS) + ",\"cycles\":" + String(st.detectCycles);
  j += ",\"frames\":" + String(st.frames) + ",\"garbage\":" + String(st.garbage) + ",\"bytes\":" + String(st.bytes);
  j += ",\"dropped\":" + String(st.droppedHostBytes) + ",\"missed\":" + String(missed) + ",\"seq\":" + String(st.lastSeq);
  j += ",\"elapsedS\":" + String(st.startedMs ? (millis() - st.startedMs) / 1000 : 0) + ",\"more\":" + String(more ? "true" : "false");
  j += ",\"list\":[";
  for (uint32_t i = 0; i < n; i++) {
    const SniffEntry &e = sniffOut[i];
    if (i) j += ",";
    j += "{\"s\":" + String(e.seq) + ",\"t\":" + String((unsigned long long)e.tUs) + ",\"k\":" + String(e.kind) + ",\"n\":" + String(e.len) + ",\"b\":[";
    uint16_t m = min((int)e.len, SNIFF_MAX_BYTES);
    for (uint16_t b = 0; b < m; b++) {
      if (b) j += ",";
      j += String(e.bytes[b]);
    }
    j += "]}";
  }
  j += "]}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

// ===================== Settings page =====================
void handleSettings() {
  if (!requireAuth()) return;

  String html = pageHead("Settings");
  html += navBar();

  struct Msg { const char *arg; bool ok; const char *text; };
  const Msg msgs[] = {
    { "diagSaved", true, "Diagnostics settings saved." },
    { "diagError", false, "Invalid diagnostics value." },
    { "wifiSaved", true, "Site WiFi settings saved - connecting." },
    { "deviceSaved", true, "Access Point settings saved - reconnect to the new AP if you changed it." },
    { "apPassShort", false, "AP password must be at least 8 characters." },
    { "apSsidBad", false, "AP name must be 1-32 characters." },
    { "securitySaved", true, "Security settings saved." },
    { "securityMismatch", false, "Passwords do not match." },
    { "securityShort", false, "Password too short (minimum 4 characters)." },
    { "saveError", false, "Saving to flash failed - settings may not survive a reboot." },
  };
  for (const Msg &m : msgs) {
    if (server.hasArg(m.arg)) html += String("<div class='") + (m.ok ? "ok" : "err") + "'>" + m.text + "</div>";
  }

  // ---- Diagnostics ----
  html += "<div class='box'><h3>Diagnostics</h3><form action='/saveDiag' method='POST'>";
  html += "Reply timeout for statistics: <input type='number' name='rspTo' min='" + String(MIN_DIAG_RESPONSE_TIMEOUT_MS) + "' max='" + String(MAX_DIAG_RESPONSE_TIMEOUT_MS) + "' value='" + String(diagResponseTimeoutMs) + "'> ms<br>";
  html += "<small class='note'>How long the dongle waits for a reply before counting a TIMEOUT. It does not change what the PC sees - set it to your master's response timeout so both agree. A reply that arrives later is counted as a LATE reply.</small><br>";
  html += "Consecutive timeouts before a slave is reported DOWN: <input type='number' name='deadTh' min='1' max='1000' value='" + String(diagDeadSlaveThreshold) + "'><br>";
  html += "<button type='submit'>Save Diagnostics</button></form></div>";

  // ---- Site WiFi ----
  html += "<div class='box'><h3>Site WiFi (optional)</h3><form action='/saveWifi' method='POST'>";
  html += "Status: ";
  if (wifiConfig.ssid.length() == 0) html += "not configured";
  else if (WiFi.status() == WL_CONNECTED) html += "connected, IP " + WiFi.localIP().toString() + " (" + String(WiFi.RSSI()) + " dBm)";
  else html += "not connected";
  html += "<br>SSID: <input type='text' name='ssid' maxlength='32' value='" + htmlEscape(wifiConfig.ssid) + "'><br>";
  html += "Password: <input type='password' name='pass' maxlength='63' placeholder='(unchanged)'><br>";
  html += "<label><input type='checkbox' name='forget' value='1'> Forget the site network</label><br>";
  html += "<small class='note'>Joins your site network in addition to the dongle's own Access Point, so the diagnostics page can also be reached at the IP shown above. Leave the password blank to keep the current one.</small><br>";
  html += "<button type='submit'>Save WiFi</button></form></div>";

  // ---- Access Point ----
  html += "<div class='box'><h3>Device Access Point</h3><form action='/saveDevice' method='POST'>";
  html += "AP name (SSID): <input type='text' name='apSsid' maxlength='32' value='" + htmlEscape(deviceConfig.apSsid) + "'><br>";
  html += "AP password: <input type='password' name='apPassword' maxlength='63' placeholder='(unchanged)'><br>";
  html += "<small class='note'>WPA2 key for this dongle's own hotspot (minimum 8 characters). Address: http://192.168.4.1. If you forget it, open the COM port at <b>74880</b> baud and send <b>factory-reset-auth</b>.</small><br>";
  html += "<button type='submit'>Save Access Point</button></form></div>";

  // ---- Security ----
  html += "<div class='box'><h3>Security</h3><form action='/saveSecurity' method='POST'>";
  if (webAuthUsingDefaults()) html += "<div class='banner'>Default login (admin/admin) in use - set your own password.</div>";
  html += "Username: <input type='text' name='webUser' maxlength='32' value='" + htmlEscape(authConfig.user) + "'><br>";
  html += "New password: <input type='password' name='webPass' maxlength='64'><br>";
  html += "Confirm password: <input type='password' name='webPass2' maxlength='64'><br>";
  html += "<small class='note'>Leave the password fields blank to keep the current password. Login sessions time out after " + String(WEB_SESSION_TIMEOUT_MS / 60000) + " minutes idle; " + String(WEB_LOGIN_MAX_FAILS) + " failed logins lock the login page for " + String(WEB_LOGIN_LOCKOUT_MS / 1000) + " s. Forgot the password? Open the COM port at <b>74880</b> baud and send <b>factory-reset-auth</b>: it restores the default login and AP password without touching other settings.</small><br>";
  html += "<button type='submit'>Save Security</button></form></div>";

  // ---- Device info ----
  html += "<div class='box'><h3>Device</h3>";
  html += "Firmware: " FW_VERSION "<br>Hostname: " + hostnameString() + "<br>AP MAC: " + WiFi.softAPmacAddress() + "<br>";
  html += "RS485: RX=D2 TX=D3 DE/RE=D4 &middot; Port settings follow the PC (default 9600 8N1)<br>";
  html += "<form action='/reboot' method='POST' onsubmit=\"return confirm('Reboot the dongle?')\"><button class='danger' type='submit'>Reboot</button></form></div>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSaveDiag() {
  if (!requireAuth()) return;
  long to = server.arg("rspTo").toInt();
  long th = server.arg("deadTh").toInt();
  if (to < MIN_DIAG_RESPONSE_TIMEOUT_MS || to > MAX_DIAG_RESPONSE_TIMEOUT_MS || th < 1 || th > 1000) {
    server.sendHeader("Location", "/settings?diagError=1");
    server.send(303);
    return;
  }
  diagResponseTimeoutMs = (uint32_t)to;
  diagDeadSlaveThreshold = (uint16_t)th;
  bool ok = saveDiagConfig();
  logKeyEvent("DIAG SETTINGS SAVED: timeout=%ldms down-after=%ld", to, th);
  server.sendHeader("Location", ok ? "/settings?diagSaved=1" : "/settings?saveError=1");
  server.send(303);
}

void handleSaveWifi() {
  if (!requireAuth()) return;
  if (server.arg("forget") == "1") {
    wifiConfig.ssid = "";
    wifiConfig.password = "";
  } else {
    String ssid = server.arg("ssid");
    ssid.trim();
    if (ssid != wifiConfig.ssid) wifiConfig.password = "";
    wifiConfig.ssid = ssid;
    if (server.arg("pass").length() > 0) wifiConfig.password = server.arg("pass");
  }
  bool ok = saveWifiConfig();
  logKeyEvent("WIFI SETTINGS SAVED: %s", wifiConfig.ssid.length() ? wifiConfig.ssid.c_str() : "(none)");

  WiFi.disconnect();
  lastWifiConnected = false;
  lastWifiAttempt = millis();
  connectSiteWiFi();

  server.sendHeader("Location", ok ? "/settings?wifiSaved=1" : "/settings?saveError=1");
  server.send(303);
}

void handleSaveDevice() {
  if (!requireAuth()) return;
  String apSsid = server.arg("apSsid");
  apSsid.trim();
  if (apSsid.length() < 1 || apSsid.length() > 32) {
    server.sendHeader("Location", "/settings?apSsidBad=1");
    server.send(303);
    return;
  }
  bool apPassChanged = false;
  if (server.arg("apPassword").length() > 0) {
    if (server.arg("apPassword").length() < AP_PASSWORD_MIN_LEN) {
      server.sendHeader("Location", "/settings?apPassShort=1");
      server.send(303);
      return;
    }
    deviceConfig.apPassword = server.arg("apPassword");
    apPassChanged = true;
  }
  deviceConfig.apSsid = apSsid;
  bool ok = saveDeviceConfig();
  logKeyEvent("AP SETTINGS SAVED: %s%s", apSsid.c_str(), apPassChanged ? " (password changed)" : "");

  server.sendHeader("Location", ok ? "/settings?deviceSaved=1" : "/settings?saveError=1");
  server.send(303);
  delay(200);
  WiFi.softAP(deviceConfig.apSsid.c_str(), deviceConfig.apPassword.c_str());
}

void handleSaveSecurity() {
  if (!requireAuth()) return;

  String user = server.arg("webUser");
  String pass1 = server.arg("webPass");
  String pass2 = server.arg("webPass2");
  user.trim();

  if (pass1 != pass2) {
    server.sendHeader("Location", "/settings?securityMismatch=1");
    server.send(303);
    return;
  }
  if (pass1.length() > 0 && pass1.length() < WEB_PASSWORD_MIN_LEN) {
    server.sendHeader("Location", "/settings?securityShort=1");
    server.send(303);
    return;
  }

  if (user.length() > 0) authConfig.user = user;
  if (pass1.length() > 0) {
    authConfig.salt = makeRandomHex(8);
    authConfig.passHash = sha256Hex(authConfig.salt + pass1);
  }

  bool ok = saveAuthConfig();
  logKeyEvent("SECURITY SETTINGS SAVED: web user=%s %s", authConfig.user.c_str(), pass1.length() ? "(password changed)" : "(password unchanged)");

  // The admin who just changed the password stays logged in.
  server.sendHeader("Location", ok ? "/settings?securitySaved=1" : "/settings?saveError=1");
  server.send(303);
}

void handleReboot() {
  if (!requireAuth()) return;
  logKeyEvent("REBOOT REQUESTED FROM WEB");
  server.send(200, "text/html", pageHead("Reboot") + "<div class='box'>Rebooting... <a href='/'>reconnect</a> in a few seconds.</div></body></html>");
  delay(500);
  ESP.restart();
}

void handleNotFound() {
  server.sendHeader("Location", "/");
  server.send(302);
}

// ===================== USB console (credential recovery) =====================
// Reached by opening the COM port at CONSOLE_BAUD. Runs in bridgeTask (the
// sole owner of the USB data port), never concurrently with bridging.
void consoleBanner() {
  Serial.println();
  Serial.println("=== Modbus Dongle console (firmware " FW_VERSION ") ===");
  Serial.println("Nothing is bridged while the port is open at 74880 baud.");
  Serial.println("Commands: status, factory-reset-auth, reboot, help");
}

void consoleStatus() {
  GlobalStats g;
  portENTER_CRITICAL(&statsMux);
  memcpy(&g, &gstats, sizeof(g));
  portEXIT_CRITICAL(&statsMux);

  Serial.printf("Firmware      : %s\r\n", FW_VERSION);
  Serial.printf("Uptime        : %lu s\r\n", (unsigned long)(millis() / 1000));
  Serial.printf("Access Point  : %s  http://%s\r\n", deviceConfig.apSsid.c_str(), WiFi.softAPIP().toString().c_str());
  if (wifiConfig.ssid.length()) {
    Serial.printf("Site WiFi     : %s  %s\r\n", wifiConfig.ssid.c_str(), WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "(not connected)");
  } else {
    Serial.println("Site WiFi     : not configured");
  }
  Serial.printf("Web login     : %s%s\r\n", authConfig.user.c_str(), webAuthUsingDefaults() ? " (default password)" : "");
  Serial.printf("Health        : %s\r\n", sevName(healthLevel));
  Serial.printf("Requests %lu  OK %lu  Timeouts %lu  CRC %lu  Exceptions %lu\r\n",
                (unsigned long)g.requests, (unsigned long)g.ok, (unsigned long)g.timeouts,
                (unsigned long)g.crcErrors, (unsigned long)g.exceptions);
}

void consoleProcess() {
  static String buf = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      buf.trim();
      if (buf == "factory-reset-auth") {
        preferences.begin("auth", false);
        preferences.clear();
        preferences.end();
        deviceConfig.apPassword = DEFAULT_AP_PASSWORD;
        saveDeviceConfig();
        Serial.println("AUTH RESET OK: web login back to " WEB_AUTH_DEFAULT_USER "/" WEB_AUTH_DEFAULT_PASS ", AP password back to " DEFAULT_AP_PASSWORD " - rebooting");
        Serial.flush();
        delay(500);
        ESP.restart();
      } else if (buf == "status") {
        consoleStatus();
      } else if (buf == "reboot") {
        Serial.println("Rebooting...");
        Serial.flush();
        delay(300);
        ESP.restart();
      } else if (buf == "help" || buf.length() > 0) {
        if (buf != "help") Serial.println("Unknown command.");
        consoleBanner();
      }
      buf = "";
    } else if (buf.length() < 64) {
      buf += c;
    }
  }
}

// ===================== Setup / webTask =====================
void setupWeb() {
  String host = hostnameString();
  WiFi.setHostname(host.c_str());  // must be set before WiFi starts
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAP(deviceConfig.apSsid.c_str(), deviceConfig.apPassword.c_str());
  logKeyEvent("AP STARTED: %s %s", deviceConfig.apSsid.c_str(), WiFi.softAPIP().toString().c_str());
  connectSiteWiFi();
  lastWifiAttempt = millis();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, handleScanPage);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/api/diag", HTTP_GET, handleApiDiag);
  server.on("/api/resetStats", HTTP_POST, handleApiResetStats);
  server.on("/api/scan", HTTP_GET, handleApiScan);
  server.on("/api/scan/start", HTTP_POST, handleApiScanStart);
  server.on("/api/scan/stop", HTTP_POST, handleApiScanStop);
  server.on("/sniff", HTTP_GET, handleSniffPage);
  server.on("/api/sniff", HTTP_GET, handleApiSniff);
  server.on("/api/sniff/start", HTTP_POST, handleApiSniffStart);
  server.on("/api/sniff/stop", HTTP_POST, handleApiSniffStop);
  server.on("/saveDiag", HTTP_POST, handleSaveDiag);
  server.on("/saveWifi", HTTP_POST, handleSaveWifi);
  server.on("/saveDevice", HTTP_POST, handleSaveDevice);
  server.on("/saveSecurity", HTTP_POST, handleSaveSecurity);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/login", HTTP_GET, handleLoginPage);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", HTTP_GET, handleLogout);
  server.onNotFound(handleNotFound);

  // WebServer only exposes headers it was told to collect BEFORE begin().
  const char *collectHeaderKeys[] = { "Cookie" };
  server.collectHeaders(collectHeaderKeys, 1);
  server.begin();
  logKeyEvent("WEB SERVER STARTED");
}

void webTask(void *arg) {
  unsigned long lastDiag = 0;
  for (;;) {
    server.handleClient();
    handleWifiRetry();

    // Keeps the LED verdict current even when no browser is open.
    if (millis() - lastDiag >= 2000) {
      lastDiag = millis();
      runDiagnosis();
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
