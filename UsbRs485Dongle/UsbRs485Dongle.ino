/* ======================================================================
   USB <-> RS485 (Modbus RTU) CONVERTER + BUS DIAGNOSTICS - Arduino Nano ESP32
   ======================================================================
   Turns the ESP32 Modbus Gateway board into a transparent USB-to-RS485
   dongle (Waveshare USB TO RS485 equivalent): the PC sees a normal COM /
   ttyACM port and any Modbus master software talks straight through it.
   On top of that, the dongle watches every transaction it carries and
   explains what is wrong with the bus on a WiFi web page.

   HARDWARE - identical to the Gateway board (brahmadeomk/Gateway):
     RS485 transceiver   RX = D2, TX = D3, DE/RE = D4   (Serial1)
     Cellular modem      D5/D6 UART, A5 PWRKEY          (not used - idle)
     Digital Out 1-4     D11, D12, D13, A4              (held LOW = off)
     Digital In / Analog D7-D10, A0-A3                  (not used)
     WiFi                onboard - Access Point + optional site WiFi

   ARCHITECTURE NOTES
   - Same core split as the Gateway: bridgeTask owns Serial1 + the USB data
     port and runs alone on Core 1 at high priority; webTask (WebServer,
     WiFi retry, diagnosis engine) runs on Core 0 with the WiFi/lwIP stack,
     so web/WiFi activity can never delay an RS485 turnaround.
   - RS485 direction control is the Gateway's proven sequence: DE/RE
     toggled inside a portENTER/EXIT_CRITICAL section, 300us driver settle,
     Serial1.flush() (blocks until the last stop bit is on the wire), then
     an immediate, un-delayed release back to receive. Modbus T3.5 silence
     is enforced before every transmission.
   - The data path stays byte-transparent: nothing is altered, delayed for
     analysis, retried or filtered. The analyzer only OBSERVES:
       * each USB burst sent to the bus is a request (CRC-checked RTU frame)
       * reply bytes are framed by the Modbus length rules for the request's
         function code, with a silence/timeout fallback
       * the result (OK / exception / timeout / CRC / wrong slave / ...) and
         the slave's response latency go into per-slave statistics
       * bytes nobody asked for are classified as late replies, foreign
         frames (another master) or noise
   - Bus health: UART framing/parity/break/overflow errors are counted via
     onReceiveError(), and the idle level of the receiver output is sampled
     while the bus is quiet (stuck LOW = A/B swapped or no fail-safe bias).
   - The diagnosis engine (rule based, webTask) turns the counters into
     findings with plain-language causes and fixes, plus an overall
     OK / WARN / FAULT verdict, shown on the web page and on the RGB LED.
   - Auto-scan: started from the web page, bridgeTask takes the bus over,
     probes slave IDs across baud/parity combinations (common settings
     first), then restores the host's settings. Host bytes arriving during
     a scan are discarded (and counted) - stop the master before scanning.
   - Web UI follows the Gateway's conventions: always-on AP (default
     "ModbusDongle" / 12345678, http://192.168.4.1), optional site WiFi,
     single-session login with salted SHA-256, 30 min idle timeout, 5-fail
     lockout, default-login banner, key event log, NVS settings.
   - Credential recovery: the USB port carries data, so the Gateway's
     serial command is reached through a "console" baud rate: open the COM
     port at CONSOLE_BAUD (74880) and send factory-reset-auth. In console
     mode nothing is bridged.

   BUILD (Arduino IDE / arduino-cli)
     Board : "Arduino Nano ESP32" (arduino:esp32:nano_nora)
             or esp32:esp32:nano_nora from the Espressif core (2.0.x/3.x)
     USB   : default mode (TinyUSB CDC). The "Debug / Hardware CDC" USB
             mode cannot report line coding and is rejected at compile time.
     Core Debug Level: None (ESP log output would mix into the data port).
     No external libraries required.
   ====================================================================== */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dongle_types.h"

#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT || (defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE)
#error "Select the Arduino Nano ESP32 with its default USB mode (TinyUSB CDC on boot). Hardware-CDC/debug mode cannot follow the host's baud/parity."
#endif

#define FW_VERSION "1.1.0"

// ===================== Pins (same as Gateway board) =====================
#define RXD2 D2
#define TXD2 D3
#define RS485_DE_RE D4

// Raw GPIO number of the RX pin, for sampling the idle line level directly.
#if defined(BOARD_HAS_PIN_REMAP)
#define RS485_RX_GPIO ((gpio_num_t)digitalPinToGPIONumber(RXD2))
#else
#define RS485_RX_GPIO ((gpio_num_t)(RXD2))
#endif

#define MODEM_PWRKEY_PIN A5  // idle HIGH on the Gateway (pulse is LOW)
const uint8_t DO_PINS[] = { D11, D12, D13, A4 };

// ===================== Converter Settings =====================
// Used until the host sets a line coding (and if FOLLOW_HOST_LINE_CODING is 0).
#define DEFAULT_BAUD 9600
#define DEFAULT_DATA_BITS 8
#define DEFAULT_PARITY 0     // 0 none, 1 odd, 2 even
#define DEFAULT_STOP_BITS 0  // 0 = 1 stop bit, 2 = 2 stop bits

// 1 = mirror the PC's COM-port settings onto RS485 (Waveshare behaviour).
// 0 = ignore them and always use the DEFAULT_* settings above.
#define FOLLOW_HOST_LINE_CODING 1

#define MIN_BAUD 300
#define MAX_BAUD 2000000

// Opening the COM port at this baud enters the text console (credential
// recovery, status) instead of bridging. 74880 is never used for Modbus.
#define CONSOLE_BAUD 74880

