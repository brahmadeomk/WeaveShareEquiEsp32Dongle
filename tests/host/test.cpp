#include "sketch.cpp"
extern unsigned long g_us;
int fails=0;
#define CHECK(c) do{ if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c);fails++;} }while(0)
void frame(std::vector<uint8_t> v, uint8_t* out, uint16_t& n){ uint16_t c=modbusCRC16(v.data(),v.size()); v.push_back(c&0xFF); v.push_back(c>>8); memcpy(out,v.data(),v.size()); n=v.size(); }
void sendReq(std::vector<uint8_t> v){ frame(v,reqBuf,reqLen); reqOverflow=false; finalizeTransaction(); processUnsolicited(); analyzerOnRequest(micros()); }
void rx(std::vector<uint8_t> v, bool crc=true, int splitAt=-1){ uint8_t b[300]; uint16_t n; if(crc) frame(v,b,n); else {memcpy(b,v.data(),v.size()); n=v.size();}
  g_us += 5000 + (n+1)*charTimeUs; // 5 ms latency + bytes on the wire + 1 char RX timeout
  if(splitAt>0){ analyzerOnRx(b,splitAt,micros()); g_us+=2000; analyzerOnRx(b+splitAt,n-splitAt,micros()); } else analyzerOnRx(b,n,micros()); }
