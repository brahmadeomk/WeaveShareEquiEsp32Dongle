#pragma once
// Host-side stand-ins, only to type-check the sketch. Not a simulator.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <functional>
#include <algorithm>
using std::min; using std::max;
#define PROGMEM
#define FPSTR(p) (p)
#define IRAM_ATTR
template<class T, class L, class H> T constrain(T x, L lo, H hi){ return x<lo?lo:(x>hi?hi:x);}
class String {
 public: std::string s;
  String(): s(){} String(const char*c): s(c?c:""){} String(const std::string&x):s(x){}
  String(char c): s(1,c){} String(int v){s=std::to_string(v);} String(unsigned v){s=std::to_string(v);}
  String(long v){s=std::to_string(v);} String(unsigned long v){s=std::to_string(v);}
  String(long long v){s=std::to_string(v);} String(unsigned long long v){s=std::to_string(v);}
  String(float v, unsigned d=2){char b[32];snprintf(b,32,"%.*f",d,v);s=b;} String(double v, unsigned d=2){char b[32];snprintf(b,32,"%.*f",d,v);s=b;}
  unsigned length() const {return s.size();} const char* c_str() const {return s.c_str();}
  void reserve(unsigned n){s.reserve(n);} 
  String& operator+=(const String&o){s+=o.s;return *this;} String& operator+=(const char*o){s+=o;return *this;} String& operator+=(char c){s+=c;return *this;}
  bool operator==(const String&o) const {return s==o.s;} bool operator!=(const String&o) const {return s!=o.s;}
  bool operator==(const char*o) const {return s==o;} bool operator!=(const char*o) const {return s!=o;}
  char operator[](unsigned i) const {return s[i];}
  void replace(const char*a,const char*b){size_t p=0;std::string A(a),B(b);while((p=s.find(A,p))!=std::string::npos){s.replace(p,A.size(),B);p+=B.size();}}
  int indexOf(const char*a,unsigned from=0) const {auto p=s.find(a,from);return p==std::string::npos?-1:(int)p;}
  int indexOf(char a,unsigned from=0) const {auto p=s.find(a,from);return p==std::string::npos?-1:(int)p;}
  String substring(unsigned a) const {return String(s.substr(a));} String substring(unsigned a,unsigned b) const {return String(s.substr(a,b-a));}
  void trim(){} long toInt() const {return atol(s.c_str());}
};
inline String operator+(const String&a,const String&b){return String(a.s+b.s);} 
inline String operator+(const String&a,const char*b){return String(a.s+b);} inline String operator+(const char*a,const String&b){return String(std::string(a)+b.s);}
inline String operator+(const String&a,char b){return String(a.s+b);} 
unsigned long millis(); unsigned long micros(); void delay(unsigned long); void delayMicroseconds(unsigned);
void pinMode(uint8_t,uint8_t); void digitalWrite(uint8_t,uint8_t);
#define HIGH 1
#define LOW 0
#define OUTPUT 3
enum { D2=2,D3,D4,D5,D6,D7,D8,D9,D10,D11,D12,D13, A0=20,A1,A2,A3,A4,A5 };
// Same style as the real Nano ESP32 variant (pins_arduino.h): constexpr
// constants, NOT macros - so "#if defined(LED_RED)" is false here too.
static constexpr uint8_t LED_RED = 14;
static constexpr uint8_t LED_GREEN = 15;
static constexpr uint8_t LED_BLUE = 16;
extern uint8_t g_pinLevel[64];   // last digitalWrite() value per pin (tests)
#define ARDUINO_USB_CDC_ON_BOOT 1
#define ARDUINO_USB_MODE 0
enum { SERIAL_5N1,SERIAL_5N2,SERIAL_5O1,SERIAL_5O2,SERIAL_5E1,SERIAL_5E2,SERIAL_6N1,SERIAL_6N2,SERIAL_6O1,SERIAL_6O2,SERIAL_6E1,SERIAL_6E2,SERIAL_7N1,SERIAL_7N2,SERIAL_7O1,SERIAL_7O2,SERIAL_7E1,SERIAL_7E2,SERIAL_8N1,SERIAL_8N2,SERIAL_8O1,SERIAL_8O2,SERIAL_8E1,SERIAL_8E2 };
typedef enum { UART_NO_ERROR, UART_BREAK_ERROR, UART_BUFFER_FULL_ERROR, UART_FIFO_OVF_ERROR, UART_FRAME_ERROR, UART_PARITY_ERROR } hardwareSerial_error_t;
typedef const char* esp_event_base_t; extern esp_event_base_t ARDUINO_USB_CDC_EVENTS;
typedef void (*esp_event_handler_t)(void*, esp_event_base_t, int32_t, void*);
enum { ARDUINO_USB_CDC_CONNECTED_EVENT, ARDUINO_USB_CDC_DISCONNECTED_EVENT, ARDUINO_USB_CDC_LINE_STATE_EVENT, ARDUINO_USB_CDC_LINE_CODING_EVENT, ARDUINO_USB_CDC_RX_EVENT, ARDUINO_USB_CDC_TX_EVENT, ARDUINO_USB_CDC_RX_OVERFLOW_EVENT };
typedef union { struct { uint32_t bit_rate; uint8_t stop_bits; uint8_t parity; uint8_t data_bits; } line_coding; } arduino_usb_cdc_event_data_t;
struct Stream { int available(); int read(); size_t read(uint8_t*, size_t); size_t write(const uint8_t*, size_t); void flush();
  void println(const char* = ""); void printf(const char*, ...); };