#define DE_SETTLE_US 300
#define ENFORCE_T35_GAP 1

// Arduino's 1200-baud "touch" reboots into the bootloader for uploads.
#define ALLOW_1200BAUD_REBOOT 1

#define UART_RX_BUFFER_SIZE 2048
#define UART_RX_FIFO_FULL 16  // deliver RX every 16 bytes (or 1 char idle)
#define USB_RX_BUFFER_SIZE 2048
#define USB_TX_TIMEOUT_MS 20
#define CHUNK_SIZE 256
#define FRAME_BUF_SIZE 260  // max RTU ADU is 256 bytes

#define LED_FLASH_MS 25

// ===================== Diagnostics defaults (overridable in Settings) =====================
#define DEFAULT_DIAG_RESPONSE_TIMEOUT_MS 1000
#define MIN_DIAG_RESPONSE_TIMEOUT_MS 20
#define MAX_DIAG_RESPONSE_TIMEOUT_MS 10000
#define DEFAULT_DEAD_SLAVE_THRESHOLD 5
#define IDLE_SAMPLE_INTERVAL_MS 500
#define LATE_REPLY_WINDOW_MS 10000

// ===================== Tasks =====================
#define BRIDGE_TASK_CORE 1
#define BRIDGE_TASK_PRIORITY (configMAX_PRIORITIES - 5)
#define BRIDGE_TASK_STACK 8192
#define WEB_TASK_CORE 0
#define WEB_TASK_PRIORITY 1
#define WEB_TASK_STACK 16384

// ===================== WiFi / Web (Gateway conventions) =====================
#define DEFAULT_AP_SSID "ModbusDongle"
#define DEFAULT_AP_PASSWORD "12345678"
#define AP_PASSWORD_MIN_LEN 8
#define HOSTNAME_PREFIX "Dongle-"
#define WIFI_RETRY_INTERVAL_MS 5000
#define WEB_AUTH_DEFAULT_USER "admin"
#define WEB_AUTH_DEFAULT_PASS "admin"
#define WEB_PASSWORD_MIN_LEN 4
#define WEB_SESSION_TIMEOUT_MS (30UL * 60UL * 1000UL)
#define WEB_LOGIN_MAX_FAILS 5
#define WEB_LOGIN_LOCKOUT_MS 60000UL
#define KEY_LOG_SIZE 20
#define KEY_LOG_MSG_LEN 112

// ===================== Shared configuration (loaded from NVS) =====================
struct AuthConfig {
  String user;
  String salt;
  String passHash;  // sha256Hex(salt + password); empty = factory default
};

struct DeviceConfig {
  String apSsid;
  String apPassword;
};

struct WifiConfig {
  String ssid;
  String password;
};

AuthConfig authConfig;
DeviceConfig deviceConfig;
WifiConfig wifiConfig;
Preferences preferences;

volatile uint32_t diagResponseTimeoutMs = DEFAULT_DIAG_RESPONSE_TIMEOUT_MS;
volatile uint16_t diagDeadSlaveThreshold = DEFAULT_DEAD_SLAVE_THRESHOLD;

// ===================== Shared runtime state =====================
portMUX_TYPE rs485Mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE lineCodingMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE scanMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE keyLogMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool lineCodingPending = false;
LineCoding pendingCoding;  // guarded by lineCodingMux
LineCoding activeCoding = { DEFAULT_BAUD, DEFAULT_DATA_BITS, DEFAULT_PARITY, DEFAULT_STOP_BITS };
volatile bool lineCodingFromHost = false;
uint32_t activeBaud = DEFAULT_BAUD;
uint32_t charTimeUs = 1146;  // real character time at the active settings

volatile uint8_t bridgeMode = MODE_BRIDGE;
volatile uint8_t idleLevel = IDLE_UNKNOWN;
volatile uint8_t healthLevel = SEV_OK;  // set by the diagnosis engine (webTask)
volatile bool usbRxOverflow = false;

// Statistics: written by bridgeTask, read (snapshot) by webTask, statsMux.
SlaveStats slaveStats[SLAVE_SLOTS];
GlobalStats gstats;
TxLogEntry txLog[TXLOG_SIZE];
uint8_t txLogHead = 0;
uint32_t txLogTotal = 0;

// Scan: request written by webTask, status written by bridgeTask, scanMux.
ScanRequest scanRequest;
ScanStatus scanStatus;
volatile bool scanStartPending = false;
volatile bool scanStopPending = false;

// Key event log (Gateway "Status Log" equivalent), keyLogMux.
char keyLog[KEY_LOG_SIZE][KEY_LOG_MSG_LEN];
uint32_t keyLogTime[KEY_LOG_SIZE];
uint8_t keyLogHead = 0;
uint8_t keyLogCount = 0;

TaskHandle_t bridgeTaskHandle = NULL;
TaskHandle_t webTaskHandle = NULL;

// ===================== bridgeTask-private state =====================
uint32_t lastBusActivityUs = 0;
uint8_t chunk[CHUNK_SIZE];

// Pending transaction (request sent, waiting for the reply).
bool awaiting = false;
uint8_t reqSlave = 0, reqFc = 0;
uint8_t reqBuf[FRAME_BUF_SIZE];
uint16_t reqLen = 0;
bool reqOverflow = false;
uint32_t txEndUs = 0;
uint8_t rspBuf[FRAME_BUF_SIZE];
uint16_t rspLen = 0;
uint32_t lastRxUs = 0;

// Bytes that arrived with no request pending.
uint8_t unsBuf[FRAME_BUF_SIZE];
uint16_t unsLen = 0;
uint32_t unsLastUs = 0;
uint8_t lastTimeoutSlave = 0;
uint32_t lastTimeoutMs = 0;

