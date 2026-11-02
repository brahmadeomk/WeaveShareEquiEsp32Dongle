// Shared types for the USB-RS485 dongle sketch.
//
// Kept in a header (not in the .ino) on purpose: the Arduino builder hoists
// auto-generated prototypes above the .ino's own type definitions, so any
// function whose signature uses a struct defined in an .ino can fail to
// compile (the same issue the Gateway sketch documents).
#pragma once
#include <Arduino.h>

// ===================== Line coding =====================
struct LineCoding {
  uint32_t baud;
  uint8_t dataBits;  // 5..8
  uint8_t parity;    // 0 none, 1 odd, 2 even, 3 mark, 4 space (CDC spec)
  uint8_t stopBits;  // 0 = 1, 1 = 1.5, 2 = 2 (CDC spec)
};

// ===================== Transaction analyzer =====================
// Result of one master request as seen on the bus.
enum TxResult : uint8_t {
  TR_OK = 0,        // valid reply, same slave + function
  TR_EXCEPTION,     // valid exception reply (fc | 0x80)
  TR_TIMEOUT,       // nothing came back
  TR_CRC,           // bytes came back but no valid frame (CRC/length)
  TR_WRONG_SLAVE,   // valid frame from a different slave ID
  TR_BAD_FUNCTION,  // valid frame, same slave, different function code
  TR_BROADCAST,     // request to ID 0, no reply expected
  TR_LATE,          // valid reply that arrived after it was given up on
  TR_FOREIGN,       // valid frame nobody here asked for (another master?)
  TR_NON_RTU        // host request without a valid RTU CRC (ASCII/other)
};

enum IdleLevel : uint8_t { IDLE_UNKNOWN = 0, IDLE_HIGH, IDLE_LOW, IDLE_UNSTABLE };

enum BridgeMode : uint8_t { MODE_BRIDGE = 0, MODE_SCAN, MODE_CONSOLE, MODE_SNIFF };

enum Severity : uint8_t { SEV_OK = 0, SEV_INFO, SEV_WARN, SEV_FAULT };

#define SLAVE_SLOTS 256  // indexed by slave ID; 1..247 are valid unicast IDs

struct SlaveStats {
  uint32_t requests;
  uint32_t ok;
  uint32_t timeouts;
  uint32_t crcErrors;
  uint32_t exceptions;
  uint32_t wrongSlave;
  uint32_t badFunction;
  uint32_t lateReplies;
  uint32_t latCount;
  uint32_t latMinUs;
  uint32_t latMaxUs;
  uint64_t latSumUs;
  uint32_t lastRequestMs;
  uint32_t lastReplyMs;  // last valid reply (OK or exception)
  uint32_t okBeforeOutage;  // ok count when the current timeout streak began
  uint16_t consecTimeouts;
  uint16_t excMask;  // bit n set = exception code n seen (1..15)
  uint8_t lastExc;
};

#define EXC_CODE_SLOTS 12  // exception codes 0..11

struct GlobalStats {
  uint32_t requests;      // valid RTU unicast requests
  uint32_t ok;
  uint32_t timeouts;
  uint32_t crcErrors;
  uint32_t exceptions;
  uint32_t wrongSlave;
  uint32_t badFunction;
  uint32_t broadcasts;
  uint32_t lateReplies;
  uint32_t foreignFrames;
  uint32_t noiseBytes;    // bytes that belonged to no valid frame
  uint32_t glitchBytes;   // single stray byte in front of an otherwise valid reply
  uint32_t nonRtuRequests;
  uint32_t uartFrameErrors;
  uint32_t uartParityErrors;
  uint32_t uartBreaks;
  uint32_t uartOverflows;
  uint32_t usbOverflows;
  uint32_t scanDroppedBytes;  // host bytes discarded while a scan owned the bus
  uint32_t txBytes;
  uint32_t rxBytes;
  uint32_t excCodeCounts[EXC_CODE_SLOTS];
  uint32_t lastActivityMs;
  uint32_t statsSinceMs;
};

#define TXLOG_SIZE 20
#define TXLOG_REQ_BYTES 16
#define TXLOG_RSP_BYTES 24

