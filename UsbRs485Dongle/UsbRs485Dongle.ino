/* ======================================================================
   USB <-> RS485 (Modbus RTU) CONVERTER  -  Arduino Nano ESP32
   ======================================================================
   Turns the ESP32 Modbus Gateway board into a transparent USB-to-RS485
   dongle, functionally equivalent to a Waveshare USB TO RS485 adapter:
   the PC sees a normal COM / ttyACM port, and any Modbus RTU master
   software (Modbus Poll, QModMaster, pymodbus, SCADA drivers, vendor
   configuration tools, ...) talks straight through it to the RS485 bus.

   HARDWARE - identical to the Gateway board (brahmadeomk/Gateway):
     RS485 transceiver   RX = D2, TX = D3, DE/RE = D4   (Serial1)
     Cellular modem      D5/D6 UART, A5 PWRKEY          (not used - idle)
     Digital Out 1-4     D11, D12, D13, A4              (held LOW = off)
     Digital In / Analog D7-D10, A0-A3                  (not used)

   ARCHITECTURE NOTES
   - The RS485 side reuses the Gateway's proven transceiver handling:
     DE/RE toggled inside a portENTER/EXIT_CRITICAL section, a 300us
     driver-enable settle before the first bit, Serial1.flush() (which
     blocks until the last stop bit is on the wire) and then an immediate,
     un-delayed release back to receive so the slave's reply is never
     clipped. Modbus T3.5 inter-frame silence is enforced before each
     transmission using the same spec formulas as the Gateway.
   - Line coding follows the host: whatever baud / data bits / parity /
     stop bits the PC software opens the COM port with is applied to the
     RS485 UART, exactly like a CH343/FT232 based dongle. The USB CDC
     line-coding callback runs in the TinyUSB task, so it only records the
     request; loop() applies it between transfers so a reconfiguration
     can never land in the middle of a frame.
   - Data path is byte-transparent (no Modbus parsing), so Modbus RTU,
     Modbus ASCII and any other half-duplex RS485 protocol pass through.
     Host -> bus: bytes are written while USB keeps delivering them, DE is
     held for the whole burst and released only once the UART is idle and
     no further USB data is pending. Bus -> host: UART bytes are forwarded
     to USB as they arrive (UART RX timeout = 1 symbol for low latency).
   - The USB port carries the user's data, so this firmware prints NO
     debug text on it. Status is shown on the onboard RGB LED instead:
       blue  flash = host -> RS485 transmit
       green flash = RS485 -> host receive
       red   flash = RX overflow / unsupported line coding (mark/space)
   - Pins wired to relays (DO1-4) are driven LOW and the modem PWRKEY is
     held at its idle level, so the same board can be re-flashed between
     Gateway and Dongle firmware without anything switching on.

   BUILD (Arduino IDE / arduino-cli)
     Board : "Arduino Nano ESP32" (arduino:esp32:nano_nora)
             or esp32:esp32:nano_nora from the Espressif core (2.0.x/3.x)
     USB   : default mode (TinyUSB CDC). The "Debug / Hardware CDC" USB
             mode cannot report line coding and is rejected at compile time.
     No external libraries required.
   ====================================================================== */

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT || (defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE)
#error "Select the Arduino Nano ESP32 with its default USB mode (TinyUSB CDC on boot). Hardware-CDC/debug mode cannot follow the host's baud/parity."
#endif


// ===================== Pins (same as Gateway board) =====================
#define RXD2 D2
#define TXD2 D3
#define RS485_DE_RE D4

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

// Limits accepted from the host. The ESP32-S3 UART is fine well past
// this, but typical RS485 transceivers (MAX485/SP3485) top out ~2.5Mbps.
#define MIN_BAUD 300
#define MAX_BAUD 2000000

// Transceiver driver-enable settle time after DE goes HIGH (from Gateway).
#define DE_SETTLE_US 300

// Enforce Modbus RTU T3.5 bus silence before transmitting. Harmless for
// other protocols; set 0 for a purely byte-level converter.
#define ENFORCE_T35_GAP 1

// Arduino's 1200-baud "touch" reboots the board into the bootloader so
// the IDE can upload. Keep enabled for easy re-flashing; set to 0 only if
// you really need to run the RS485 bus at 1200 baud (then use the
// double-tap-reset bootloader for uploads).
#define ALLOW_1200BAUD_REBOOT 1

#define UART_RX_BUFFER_SIZE 2048
#define USB_RX_BUFFER_SIZE 2048
#define USB_TX_TIMEOUT_MS 20       // drop instead of stalling if host stops reading
#define CHUNK_SIZE 256

#define LED_FLASH_MS 25

// ===================== State =====================
portMUX_TYPE rs485Mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE lineCodingMux = portMUX_INITIALIZER_UNLOCKED;