// ===================== Key event log =====================
void logKeyEvent(const char *fmt, ...) {
  char buf[KEY_LOG_MSG_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  uint32_t now = millis();
  portENTER_CRITICAL(&keyLogMux);
  memcpy(keyLog[keyLogHead], buf, KEY_LOG_MSG_LEN);
  keyLogTime[keyLogHead] = now;
  keyLogHead = (keyLogHead + 1) % KEY_LOG_SIZE;
  if (keyLogCount < KEY_LOG_SIZE) keyLogCount++;
  portEXIT_CRITICAL(&keyLogMux);
}

// ===================== Status LED (RGB, active LOW) =====================
#if defined(LED_RED) && defined(LED_GREEN) && defined(LED_BLUE)
#define HAS_RGB_LED 1
#else
#define HAS_RGB_LED 0
#endif

unsigned long ledTxUntil = 0, ledRxUntil = 0, ledErrUntil = 0;

void ledInit() {
#if HAS_RGB_LED
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_BLUE, HIGH);
#endif
}

// Activity flashes win; otherwise the health verdict blinks once every
// 2 s: red = FAULT, yellow (red+green) = WARN. Scan = slow purple.
void ledUpdate() {
#if HAS_RGB_LED
  static uint8_t shown = 0xFF;
  unsigned long now = millis();
  uint8_t want = ((long)(ledErrUntil - now) > 0 ? 1 : 0) |
                 ((long)(ledRxUntil - now) > 0 ? 2 : 0) |
                 ((long)(ledTxUntil - now) > 0 ? 4 : 0);

  if (want == 0) {
    if (bridgeMode == MODE_SCAN) {
      if ((now % 1000) < 150) want = 1 | 4;
    } else if ((now % 2000) < 120) {
      if (healthLevel == SEV_FAULT) want = 1;
      else if (healthLevel == SEV_WARN) want = 1 | 2;
    }
  }

  if (want == shown) return;
  shown = want;
  digitalWrite(LED_RED, (want & 1) ? LOW : HIGH);
  digitalWrite(LED_GREEN, (want & 2) ? LOW : HIGH);
  digitalWrite(LED_BLUE, (want & 4) ? LOW : HIGH);
#endif
}

// ===================== RS485 Direction Control (from Gateway) =====================
void preTransmission() {
  portENTER_CRITICAL(&rs485Mux);
  digitalWrite(RS485_DE_RE, HIGH);
  portEXIT_CRITICAL(&rs485Mux);

  delayMicroseconds(DE_SETTLE_US);
}

void postTransmission() {
  // No added delay: Serial1.flush() (called just before this) already
  // blocks until the last stop bit is on the wire.
  portENTER_CRITICAL(&rs485Mux);
  digitalWrite(RS485_DE_RE, LOW);
  portEXIT_CRITICAL(&rs485Mux);
}

// ===================== Modbus helpers (from Gateway) =====================
uint32_t modbusCharTimeUs() {
  return (uint32_t)((11.0 * 1000000.0) / activeBaud);
}

uint32_t modbusT35Us() {
  return (activeBaud > 19200) ? 1750 : (modbusCharTimeUs() * 7 / 2);
}