void idle(uint32_t ms){ g_us += ms*1000; analyzerTick(micros()); }
int main(){
  activeCoding={9600,8,0,0}; beginRs485(activeCoding);
  // 1) OK FC3 reply, split delivery
  sendReq({1,3,0,0,0,2}); rx({1,3,4,0,1,0,2},true,3);
  CHECK(slaveStats[1].ok==1); CHECK(!awaiting);
  CHECK(slaveStats[1].latMinUs>=4000 && slaveStats[1].latMinUs<=8000); printf("  latency measured: %u us (true 5000 + split delay)\n", slaveStats[1].latMinUs);
  // 2) exception 02
  sendReq({1,3,0x9C,0x40,0,1}); rx({1,0x83,2}); CHECK(slaveStats[1].exceptions==1); CHECK(slaveStats[1].excMask & 4);
  // 3) timeout via diag timeout
  sendReq({2,3,0,0,0,1}); idle(1100); CHECK(slaveStats[2].timeouts==1); CHECK(!awaiting);
  // 4) late reply after timeout
  rx({2,3,2,0,7}); idle(50); CHECK(slaveStats[2].lateReplies==1); CHECK(gstats.foreignFrames==0);
  // 5) timeout via next request (host retry)
  sendReq({3,4,0,0,0,1}); g_us+=200000; sendReq({3,4,0,0,0,1}); CHECK(slaveStats[3].timeouts==1);
  rx({3,4,2,0x12,0x34}); CHECK(slaveStats[3].ok==1);
  // 6) CRC error
  sendReq({4,3,0,0,0,1}); rx({4,3,2,0,1,0x55,0x55},false); idle(100); CHECK(slaveStats[4].crcErrors==1);
  // 7) glitch byte in front of valid reply
  { sendReq({5,6,0,1,0,9}); uint8_t b[16]; uint16_t n; frame({5,6,0,1,0,9},b+1,n); b[0]=0x00; g_us+=3000; analyzerOnRx(b,n+1,micros()); idle(100);
    CHECK(slaveStats[5].ok==1); CHECK(gstats.glitchBytes==1); }
  // 8) wrong slave
  sendReq({6,3,0,0,0,1}); rx({7,3,2,0,1}); CHECK(slaveStats[6].wrongSlave==1);
  // 9) foreign frame while idle
  idle(10); rx({9,3,0,0,0,1}); idle(100); CHECK(gstats.foreignFrames==1);
  // 10) broadcast + non-RTU
  sendReq({0,6,0,1,0,1}); CHECK(gstats.broadcasts==1);
  { const char* a=":010300000001FB\r\n"; reqLen=strlen(a); memcpy(reqBuf,a,reqLen); reqOverflow=false; analyzerOnRequest(micros()); CHECK(gstats.nonRtuRequests==1); }
  // 11) unknown FC (8) framed by silence
  sendReq({1,8,0,0,0x12,0x34}); rx({1,8,0,0,0x12,0x34}); CHECK(awaiting); idle(100); CHECK(!awaiting); CHECK(slaveStats[1].ok==2);
  // 12) never-answering slave -> FAULT finding
  for(int i=0;i<5;i++){ sendReq({10,3,0,0,0,1}); idle(1100);} 
  runDiagnosis();
  bool foundNever=false, foundExc2=false, foundForeign=false, foundGlitch=false;
  for(int i=0;i<findingCount;i++){ printf("  [%s] %s\n", sevName(findings[i].sev), findings[i].title.c_str());
    if(findings[i].title.s.find("Slave 10 never answers")!=std::string::npos) foundNever=true;
    if(findings[i].title.s.find("exception 02")!=std::string::npos) foundExc2=true;
    if(findings[i].title.s.find("Another device")!=std::string::npos) foundForeign=true;
    if(findings[i].title.s.find("Stray byte")!=std::string::npos) foundGlitch=true; }
  for(int i=0;i<findingCount;i++){ CHECK(findings[i].cause.length()>0); CHECK(findings[i].remedy.length()>0); }
  { int steps=1; for(char c: findings[0].remedy.s) if(c=='\n') steps++; printf("  first finding remedy has %d step(s)\n", steps); }
  CHECK(foundNever); CHECK(foundExc2); CHECK(foundForeign); CHECK(foundGlitch); CHECK(overallSev==SEV_FAULT);
  // 13) healthy bus
  resetStatistics(); for(int i=0;i<30;i++){ sendReq({1,3,0,0,0,1}); rx({1,3,2,0,(uint8_t)i}); idle(20);} runDiagnosis();
  CHECK(overallSev<=SEV_INFO); CHECK(findingCount>=1 && findings[findingCount-1].title=="Bus healthy");
  printf("  %s / %s\n", findings[findingCount-1].cause.c_str(), findings[findingCount-1].remedy.c_str());
  // 14) dead after working
  for(int i=0;i<6;i++){ sendReq({1,3,0,0,0,1}); idle(1100);} runDiagnosis(); bool down=false;
  for(int i=0;i<findingCount;i++) if(findings[i].title=="Slave 1 stopped responding") down=true; CHECK(down);
  // ===== 15) Sniffer: splitting, pairing, passive diagnosis =====
  {
    resetStatistics(); analyzerAbort();
    bridgeMode = MODE_SNIFF; memset(&sniffStatus,0,sizeof(sniffStatus)); sniffStatus.state = SNIFF_RUNNING;
    sniffClockUs = 0; sniffLastMicros = micros();
    auto seg = [&](std::vector<std::vector<uint8_t>> parts, bool crc=true){
      segLen=0; for(auto& v: parts){ uint8_t b[300]; uint16_t n; if(crc) frame(v,b,n); else {memcpy(b,v.data(),v.size()); n=v.size();}
        memcpy(segBuf+segLen,b,n); segLen+=n; }
      g_us += 20000; segLastUs = micros(); splitSegment(false); };
    auto lastKind=[&](int back){ return sniffLog[(sniffStatus.lastSeq-back)%SNIFF_LOG_SIZE].kind; };
    // request + response arriving in ONE segment (fast slave / high baud)
    seg({{5,3,0,10,0,2},{5,3,4,0,1,0,2}});
    CHECK(sniffStatus.frames==2); CHECK(lastKind(1)==SK_REQUEST); CHECK(lastKind(0)==SK_RESPONSE); CHECK(slaveStats[5].ok==1);
    // separate segments + exception
    seg({{5,3,0x9C,0x40,0,1}}); seg({{5,0x83,2}});
    CHECK(lastKind(0)==SK_EXCEPTION); CHECK(slaveStats[5].exceptions==1);
    // FC6 write + echo reply (same shape as the request)
    seg({{5,6,0,1,0,7}}); seg({{5,6,0,1,0,7}});
    CHECK(lastKind(1)==SK_REQUEST); CHECK(lastKind(0)==SK_RESPONSE); CHECK(slaveStats[5].ok==2);
    // master retries before the reply: second identical request is a request, first counts as timeout
    seg({{9,4,0,0,0,1}}); seg({{9,4,0,0,0,1}}); seg({{9,4,2,0,5}});
    CHECK(slaveStats[9].timeouts==1); CHECK(slaveStats[9].ok==1);
    // stray 0x00 before a reply inside one segment
    { seg({{5,3,0,0,0,1}}); uint8_t b[16]; uint16_t n; frame({5,3,2,0,9},b+1,n); b[0]=0; memcpy(segBuf,b,n+1); segLen=n+1; g_us+=20000; segLastUs=micros(); splitSegment(false);
      CHECK(lastKind(0)==SK_RESPONSE); CHECK(lastKind(1)==SK_GARBAGE); CHECK(slaveStats[5].ok==3); }
    // pure garbage
    uint32_t g0=sniffStatus.garbage; seg({{0xFF,0x13,0x37,0x55,0xAA}},false); CHECK(sniffStatus.garbage==g0+1);
    // FC16 write multiple + reply
    seg({{5,16,0,0,0,2,4,0,1,0,2}}); seg({{5,16,0,0,0,2}}); CHECK(lastKind(1)==SK_REQUEST); CHECK(lastKind(0)==SK_RESPONSE);
    printf("  sniffer: %u frames, %u garbage, slave5 ok=%u exc=%u\n", sniffStatus.frames, sniffStatus.garbage, slaveStats[5].ok, slaveStats[5].exceptions);
    // passive diagnosis: sniffer finding present, no "Another device" finding
    runDiagnosis(); bool sn=false, foreign=false;
    for(int i=0;i<findingCount;i++){ if(findings[i].title.s.find("Sniffer mode")!=std::string::npos) sn=true; if(findings[i].title.s.find("Another device")!=std::string::npos) foreign=true; }
    CHECK(sn); CHECK(!foreign);
  }
  // ===== 16) Auto-detect must not lock on noise =====
  {
    unsigned seed=12345; auto rnd=[&](){ seed=seed*1103515245u+12345u; return (seed>>16)&0xFF; };
    detectScoreCur=0; int segs=5000;
    for(int k=0;k<segs;k++){ segLen=8+rnd()%40; for(int i=0;i<segLen;i++) segBuf[i]=rnd(); segLastUs=micros(); splitSegment(true); }
    printf("  auto-detect: %u plausible frame(s) found in %d random noise segments (lock needs 3 within 2 s)\n", detectScoreCur, segs);
    CHECK(detectScoreCur <= 2);
    detectScoreCur=0; uint8_t b[16]; uint16_t n; frame({1,3,0,0,0,1},b,n); memcpy(segBuf,b,n); segLen=n; segLastUs=micros(); splitSegment(true);
    CHECK(detectScoreCur==1);
    bridgeMode = MODE_BRIDGE;
  }
  printf(fails? "%d FAILED\n" : "ALL PASSED\n", fails); return fails;
}