struct TxLogEntry {
  uint32_t ms;
  uint32_t latencyUs;
  uint16_t reqLen;
  uint16_t rspLen;
  uint8_t slave;
  uint8_t fc;
  uint8_t result;  // TxResult
  uint8_t excCode;
  uint8_t req[TXLOG_REQ_BYTES];
  uint8_t rsp[TXLOG_RSP_BYTES];
};

// ===================== Auto-scan =====================
#define SCAN_MAX_COMBOS 32
#define SCAN_MAX_RESULTS 64

struct ScanCombo {
  uint32_t baud;
  uint8_t parity;    // 0 N, 1 O, 2 E (CDC numbering)
  uint8_t stopBits;  // 0 = 1 stop bit, 2 = 2 stop bits
};

struct ScanRequest {
  ScanCombo combos[SCAN_MAX_COMBOS];
  uint8_t comboCount;
  uint8_t idFrom;
  uint8_t idTo;
  uint8_t fc;        // 3 or 4
  uint16_t addr;
  uint16_t timeoutMs;
  bool stopAtFirstHit;
};

enum ScanState : uint8_t { SCAN_IDLE = 0, SCAN_RUNNING, SCAN_DONE, SCAN_STOPPED };

struct ScanResult {
  ScanCombo combo;
  uint32_t latencyUs;
  uint8_t id;
  uint8_t excCode;  // 0 = normal reply
};

struct ScanStatus {
  uint8_t state;  // ScanState
  uint8_t comboIndex;
  uint8_t currentId;
  uint8_t resultCount;
  uint32_t probesDone;
  uint32_t probesTotal;
  uint32_t garbledMask;  // bit per combo: bytes seen but no valid frame
  uint32_t startedMs;
  uint32_t finishedMs;
  uint32_t droppedHostBytes;
  ScanResult results[SCAN_MAX_RESULTS];
};

// ===================== Sniffer (listen-only) =====================
#define SNIFF_LOG_SIZE 200
#define SNIFF_MAX_BYTES 64
#define SNIFF_DETECT_COMBOS 16

enum SniffKind : uint8_t {
  SK_REQUEST = 0,  // master -> slave
  SK_RESPONSE,     // slave -> master, normal reply
  SK_EXCEPTION,    // slave -> master, exception reply
  SK_FRAME,        // valid CRC, direction unknown (no matching request seen)
  SK_GARBAGE       // bytes that form no valid frame
};

enum SniffState : uint8_t { SNIFF_OFF = 0, SNIFF_DETECTING, SNIFF_RUNNING };

struct SniffEntry {
  uint64_t tUs;  // end of frame, microseconds since sniffing started
  uint32_t seq;
  uint16_t len;  // real length (bytes[] holds at most SNIFF_MAX_BYTES)
  uint8_t kind;  // SniffKind
  uint8_t bytes[SNIFF_MAX_BYTES];
};

struct SniffRequest {
  bool autoDetect;
  bool forwardUsb;
  ScanCombo combo;  // used when autoDetect is false
};

struct SniffStatus {
  uint8_t state;  // SniffState
  uint8_t detectIdx;
  uint8_t detectCycles;
  bool autoDetected;
  bool forwardUsb;
  ScanCombo combo;  // current listening setting
  uint32_t frames;
  uint32_t garbage;
  uint32_t bytes;
  uint32_t droppedHostBytes;
  uint32_t startedMs;
  uint32_t lastSeq;
  uint16_t detectScore[SNIFF_DETECT_COMBOS];
};

// ===================== Diagnosis (web side) =====================
#define MAX_FINDINGS 24

// Action button offered next to a finding's remedy on the web page.
enum FindingAction : uint8_t { ACT_NONE = 0, ACT_SCAN, ACT_RESET, ACT_SETTINGS, ACT_SNIFF };

struct Finding {
  uint8_t sev;     // Severity
  uint8_t action;  // FindingAction
  String title;
  String cause;    // what is most likely wrong
  String remedy;   // numbered steps, one per line ('\n' separated)
};

// ===================== SHA-256 (web login) =====================
struct Sha256Ctx {
  uint8_t data[64];
  uint32_t datalen;
  uint64_t bitlen;
  uint32_t state[8];
};