struct LineCoding {
  uint32_t baud;
  uint8_t dataBits;  // 5..8
  uint8_t parity;    // 0 none, 1 odd, 2 even, 3 mark, 4 space (CDC spec)
  uint8_t stopBits;  // 0 = 1, 1 = 1.5, 2 = 2 (CDC spec)
};

volatile bool lineCodingPending = false;
LineCoding pendingCoding;  // guarded by lineCodingMux
LineCoding activeCoding = { DEFAULT_BAUD, DEFAULT_DATA_BITS, DEFAULT_PARITY, DEFAULT_STOP_BITS };
uint32_t activeBaud = DEFAULT_BAUD;

uint32_t lastBusActivityUs = 0;
uint8_t chunk[CHUNK_SIZE];

volatile bool usbRxOverflow = false;

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

void ledUpdate() {
#if HAS_RGB_LED
  static uint8_t shown = 0xFF;
  unsigned long now = millis();
  uint8_t want = ((long)(ledErrUntil - now) > 0 ? 1 : 0) |
                 ((long)(ledRxUntil - now) > 0 ? 2 : 0) |
                 ((long)(ledTxUntil - now) > 0 ? 4 : 0);
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

// ===================== Modbus RTU timing (from Gateway) =====================
uint32_t modbusCharTimeUs() {
  return (uint32_t)((11.0 * 1000000.0) / activeBaud);
}

uint32_t modbusT35Us() {
  return (activeBaud > 19200) ? 1750 : (modbusCharTimeUs() * 7 / 2);
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

void beginRs485(const LineCoding &lc) {
  activeBaud = lc.baud;
  Serial1.setRxBufferSize(UART_RX_BUFFER_SIZE);
  Serial1.begin(lc.baud, toSerialConfig(lc), RXD2, TXD2);
  // Deliver received bytes after 1 symbol of line idle instead of waiting
  // for the FIFO to fill - keeps bus->USB latency low at slow baud rates.
  Serial1.setRxTimeout(1);

  while (Serial1.available()) Serial1.read();
  lastBusActivityUs = micros();
}

void applyLineCoding(const LineCoding &lc) {
  if (lc.baud == activeCoding.baud && lc.dataBits == activeCoding.dataBits &&
      lc.parity == activeCoding.parity && lc.stopBits == activeCoding.stopBits) {
    return;
  }

  if (lc.parity > 2) ledErrUntil = millis() + 500;

  // DE is already LOW here (loop() only calls this between transfers).
  Serial1.end();
  beginRs485(lc);
  activeCoding = lc;
}

#if FOLLOW_HOST_LINE_CODING
// Runs in the TinyUSB task: record only, loop() applies it.
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

// ===================== Data Path =====================
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
  int pre = Serial1.available();
  while (pre > 0) {
    int n = Serial1.read(chunk, min(pre, (int)CHUNK_SIZE));
    if (n <= 0) break;
    Serial.write(chunk, n);
    pre -= n;
  }

  preTransmission();

  for (;;) {
    int avail;
    while ((avail = Serial.available()) > 0) {
      int n = Serial.read(chunk, min(avail, (int)CHUNK_SIZE));
      if (n <= 0) break;
      Serial1.write(chunk, n);
    }
    Serial1.flush();  // blocks until the last stop bit has left the UART
    // A frame split across two USB packets: keep DE asserted and continue
    // instead of releasing the bus mid-frame.
    if (Serial.available() <= 0) break;
  }

  postTransmission();
  lastBusActivityUs = micros();
  ledTxUntil = millis() + LED_FLASH_MS;
  return true;
}

// RS485 -> host.
bool pumpRs485ToUsb() {
  int avail = Serial1.available();
  if (avail <= 0) return false;

  while (avail > 0) {
    int n = Serial1.read(chunk, min(avail, (int)CHUNK_SIZE));
    if (n <= 0) break;
    Serial.write(chunk, n);
    avail = Serial1.available();
  }

  lastBusActivityUs = micros();
  ledRxUntil = millis() + LED_FLASH_MS;
  return true;
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

  // Brief white flash = booted.
  ledErrUntil = ledRxUntil = ledTxUntil = millis() + 200;
}

void loop() {
#if FOLLOW_HOST_LINE_CODING
  if (lineCodingPending) {
    LineCoding lc;
    portENTER_CRITICAL(&lineCodingMux);
    lc = pendingCoding;
    lineCodingPending = false;
    portEXIT_CRITICAL(&lineCodingMux);
    applyLineCoding(lc);
  }
#endif

  bool busy = pumpRs485ToUsb();
  busy |= pumpUsbToRs485();

  if (usbRxOverflow) {
    usbRxOverflow = false;
    ledErrUntil = millis() + 200;
  }

  ledUpdate();

  // Nothing moving: give the CPU back for one tick. UART/USB bytes keep
  // buffering in their drivers meanwhile, and DE is never held across this.
  if (!busy) vTaskDelay(1);
}