struct HardwareSerial : Stream { void begin(unsigned long, uint32_t, int8_t, int8_t); void end(); size_t setRxBufferSize(size_t); bool setRxTimeout(uint8_t); bool setRxFIFOFull(uint8_t);
  void onReceiveError(std::function<void(hardwareSerial_error_t)>); };
struct USBCDC : Stream { size_t setRxBufferSize(size_t); void setTxTimeoutMs(uint32_t); void enableReboot(bool); void onEvent(esp_event_handler_t); uint32_t baudRate(); operator bool() const; };
extern USBCDC Serial; extern HardwareSerial Serial1;
// FreeRTOS / ESP
typedef int portMUX_TYPE; 
#define portMUX_INITIALIZER_UNLOCKED 0
void portENTER_CRITICAL(portMUX_TYPE*); void portEXIT_CRITICAL(portMUX_TYPE*);
typedef void* TaskHandle_t; void vTaskDelay(uint32_t); uint32_t pdMS_TO_TICKS(uint32_t);
#define configMAX_PRIORITIES 25
int xTaskCreatePinnedToCore(void(*)(void*), const char*, uint32_t, void*, int, TaskHandle_t*, int);
typedef int gpio_num_t; int gpio_get_level(gpio_num_t);
uint32_t esp_random();
struct EspClass { uint64_t getEfuseMac(); uint32_t getFreeHeap(); void restart(); }; extern EspClass ESP;
struct IPAddress { String toString() const; };
#define WL_CONNECTED 3
#define WIFI_AP_STA 3
struct WiFiClass { int status(); IPAddress localIP(); IPAddress softAPIP(); int RSSI(); String softAPmacAddress(); bool setHostname(const char*); bool mode(int); void setSleep(bool);
  bool softAP(const char*, const char*); void begin(const char*, const char*); bool disconnect(); }; extern WiFiClass WiFi;
enum HTTPMethod { HTTP_GET, HTTP_POST };
struct WebServer { WebServer(int); String header(const char*); void sendHeader(const char*, const String&); void send(int, const char* = nullptr, const String& = String());
  bool hasArg(const char*); String arg(const char*); void on(const char*, HTTPMethod, void(*)()); void onNotFound(void(*)()); void collectHeaders(const char**, size_t); void begin(); void handleClient(); };
struct Preferences { bool begin(const char*, bool); void end(); size_t putString(const char*, const String&); size_t putUInt(const char*, uint32_t);
  String getString(const char*, const String&); uint32_t getUInt(const char*, uint32_t); bool clear(); };