uint16_t modbusCRC16(const uint8_t *buf, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (uint16_t pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];
    for (uint8_t i = 0; i < 8; i++) {
      if (crc & 0x0001) {
        crc >>= 1;
        crc ^= 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

bool frameCrcOk(const uint8_t *p, uint16_t len) {
  if (len < 4) return false;
  uint16_t rx = (uint16_t)p[len - 2] | ((uint16_t)p[len - 1] << 8);
  return modbusCRC16(p, len - 2) == rx;
}

// Expected reply length for a request with function code fc, given the
// first n reply bytes. -1 = need more bytes, 0 = unknown (frame by silence).
int expectedReplyLen(const uint8_t *p, uint16_t n, uint8_t fc) {
  if (n < 2) return -1;
  if (p[1] == (uint8_t)(fc | 0x80)) return 5;
  switch (fc) {
    case 1: case 2: case 3: case 4:
    case 17: case 23:
      return (n >= 3) ? 5 + p[2] : -1;
    case 5: case 6: case 15: case 16:
    case 11:
      return 8;
    case 7:
      return 5;
    default:
      return 0;
  }
}

// Silence after which a reply with unknown length is considered complete.
// UART data is delivered every UART_RX_FIFO_FULL bytes, so the gap between
// deliveries of one continuous frame can be ~that many character times.
uint32_t replyGapUs() {
  return (UART_RX_FIFO_FULL + 4) * charTimeUs + 2000;
}

// ===================== Line Coding =====================
uint32_t toSerialConfig(const LineCoding &lc) {
  // Mark/space parity isn't supported by the ESP32 UART: fall back to none.
  uint8_t par = (lc.parity == 1 || lc.parity == 2) ? lc.parity : 0;
  bool twoStop = (lc.stopBits != 0);  // 1.5 is rounded up to 2

#define PICK(bits)                                                     \
  if (par == 0) return twoStop ? SERIAL_##bits##N2 : SERIAL_##bits##N1; \
  if (par == 1) return twoStop ? SERIAL_##bits##O2 : SERIAL_##bits##O1; \
  return twoStop ? SERIAL_##bits##E2 : SERIAL_##bits##E1;

  switch (lc.dataBits) {
    case 5: PICK(5)
    case 6: PICK(6)
    case 7: PICK(7)
    default: PICK(8)
  }
#undef PICK
  return SERIAL_8N1;  // unreachable
}

String lineCodingText(const LineCoding &lc) {
  const char parChar[] = { 'N', 'O', 'E', 'M', 'S' };
  String s = String(lc.baud) + " " + String(lc.dataBits);
  s += (lc.parity <= 4) ? parChar[lc.parity] : '?';
  s += (lc.stopBits == 0) ? "1" : (lc.stopBits == 1 ? "1.5" : "2");
  return s;
}

void onUartError(hardwareSerial_error_t err) {
  portENTER_CRITICAL(&statsMux);
  switch (err) {
    case UART_FRAME_ERROR: gstats.uartFrameErrors++; break;
    case UART_PARITY_ERROR: gstats.uartParityErrors++; break;
    case UART_BREAK_ERROR: gstats.uartBreaks++; break;
    case UART_BUFFER_FULL_ERROR:
    case UART_FIFO_OVF_ERROR: gstats.uartOverflows++; break;
    default: break;
  }
  portEXIT_CRITICAL(&statsMux);
}

void beginRs485(const LineCoding &lc) {
  activeBaud = lc.baud;
  uint8_t bits = 1 + lc.dataBits + ((lc.parity == 1 || lc.parity == 2) ? 1 : 0) + (lc.stopBits ? 2 : 1);
  charTimeUs = (uint32_t)((bits * 1000000.0) / lc.baud);

  Serial1.setRxBufferSize(UART_RX_BUFFER_SIZE);
  Serial1.begin(lc.baud, toSerialConfig(lc), RXD2, TXD2);
  // Deliver received bytes after 1 symbol of line idle, or every
  // UART_RX_FIFO_FULL bytes, instead of waiting for a full FIFO.
  Serial1.setRxTimeout(1);
  Serial1.setRxFIFOFull(UART_RX_FIFO_FULL);
  Serial1.onReceiveError(onUartError);

  while (Serial1.available()) Serial1.read();
  lastBusActivityUs = micros();
}

void restartRs485(const LineCoding &lc) {
  Serial1.end();
  beginRs485(lc);
}

void applyLineCoding(const LineCoding &lc) {
  if (lc.baud == activeCoding.baud && lc.dataBits == activeCoding.dataBits &&
      lc.parity == activeCoding.parity && lc.stopBits == activeCoding.stopBits) {
    return;
  }

  if (lc.parity > 2) ledErrUntil = millis() + 500;

  // DE is already LOW here (only called between transfers).
  restartRs485(lc);
  activeCoding = lc;
  lineCodingFromHost = true;

  bool console = (lc.baud == CONSOLE_BAUD);
  if (console && bridgeMode != MODE_CONSOLE) {
    bridgeMode = MODE_CONSOLE;
    logKeyEvent("USB CONSOLE MODE (%u baud)", (unsigned)CONSOLE_BAUD);
    consoleBanner();
  } else if (!console && bridgeMode == MODE_CONSOLE) {
    bridgeMode = MODE_BRIDGE;
  }

  if (!console) {
    logKeyEvent("PORT SETTINGS FROM HOST: %s", lineCodingText(lc).c_str());
  }
}

#if FOLLOW_HOST_LINE_CODING
// Runs in the TinyUSB task: record only, bridgeTask applies it.
void usbEventCallback(void *arg, esp_event_base_t base, int32_t id, void *eventData) {
  if (base != ARDUINO_USB_CDC_EVENTS) return;
  arduino_usb_cdc_event_data_t *data = (arduino_usb_cdc_event_data_t *)eventData;

  if (id == ARDUINO_USB_CDC_LINE_CODING_EVENT) {
    LineCoding lc;
    lc.baud = data->line_coding.bit_rate;
    lc.dataBits = data->line_coding.data_bits;
    lc.parity = data->line_coding.parity;
    lc.stopBits = data->line_coding.stop_bits;

    if (lc.baud < MIN_BAUD || lc.baud > MAX_BAUD) return;
    if (lc.dataBits < 5 || lc.dataBits > 8) lc.dataBits = 8;

    portENTER_CRITICAL(&lineCodingMux);
    pendingCoding = lc;
    lineCodingPending = true;
    portEXIT_CRITICAL(&lineCodingMux);
  } else if (id == ARDUINO_USB_CDC_RX_OVERFLOW_EVENT) {
    usbRxOverflow = true;
  }
}
#endif

void applyPendingLineCoding() {
#if FOLLOW_HOST_LINE_CODING
  if (!lineCodingPending) return;
  LineCoding lc;
  portENTER_CRITICAL(&lineCodingMux);
  lc = pendingCoding;
  lineCodingPending = false;
  portEXIT_CRITICAL(&lineCodingMux);
  applyLineCoding(lc);
#endif
}

// ===================== Transaction analyzer (observe only) =====================
void txLogAdd(uint8_t result, uint8_t slave, uint8_t fc, uint8_t exc, uint32_t latUs,
              const uint8_t *req, uint16_t rqLen, const uint8_t *rsp, uint16_t rsLen) {
  // Caller holds statsMux.
  TxLogEntry &e = txLog[txLogHead];
  e.ms = millis();
  e.result = result;
  e.slave = slave;
  e.fc = fc;
  e.excCode = exc;
  e.latencyUs = latUs;
  e.reqLen = rqLen;
  e.rspLen = rsLen;
  memcpy(e.req, req, min((int)rqLen, TXLOG_REQ_BYTES));
  if (rsp && rsLen) memcpy(e.rsp, rsp, min((int)rsLen, TXLOG_RSP_BYTES));
  txLogHead = (txLogHead + 1) % TXLOG_SIZE;
  txLogTotal++;
}

// Looks for a valid reply frame at offset 0, then 1 (a single stray byte
// in front of a reply is a classic transceiver-switching glitch).
bool findReplyFrame(const uint8_t *buf, uint16_t len, uint8_t fc, uint16_t &off, uint16_t &frameLen) {
  for (uint16_t o = 0; o <= 1 && o < len; o++) {
    int e = expectedReplyLen(buf + o, len - o, fc);
    if (e < 0) continue;
    uint16_t L = (e > 0) ? (uint16_t)e : (uint16_t)(len - o);
    if (L <= len - o && frameCrcOk(buf + o, L)) {
      off = o;
      frameLen = L;
      return true;
    }
  }
  return false;
}

void finalizeTransaction() {
  if (!awaiting) return;
  awaiting = false;

  uint8_t result;
  uint8_t exc = 0;
  uint32_t latUs = 0;
  uint16_t off = 0, frameLen = 0;
  bool glitch = false;
  uint16_t trailing = 0;

  if (rspLen == 0) {
    result = TR_TIMEOUT;
  } else if (!findReplyFrame(rspBuf, rspLen, reqFc, off, frameLen)) {
    result = TR_CRC;
  } else {
    glitch = (off == 1);
    trailing = rspLen - off - frameLen;
    const uint8_t *p = rspBuf + off;
    if (p[0] != reqSlave) {
      result = TR_WRONG_SLAVE;
    } else if (p[1] == (uint8_t)(reqFc | 0x80)) {
      result = TR_EXCEPTION;
      exc = p[2];
    } else if (p[1] != reqFc) {
      result = TR_BAD_FUNCTION;
    } else {
      result = TR_OK;
    }
    // Reply start = last delivery - (reply length + 1 char RX timeout).
    uint32_t span = lastRxUs - txEndUs;
    uint32_t onWire = (uint32_t)(rspLen + 1) * charTimeUs;
    latUs = (span > onWire) ? span - onWire : 0;
  }

  uint32_t nowMs = millis();
  portENTER_CRITICAL(&statsMux);
  SlaveStats &s = slaveStats[reqSlave];
  switch (result) {
    case TR_TIMEOUT:
      if (s.consecTimeouts == 0) s.okBeforeOutage = s.ok;
      s.timeouts++;
      if (s.consecTimeouts < 0xFFFF) s.consecTimeouts++;
      gstats.timeouts++;
      break;
    case TR_CRC:
      s.crcErrors++;
      gstats.crcErrors++;
      break;
    case TR_WRONG_SLAVE:
      s.wrongSlave++;
      gstats.wrongSlave++;
      break;
    case TR_BAD_FUNCTION:
      s.badFunction++;
      gstats.badFunction++;
      break;
    case TR_EXCEPTION:
      s.exceptions++;
      gstats.exceptions++;
      if (exc < 16) s.excMask |= (1 << exc);
      s.lastExc = exc;
      if (exc < EXC_CODE_SLOTS) gstats.excCodeCounts[exc]++;
      break;
    case TR_OK:
      s.ok++;
      gstats.ok++;
      break;
  }
  if (result == TR_OK || result == TR_EXCEPTION || result == TR_BAD_FUNCTION) {
    s.consecTimeouts = 0;
    s.lastReplyMs = nowMs;
    s.latCount++;
    s.latSumUs += latUs;
    if (s.latCount == 1 || latUs < s.latMinUs) s.latMinUs = latUs;
    if (latUs > s.latMaxUs) s.latMaxUs = latUs;
  }
  if (glitch) gstats.glitchBytes++;
  gstats.noiseBytes += trailing;
  txLogAdd(result, reqSlave, reqFc, exc, latUs, reqBuf, reqLen, rspBuf, rspLen);
  portEXIT_CRITICAL(&statsMux);

  if (result == TR_TIMEOUT) {
    lastTimeoutSlave = reqSlave;
    lastTimeoutMs = nowMs;
  }
}

// Bytes that arrived while nothing was pending, once the line went quiet.
void processUnsolicited() {
  if (unsLen == 0) return;

  bool valid = frameCrcOk(unsBuf, unsLen);
  uint32_t nowMs = millis();

  portENTER_CRITICAL(&statsMux);
  if (valid && lastTimeoutSlave != 0 && unsBuf[0] == lastTimeoutSlave &&
      nowMs - lastTimeoutMs < LATE_REPLY_WINDOW_MS) {
    slaveStats[lastTimeoutSlave].lateReplies++;
    gstats.lateReplies++;
    txLogAdd(TR_LATE, unsBuf[0], unsBuf[1], 0, 0, unsBuf, 0, unsBuf, unsLen);
  } else if (valid) {
    gstats.foreignFrames++;
    txLogAdd(TR_FOREIGN, unsBuf[0], unsBuf[1], 0, 0, unsBuf, 0, unsBuf, unsLen);
  } else {
    gstats.noiseBytes += unsLen;
  }
  portEXIT_CRITICAL(&statsMux);

  unsLen = 0;
}

void analyzerOnRx(const uint8_t *buf, int n, uint32_t nowUs) {
  portENTER_CRITICAL(&statsMux);
  gstats.rxBytes += n;
  gstats.lastActivityMs = millis();
  portEXIT_CRITICAL(&statsMux);

  if (awaiting) {
    int room = FRAME_BUF_SIZE - rspLen;
    int take = min(n, room);
    memcpy(rspBuf + rspLen, buf, take);
    rspLen += take;
    lastRxUs = nowUs;

    uint16_t off, frameLen;
    if (findReplyFrame(rspBuf, rspLen, reqFc, off, frameLen) && expectedReplyLen(rspBuf + off, rspLen - off, reqFc) > 0) {
      finalizeTransaction();  // complete, known-length reply
    } else if (rspLen >= FRAME_BUF_SIZE) {
      finalizeTransaction();
    }
  } else {
    int room = FRAME_BUF_SIZE - unsLen;
    int take = min(n, room);
    memcpy(unsBuf + unsLen, buf, take);
    unsLen += take;
    unsLastUs = nowUs;
    if (unsLen >= FRAME_BUF_SIZE) processUnsolicited();
  }
}

void analyzerOnRequest(uint32_t nowUs) {
  uint32_t nowMs = millis();

  portENTER_CRITICAL(&statsMux);
  gstats.txBytes += reqLen;
  gstats.lastActivityMs = nowMs;

  if (reqOverflow || !frameCrcOk(reqBuf, reqLen)) {
    gstats.nonRtuRequests++;
    txLogAdd(TR_NON_RTU, reqLen ? reqBuf[0] : 0, reqLen > 1 ? reqBuf[1] : 0, 0, 0, reqBuf, reqLen, NULL, 0);
    portEXIT_CRITICAL(&statsMux);
    return;
  }

  if (reqBuf[0] == 0) {
    gstats.broadcasts++;
    txLogAdd(TR_BROADCAST, 0, reqBuf[1], 0, 0, reqBuf, reqLen, NULL, 0);
    portEXIT_CRITICAL(&statsMux);
    return;
  }

  reqSlave = reqBuf[0];
  reqFc = reqBuf[1];
  slaveStats[reqSlave].requests++;
  slaveStats[reqSlave].lastRequestMs = nowMs;
  gstats.requests++;
  portEXIT_CRITICAL(&statsMux);

  awaiting = true;
  txEndUs = nowUs;
  rspLen = 0;
  lastRxUs = nowUs;
}

void analyzerTick(uint32_t nowUs) {
  if (awaiting) {
    if (rspLen == 0) {
      if (nowUs - txEndUs > diagResponseTimeoutMs * 1000UL) finalizeTransaction();
    } else if (nowUs - lastRxUs > replyGapUs()) {
      finalizeTransaction();
    }
  }
  if (unsLen > 0 && nowUs - unsLastUs > replyGapUs()) {
    processUnsolicited();
  }
}

void analyzerAbort() {
  awaiting = false;
  rspLen = 0;
  unsLen = 0;
}

// ===================== Bus idle level =====================
// With DE low and nothing on the wire, a correctly wired and biased bus
// makes the transceiver's RO output idle HIGH. Stuck LOW = A/B swapped or
// no fail-safe bias; flipping = floating bus picking up noise.
void sampleIdleLine(uint32_t nowUs) {
  static uint32_t lastSampleMs = 0;
  static uint8_t hist[4];
  static uint8_t histCount = 0, histPos = 0;

  uint32_t nowMs = millis();
  if (nowMs - lastSampleMs < IDLE_SAMPLE_INTERVAL_MS) return;
  if (awaiting || unsLen > 0) return;
  uint32_t quietUs = max((uint32_t)50000, 20 * charTimeUs);
  if (nowUs - lastBusActivityUs < quietUs) return;
  lastSampleMs = nowMs;

  uint8_t high = 0;
  for (int i = 0; i < 16; i++) {
    high += gpio_get_level(RS485_RX_GPIO) ? 1 : 0;
    delayMicroseconds(50);
  }
  if (Serial1.available() > 0) return;  // a frame started mid-sample: discard

  hist[histPos] = high;
  histPos = (histPos + 1) % 4;
  if (histCount < 4) histCount++;

  bool allHigh = true, allLow = true;
  for (int i = 0; i < histCount; i++) {
    if (hist[i] != 16) allHigh = false;
    if (hist[i] != 0) allLow = false;
  }
  idleLevel = allHigh ? IDLE_HIGH : (allLow ? IDLE_LOW : IDLE_UNSTABLE);
}

// ===================== Data Path =====================
// RS485 -> host (also feeds the analyzer).
bool pumpRs485ToUsb() {
  int avail = Serial1.available();
  if (avail <= 0) return false;

  while (avail > 0) {
    int n = Serial1.read(chunk, min(avail, (int)CHUNK_SIZE));
    if (n <= 0) break;
    Serial.write(chunk, n);
    analyzerOnRx(chunk, n, micros());
    avail = Serial1.available();
  }

  lastBusActivityUs = micros();
  ledRxUntil = millis() + LED_FLASH_MS;
  return true;
}

// Host -> RS485. Holds DE for the whole USB burst; returns true if it sent.
bool pumpUsbToRs485() {
  if (Serial.available() <= 0) return false;

#if ENFORCE_T35_GAP
  uint32_t t35 = modbusT35Us();
  uint32_t since = micros() - lastBusActivityUs;
  if (since < t35) delayMicroseconds(t35 - since);
#endif

  // Anything that arrived from the bus before we take it over belongs to
  // the previous exchange - forward it to the host first, don't lose it.
  pumpRs485ToUsb();

  // A new request closes whatever the previous one was still waiting for.
  finalizeTransaction();
  processUnsolicited();

  reqLen = 0;
  reqOverflow = false;

  preTransmission();

  for (;;) {
    int avail;
    while ((avail = Serial.available()) > 0) {
      int n = Serial.read(chunk, min(avail, (int)CHUNK_SIZE));
      if (n <= 0) break;
      Serial1.write(chunk, n);

      int take = min(n, (int)(FRAME_BUF_SIZE - reqLen));
      if (take < n) reqOverflow = true;
      memcpy(reqBuf + reqLen, chunk, take);
      reqLen += take;
    }
    Serial1.flush();  // blocks until the last stop bit has left the UART
    // A frame split across two USB packets: keep DE asserted and continue
    // instead of releasing the bus mid-frame.
    if (Serial.available() <= 0) break;
  }

  postTransmission();
  uint32_t now = micros();
  lastBusActivityUs = now;
  ledTxUntil = millis() + LED_FLASH_MS;

  analyzerOnRequest(now);
  return true;
}

// ===================== Auto-scan (bridgeTask owns the bus) =====================
ScanRequest scanReq;  // bridgeTask's private copy
bool scanComboApplied = false;
bool scanHitInCombo = false;
uint8_t scanComboIdx = 0;
uint16_t scanId = 0;

void scanUpdateStatus(uint8_t state) {
  portENTER_CRITICAL(&scanMux);
  scanStatus.state = state;
  scanStatus.comboIndex = scanComboIdx;
  scanStatus.currentId = (uint8_t)min((int)scanId, 255);
  if (state != SCAN_RUNNING) scanStatus.finishedMs = millis();
  portEXIT_CRITICAL(&scanMux);
}

void scanFinish(uint8_t state) {
  scanUpdateStatus(state);
  restartRs485(activeCoding);
  bridgeMode = (activeCoding.baud == CONSOLE_BAUD) ? MODE_CONSOLE : MODE_BRIDGE;

  uint8_t found;
  portENTER_CRITICAL(&scanMux);
  found = scanStatus.resultCount;
  portEXIT_CRITICAL(&scanMux);
  logKeyEvent("SCAN %s: %u device(s) found", state == SCAN_DONE ? "FINISHED" : "STOPPED", (unsigned)found);
}

void scanStart() {
  portENTER_CRITICAL(&scanMux);
  scanReq = scanRequest;
  scanStartPending = false;
  scanStopPending = false;
  memset(&scanStatus, 0, sizeof(scanStatus));
  scanStatus.state = SCAN_RUNNING;
  scanStatus.probesTotal = (uint32_t)scanReq.comboCount * (scanReq.idTo - scanReq.idFrom + 1);
  scanStatus.startedMs = millis();
  portEXIT_CRITICAL(&scanMux);

  analyzerAbort();
  postTransmission();
  bridgeMode = MODE_SCAN;
  scanComboIdx = 0;
  scanComboApplied = false;
  logKeyEvent("SCAN STARTED: IDs %u-%u, %u setting(s)", scanReq.idFrom, scanReq.idTo, scanReq.comboCount);
}

void scanDiscardHostBytes() {
  uint32_t dropped = 0;
  int avail;
  while ((avail = Serial.available()) > 0) {
    int n = Serial.read(chunk, min(avail, (int)CHUNK_SIZE));
    if (n <= 0) break;
    dropped += n;
  }
  if (dropped) {
    portENTER_CRITICAL(&statsMux);
    gstats.scanDroppedBytes += dropped;
    portEXIT_CRITICAL(&statsMux);
    portENTER_CRITICAL(&scanMux);
    scanStatus.droppedHostBytes += dropped;
    portEXIT_CRITICAL(&scanMux);
  }
}

// One probe per call so a stop request is honoured within one timeout.
void scanStep() {
  scanDiscardHostBytes();

  if (scanStopPending) {
    scanStopPending = false;
    scanFinish(SCAN_STOPPED);
    return;
  }

  if (scanComboIdx >= scanReq.comboCount) {
    scanFinish(SCAN_DONE);
    return;
  }

  const ScanCombo &c = scanReq.combos[scanComboIdx];
  if (!scanComboApplied) {
    LineCoding lc = { c.baud, 8, c.parity, c.stopBits };
    restartRs485(lc);
    scanComboApplied = true;
    scanHitInCombo = false;
    scanId = scanReq.idFrom;
    vTaskDelay(pdMS_TO_TICKS(20));  // let a slave that saw garbage resync
    while (Serial1.available()) Serial1.read();
  }

  // ---- send one probe: read 1 register at addr ----
  uint8_t req[8];
  req[0] = (uint8_t)scanId;
  req[1] = scanReq.fc;
  req[2] = scanReq.addr >> 8;
  req[3] = scanReq.addr & 0xFF;
  req[4] = 0;
  req[5] = 1;
  uint16_t crc = modbusCRC16(req, 6);
  req[6] = crc & 0xFF;
  req[7] = crc >> 8;

  uint32_t gap = modbusT35Us() + 2000;
  uint32_t since = micros() - lastBusActivityUs;
  if (since < gap) delayMicroseconds(gap - since);
  while (Serial1.available()) Serial1.read();

  uint32_t errBefore;
  portENTER_CRITICAL(&statsMux);
  errBefore = gstats.uartFrameErrors + gstats.uartParityErrors;
  portEXIT_CRITICAL(&statsMux);

  preTransmission();
  Serial1.write(req, sizeof(req));
  Serial1.flush();
  postTransmission();
  uint32_t t0 = micros();
  ledTxUntil = millis() + LED_FLASH_MS;

  uint8_t rx[FRAME_BUF_SIZE];
  uint16_t n = 0;
  uint32_t lastRx = t0;
  uint32_t timeoutUs = (uint32_t)scanReq.timeoutMs * 1000UL;
  bool complete = false;

  while (micros() - t0 < timeoutUs) {
    int a = Serial1.available();
    if (a > 0) {
      n += Serial1.read(rx + n, min(a, (int)(FRAME_BUF_SIZE - n)));
      lastRx = micros();
      int e = expectedReplyLen(rx, n, scanReq.fc);
      if ((e > 0 && n >= e) || n >= FRAME_BUF_SIZE) {
        complete = true;
        break;
      }
    } else if (n > 0 && micros() - lastRx > replyGapUs()) {
      break;
    } else {
      vTaskDelay(1);
    }
  }
  (void)complete;
  lastBusActivityUs = micros();

  uint32_t errAfter;
  portENTER_CRITICAL(&statsMux);
  errAfter = gstats.uartFrameErrors + gstats.uartParityErrors;
  portEXIT_CRITICAL(&statsMux);

  uint16_t off = 0, frameLen = 0;
  bool valid = (n > 0) && findReplyFrame(rx, n, scanReq.fc, off, frameLen) && rx[off] == (uint8_t)scanId;

  portENTER_CRITICAL(&scanMux);
  scanStatus.probesDone++;
  if (valid) {
    if (scanStatus.resultCount < SCAN_MAX_RESULTS) {
      ScanResult &r = scanStatus.results[scanStatus.resultCount++];
      r.id = (uint8_t)scanId;
      r.combo = c;
      r.excCode = (rx[off + 1] & 0x80) ? rx[off + 2] : 0;
      uint32_t span = lastRx - t0;
      uint32_t onWire = (uint32_t)(n + 1) * charTimeUs;
      r.latencyUs = span > onWire ? span - onWire : 0;
    }
  } else if (n > 0 || errAfter != errBefore) {
    scanStatus.garbledMask |= (1UL << scanComboIdx);
  }
  portEXIT_CRITICAL(&scanMux);

  if (valid) {
    scanHitInCombo = true;
    ledRxUntil = millis() + LED_FLASH_MS;
  }

  scanId++;
  if (scanId > scanReq.idTo) {
    if (scanReq.stopAtFirstHit && scanHitInCombo) {
      scanComboIdx = scanReq.comboCount;  // done after this setting
    } else {
      scanComboIdx++;
    }
    scanComboApplied = false;
  }
  scanUpdateStatus(SCAN_RUNNING);
}

// ===================== bridgeTask (Core 1) =====================
void bridgeTask(void *arg) {
  for (;;) {
    if (scanStartPending && bridgeMode != MODE_SCAN) {
      scanStart();
    }

    if (bridgeMode == MODE_SCAN) {
      scanStep();
      ledUpdate();
      continue;  // scanStep() blocks/yields inside its probe wait
    }

    applyPendingLineCoding();

    if (bridgeMode == MODE_CONSOLE) {
      consoleProcess();
      while (Serial1.available()) Serial1.read();
      ledUpdate();
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    bool busy = pumpRs485ToUsb();
    busy |= pumpUsbToRs485();

    uint32_t now = micros();
    analyzerTick(now);
    sampleIdleLine(now);

    if (usbRxOverflow) {
      usbRxOverflow = false;
      ledErrUntil = millis() + 200;
      portENTER_CRITICAL(&statsMux);
      gstats.usbOverflows++;
      portEXIT_CRITICAL(&statsMux);
    }

    ledUpdate();

    // Nothing moving: give the CPU back for one tick. UART/USB bytes keep
    // buffering in their drivers meanwhile, and DE is never held across this.
    if (!busy) vTaskDelay(1);
  }
}

// ===================== Setup / Loop =====================
void setup() {
  // Make the board's other hardware safe before anything else.
  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW);  // receive

  for (uint8_t i = 0; i < sizeof(DO_PINS); i++) {
    pinMode(DO_PINS[i], OUTPUT);
    digitalWrite(DO_PINS[i], LOW);
  }

  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);  // idle level: no power-key press

  ledInit();

  // Serial (USB CDC) is started by the core before setup().
  Serial.setRxBufferSize(USB_RX_BUFFER_SIZE);
  Serial.setTxTimeoutMs(USB_TX_TIMEOUT_MS);
  Serial.enableReboot(ALLOW_1200BAUD_REBOOT);

  memset(slaveStats, 0, sizeof(slaveStats));
  memset(&gstats, 0, sizeof(gstats));
  memset(&scanStatus, 0, sizeof(scanStatus));
  gstats.statsSinceMs = millis();

  logKeyEvent("BOOT (firmware %s)", FW_VERSION);

  loadAllConfig();  // web_ui.ino: auth, device/AP, WiFi, diagnostics

  LineCoding initial = { DEFAULT_BAUD, DEFAULT_DATA_BITS, DEFAULT_PARITY, DEFAULT_STOP_BITS };
  activeCoding = initial;
  beginRs485(initial);

#if FOLLOW_HOST_LINE_CODING
  Serial.onEvent(usbEventCallback);
  // If the host had already opened the port before we registered the
  // callback, pick up its current baud (parity unknown -> assume 8N1).
  uint32_t hostBaud = Serial.baudRate();
  if (hostBaud >= MIN_BAUD && hostBaud <= MAX_BAUD && hostBaud != DEFAULT_BAUD) {
    LineCoding lc = { hostBaud, 8, 0, 0 };
    applyLineCoding(lc);
  }
#endif

  setupWeb();  // web_ui.ino: WiFi AP/STA + WebServer routes

  xTaskCreatePinnedToCore(bridgeTask, "BridgeTask", BRIDGE_TASK_STACK, NULL,
                          BRIDGE_TASK_PRIORITY, &bridgeTaskHandle, BRIDGE_TASK_CORE);
  xTaskCreatePinnedToCore(webTask, "WebTask", WEB_TASK_STACK, NULL,
                          WEB_TASK_PRIORITY, &webTaskHandle, WEB_TASK_CORE);

  // Brief white flash = booted.
  ledErrUntil = ledRxUntil = ledTxUntil = millis() + 200;
}

// Intentionally idle: all work runs in bridgeTask (Core 1) / webTask (Core 0).
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
