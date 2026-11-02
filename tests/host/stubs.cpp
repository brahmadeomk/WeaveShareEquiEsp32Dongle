#include "Arduino.h"
unsigned long g_us = 1000000;
unsigned long millis(){return g_us/1000;} unsigned long micros(){return g_us;} void delay(unsigned long ms){g_us+=ms*1000;} void delayMicroseconds(unsigned u){g_us+=u;}
void pinMode(uint8_t,uint8_t){} void digitalWrite(uint8_t,uint8_t){}
esp_event_base_t ARDUINO_USB_CDC_EVENTS="cdc";
int Stream::available(){return 0;} int Stream::read(){return -1;} size_t Stream::read(uint8_t*,size_t){return 0;} size_t Stream::write(const uint8_t*,size_t n){return n;} void Stream::flush(){}
void Stream::println(const char*s){printf("%s\n",s);} void Stream::printf(const char*f,...){va_list a;va_start(a,f);vprintf(f,a);va_end(a);}
void HardwareSerial::begin(unsigned long,uint32_t,int8_t,int8_t){} void HardwareSerial::end(){} size_t HardwareSerial::setRxBufferSize(size_t n){return n;} bool HardwareSerial::setRxTimeout(uint8_t){return true;} bool HardwareSerial::setRxFIFOFull(uint8_t){return true;}
void HardwareSerial::onReceiveError(std::function<void(hardwareSerial_error_t)>){}
size_t USBCDC::setRxBufferSize(size_t n){return n;} void USBCDC::setTxTimeoutMs(uint32_t){} void USBCDC::enableReboot(bool){} void USBCDC::onEvent(esp_event_handler_t){} uint32_t USBCDC::baudRate(){return 9600;} USBCDC::operator bool() const {return true;}
USBCDC Serial; HardwareSerial Serial1;
void portENTER_CRITICAL(portMUX_TYPE*){} void portEXIT_CRITICAL(portMUX_TYPE*){}
void vTaskDelay(uint32_t){} uint32_t pdMS_TO_TICKS(uint32_t m){return m;}
int xTaskCreatePinnedToCore(void(*)(void*),const char*,uint32_t,void*,int,TaskHandle_t*,int){return 1;}
int gpio_get_level(gpio_num_t){return 1;} uint32_t esp_random(){return 4;}
uint64_t EspClass::getEfuseMac(){return 0x112233445566ULL;} uint32_t EspClass::getFreeHeap(){return 1;} void EspClass::restart(){} EspClass ESP;
String IPAddress::toString() const {return "192.168.4.1";}
int WiFiClass::status(){return 0;} IPAddress WiFiClass::localIP(){return {};} IPAddress WiFiClass::softAPIP(){return {};} int WiFiClass::RSSI(){return 0;} String WiFiClass::softAPmacAddress(){return "";}
bool WiFiClass::setHostname(const char*){return true;} bool WiFiClass::mode(int){return true;} void WiFiClass::setSleep(bool){} bool WiFiClass::softAP(const char*,const char*){return true;} void WiFiClass::begin(const char*,const char*){} bool WiFiClass::disconnect(){return true;} WiFiClass WiFi;
WebServer::WebServer(int){} String WebServer::header(const char*){return "";} void WebServer::sendHeader(const char*,const String&){} void WebServer::send(int,const char*,const String&){}
bool WebServer::hasArg(const char*){return false;} String WebServer::arg(const char*){return "";} void WebServer::on(const char*,HTTPMethod,void(*)()){} void WebServer::onNotFound(void(*)()){} void WebServer::collectHeaders(const char**,size_t){} void WebServer::begin(){} void WebServer::handleClient(){}
bool Preferences::begin(const char*,bool){return true;} void Preferences::end(){} size_t Preferences::putString(const char*,const String&v){return v.length();} size_t Preferences::putUInt(const char*,uint32_t){return 4;}
String Preferences::getString(const char*,const String&d){return d;} uint32_t Preferences::getUInt(const char*,uint32_t d){return d;} bool Preferences::clear(){return true;}
