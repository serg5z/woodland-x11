#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <fcntl.h>
#include <errno.h>

#include "LGFX_CYD.h"
#include "src/core/x11_server.h"
#include "src/net/x11_net.h"
#include "src/input/x11_touch.h"

static constexpr int W=320, H=240, X11_PORT=6000;
static constexpr size_t REPLY_CAP=4096, OUTQ_MAX=12288, MEMORY_BUDGET=80*1024;
#define CYD_RESOURCES_PER_CLIENT 48
static LGFX_CYD<lgfx::Panel_ST7789> lcd;
static x11_server server;
static x11_net net;
static x11_platform platform;
static x11_server_config server_cfg;
static x11_net_config net_cfg;
static Preferences prefs;
static x11_touch pointer_touch;
static x11_touch_config pointer_touch_cfg;
static String wifi_ssid, wifi_pass;
static uint8_t auth_cookie[X11_COOKIE_LEN];
static uint16_t touch_calibration[8];
static bool have_cookie=false, network_started=false, core_started=false, touch_on=true, touch_log=false;
static bool touch_calibrated=false;
static int listener=-1;
static uint32_t accept_errors=0;
static int last_accept_error=0;
static char serial_line[160];
/* Diagnostics are large; keep them out of the loop task's limited stack. */
static char diag_net[4096], diag_core[2048];
/* Live read-only _WOODLAND_DIAG property, capped to the network reply scratch. */
static size_t platform_diag(void *,char *out,size_t cap) {
  if(!cap)return 0;
  const uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
  int n=snprintf(out,cap,
    "heap.total=%u\nheap.free=%u\nheap.largest=%u\nheap.minimum=%u\nheap.psram=%u\n"
    "net.budget=%u\nnet.reserved=%u\nnet.core_used=%u\nnet.core_peak=%u\n"
    "net.core_alloc_failed=%u\nnet.conn_alloc_failed=%u\n"
    "core.resources=%u\ncore.resources_max=%u\ncore.resources_per_client_max=%u\ncore.clients_open=%u\n",
    (unsigned)heap_caps_get_total_size(caps),(unsigned)heap_caps_get_free_size(caps),
    (unsigned)heap_caps_get_largest_free_block(caps),(unsigned)heap_caps_get_minimum_free_size(caps),
    psramFound()?1u:0u,(unsigned)net.cfg.budget,(unsigned)net.n.reserved,(unsigned)net.n.core_used,
    (unsigned)net.n.core_peak,(unsigned)net.n.core_alloc_failed,(unsigned)net.n.conn_alloc_failed,
    (unsigned)x11_resources_used(&server),(unsigned)(server.max_resources-1),
    (unsigned)server.cfg.max_resources_per_client,(unsigned)server.nclients);
  if(n<0)return 0;
  return (size_t)n<cap?(size_t)n:cap-1;
}
static size_t serial_len=0;
static uint16_t pixels[320];
static struct { uint32_t reads,writes,fills; uint64_t read_bytes,write_bytes,read_us,write_us; } lcd_io;
static uint32_t last_heap_report=0, last_net_report=0;
static uint32_t last_heap_sample=0;
static uint32_t sampled_largest_min=UINT32_MAX, sampled_largest_min_free=0, sampled_largest_min_ms=0;
static uint32_t sampled_free_min=UINT32_MAX, sampled_free_min_largest=0, sampled_free_min_ms=0;
static int last_open_sample=-1;
static uint32_t last_accepted=0,last_setup=0;
static uint32_t last_touch_poll=0,last_touch_log=0,touch_poll_count=0,touch_event_count=0;
static uint32_t touch_interval_max_ms=0,touch_read_us_max=0,touch_event_us_max=0;
static int16_t touch_raw_x=-1,touch_raw_y=-1;

static uint16_t swap16(uint16_t v) { return (uint16_t)((v<<8)|(v>>8)); }
static uint32_t pxread(void *,int x,int y,int) {
  uint32_t t=micros(); lcd.readRect(x,y,1,1,pixels); lcd_io.read_us+=micros()-t;
  ++lcd_io.reads; lcd_io.read_bytes+=2; return swap16(pixels[0]);
}
static void pxwrite(void *,int x,int y,int,uint32_t p) {
  pixels[0]=swap16((uint16_t)p); uint32_t t=micros(); lcd.pushImage(x,y,1,1,pixels);
  lcd_io.write_us+=micros()-t; ++lcd_io.writes; lcd_io.write_bytes+=2;
}
static void rectfill(void *,int x,int y,int w,int h,uint32_t p,int) {
  uint32_t t=micros(); lcd.fillRect(x,y,w,h,(uint16_t)p); lcd_io.write_us+=micros()-t;
  ++lcd_io.fills; lcd_io.write_bytes+=(uint64_t)w*h*2;
}
static void spanread(void *,int x,int y,int n,uint16_t *out) {
  uint32_t t=micros(); lcd.readRect(x,y,n,1,pixels); lcd_io.read_us+=micros()-t;
  ++lcd_io.reads; lcd_io.read_bytes+=(uint64_t)n*2;
  for(int i=0;i<n;++i) out[i]=swap16(pixels[i]);
}
static void spanwrite(void *,int x,int y,int n,const uint16_t *in) {
  for(int i=0;i<n;++i) pixels[i]=swap16(in[i]);
  uint32_t t=micros(); lcd.pushImage(x,y,n,1,pixels); lcd_io.write_us+=micros()-t;
  ++lcd_io.writes; lcd_io.write_bytes+=(uint64_t)n*2;
}
static const xr_surface_ops surface_ops={pxread,pxwrite,rectfill,spanread,spanwrite};

static void heap_sample() {
  const uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
  uint32_t free=heap_caps_get_free_size(caps),largest=heap_caps_get_largest_free_block(caps),now=millis();
  if(largest<sampled_largest_min)sampled_largest_min=largest,sampled_largest_min_free=free,sampled_largest_min_ms=now;
  if(free<sampled_free_min)sampled_free_min=free,sampled_free_min_largest=largest,sampled_free_min_ms=now;
}

static void heap_report(const char *stage) {
  const uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT; multi_heap_info_t h={}; heap_caps_get_info(&h,caps);
  Serial.printf("HEAP,%s,total=%u,free=%u,largest=%u,min=%u,psram=%u,largest_sample_min=%u@%ums(free=%u),free_sample_min=%u@%ums(largest=%u)\n",stage,
    (unsigned)heap_caps_get_total_size(caps),(unsigned)heap_caps_get_free_size(caps),
    (unsigned)heap_caps_get_largest_free_block(caps),(unsigned)heap_caps_get_minimum_free_size(caps),
    psramFound()?1u:0u,(unsigned)sampled_largest_min,(unsigned)sampled_largest_min_ms,
    (unsigned)sampled_largest_min_free,(unsigned)sampled_free_min,(unsigned)sampled_free_min_ms,
    (unsigned)sampled_free_min_largest);
}
static void net_stats() {
  Serial.printf("TCP_ACCEPT,errors=%lu,last_errno=%d\n",(unsigned long)accept_errors,last_accept_error);
  size_t n=x11_net_stats(&net,diag_net,sizeof(diag_net));
  Serial.printf("NETSTATS,%u\n",(unsigned)n); Serial.write((const uint8_t*)diag_net,n); Serial.println();
  n=x11_server_stats(&server,diag_core,sizeof(diag_core));
  Serial.printf("CORESTATS,%u\n",(unsigned)n); Serial.write((const uint8_t*)diag_core,n); Serial.println();
  Serial.printf("LCDIO,read_calls=%lu,write_calls=%lu,fill_calls=%lu,read_bytes=%llu,write_bytes=%llu,read_us=%llu,write_us=%llu\n",
    (unsigned long)lcd_io.reads,(unsigned long)lcd_io.writes,(unsigned long)lcd_io.fills,(unsigned long long)lcd_io.read_bytes,(unsigned long long)lcd_io.write_bytes,
    (unsigned long long)lcd_io.read_us,(unsigned long long)lcd_io.write_us);
  heap_report("stats");
  Serial.printf("TOUCHSTATS,on=%u,calibrated=%u,polls=%lu,events=%lu,presses=%lu,releases=%lu,"
    "x=%d,y=%d,raw_x=%d,raw_y=%d,interval_max_ms=%lu,read_max_us=%lu,event_max_us=%lu\n",
    touch_on?1u:0u,touch_calibrated?1u:0u,(unsigned long)touch_poll_count,(unsigned long)touch_event_count,
    (unsigned long)pointer_touch.presses,(unsigned long)pointer_touch.releases,pointer_touch.x,pointer_touch.y,
    touch_raw_x,touch_raw_y,(unsigned long)touch_interval_max_ms,(unsigned long)touch_read_us_max,
    (unsigned long)touch_event_us_max);
  Serial.printf("XINPUT,grab=%d/k%u/w%08lx,pointer=%d,%d,queue=%u,dropped=%u\n",
    (int)server.pgrab.client,(unsigned)server.pgrab.kind,(unsigned long)server.pgrab.window,
    (int)server.ptr_x,(int)server.ptr_y,(unsigned)server.qlen,(unsigned)server.input_dropped);
  Serial.printf("XTEST_INPUT,motion=%lu,press=%lu,release=%lu,ignored_keys=%lu,requests=%lu,request_max_us=%lu,request_total_us=%llu\n",
    (unsigned long)server.xtest_fake[0],(unsigned long)server.xtest_fake[1],(unsigned long)server.xtest_fake[2],
    (unsigned long)server.xtest_key_ignored,(unsigned long)net.n.xtest_requests,(unsigned long)net.n.xtest_request_us_max,
    (unsigned long long)net.n.xtest_request_us_total);
}
static uint32_t tick_ms(void *) { return millis(); }
static uint64_t tick_us(void *) { return (uint64_t)esp_timer_get_time(); }
static void *mem_alloc(void *,size_t n) { return malloc(n); }
static void mem_free(void *,void *p) { free(p); }
static long io_recv(void *,int fd,uint8_t *p,size_t n) {
  int r=lwip_recv(fd,p,n,0); if(r>=0)return r;
  return (errno==EAGAIN||errno==EWOULDBLOCK)?X11_IO_WOULD_BLOCK:X11_IO_ERROR;
}
static long io_send(void *,int fd,const uint8_t *p,size_t n) {
  int r=lwip_send(fd,p,n,MSG_DONTWAIT); if(r>=0)return r;
  return (errno==EAGAIN||errno==EWOULDBLOCK)?X11_IO_WOULD_BLOCK:X11_IO_ERROR;
}
static void io_close(void *,int fd) { lwip_close(fd); }
static void request_log(void *,int,const uint8_t *req,size_t,const uint8_t *out,size_t n) {
  if(n && out[0]==0) Serial.printf("XERROR,opcode=%u,code=%u,bad=%02x%02x%02x%02x\n",
    req[0],out[1],out[7],out[6],out[5],out[4]);
}
static int io_ready(void *,int fd,int write) {
  fd_set rd,wr; FD_ZERO(&rd);FD_ZERO(&wr); if(write)FD_SET(fd,&wr);else FD_SET(fd,&rd);
  timeval tv={0,0}; int r=lwip_select(fd+1,write?nullptr:&rd,write?&wr:nullptr,nullptr,&tv);
  return r>0 && (write?FD_ISSET(fd,&wr):FD_ISSET(fd,&rd));
}
static void touch_poll() {
  if(!core_started||!touch_on)return;
  uint32_t now=millis(),interval=now-last_touch_poll;
  if(interval<15)return;
  last_touch_poll=now;++touch_poll_count;
  if(interval>touch_interval_max_ms)touch_interval_max_ms=interval;
  lgfx::touch_point_t p;
  uint32_t t0=micros();uint_fast8_t points=lcd.getTouchRaw(&p);uint32_t read_us=micros()-t0;
  if(read_us>touch_read_us_max)touch_read_us_max=read_us;
  int x=0,y=0;
  if(points){touch_raw_x=p.x;touch_raw_y=p.y;lcd.convertRawXY(&p,1);x=p.x;y=p.y;}
  uint8_t was_down=pointer_touch.down;
  int changed=x11_touch_sample(&pointer_touch,now,1,points, x, y);
  if(changed){
    t0=micros();x11_pointer_input(&server,pointer_touch.x,pointer_touch.y,pointer_touch.down?1u:0u);
    uint32_t event_us=micros()-t0;if(event_us>touch_event_us_max)touch_event_us_max=event_us;
    ++touch_event_count;
    if(touch_log&&(pointer_touch.down!=was_down||now-last_touch_log>=100)){
      last_touch_log=now;Serial.printf("TOUCH,raw=%d,%d,xy=%d,%d,state=%s,ptr=%d,%d,grab=%d/k%u/w%08lx\n",
        touch_raw_x,touch_raw_y,pointer_touch.x,pointer_touch.y,pointer_touch.down?"down":"up",
        (int)server.ptr_x,(int)server.ptr_y,
        (int)server.pgrab.client,(unsigned)server.pgrab.kind,(unsigned long)server.pgrab.window);
    }
  }
}
static void make_nonblock(int fd) { int f=lwip_fcntl(fd,F_GETFL,0); if(f>=0)lwip_fcntl(fd,F_SETFL,f|O_NONBLOCK); }
static void poll_accept() {
  if(listener<0)return;
  sockaddr_in peer={}; socklen_t n=sizeof(peer); int fd=lwip_accept(listener,(sockaddr*)&peer,&n);
  if(fd<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK){++accept_errors;last_accept_error=errno;}return;}
  make_nonblock(fd);
  x11_net_accept(&net,fd,ntohl(peer.sin_addr.s_addr),ntohs(peer.sin_port));
}
static bool open_listener() {
  if(listener>=0)return true;
  int fd=lwip_socket(AF_INET,SOCK_STREAM,IPPROTO_IP); if(fd<0)return false;
  int one=1; lwip_setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one)); make_nonblock(fd);
  sockaddr_in a={};a.sin_family=AF_INET;a.sin_port=htons(X11_PORT);a.sin_addr.s_addr=htonl(INADDR_ANY);
  if(lwip_bind(fd,(sockaddr*)&a,sizeof(a))<0||lwip_listen(fd,1)<0){lwip_close(fd);return false;}
  listener=fd;
  Serial.printf("LISTEN,ip=%s,display=%s:0,port=%d,auth=MIT-MAGIC-COOKIE-1\n",
    WiFi.localIP().toString().c_str(),WiFi.localIP().toString().c_str(),X11_PORT);
  heap_report("listener");return true;
}
static bool start_x11() {
  if(core_started)return true;
  if(!have_cookie||WiFi.status()!=WL_CONNECTED){Serial.println("START_WAIT,requires WiFi association and auth cookie");return false;}
  x11_default_config(&server_cfg);server_cfg.width_px=W;server_cfg.height_px=H;server_cfg.width_mm=49;server_cfg.height_mm=37;
  server_cfg.cookie=auth_cookie;server_cfg.max_clients=X11_SERVER_CLIENTS;server_cfg.max_resources=256;
  server_cfg.max_resources_per_client=CYD_RESOURCES_PER_CLIENT;
  x11_net_default_config(&net_cfg,OUTQ_MAX,MEMORY_BUDGET);
  net_cfg.reply_cap=REPLY_CAP;net_cfg.in_max=4*4096+64;net_cfg.outq_min=1024;
  net_cfg.outq_reserve=REPLY_CAP+X11_NET_DEFERRED_EVENTS*32;net_cfg.outq_max=OUTQ_MAX;
  net_cfg.setup_timeout_ms=10000;net_cfg.request_budget_us=0;
  x11_net_ops ops={};ops.recv=io_recv;ops.send=io_send;ops.close=io_close;ops.now_ms=tick_ms;ops.now_us=tick_us;
  ops.alloc=mem_alloc;ops.release=mem_free;ops.on_request=request_log;
  platform={};platform.framebuffer_ops=&surface_ops;platform.diag=platform_diag;
  uint32_t t=micros();
  int r=x11_net_init(&net,&server,&net_cfg,&ops,&platform);
  if(r){Serial.printf("INIT_FAIL,net=%d\n",r);heap_report("net_init_fail");return false;}
  heap_report("after_net_init");
  r=x11_server_init_platform(&server,&server_cfg,&platform);
  if(r){Serial.printf("INIT_FAIL,core=%d\n",r);heap_report("core_init_fail");return false;}
  core_started=true;
  Serial.printf("X11_READY,init_us=%lu,server_struct=%u,net_struct=%u,reply_cap=%u,screen=%dx%d,clients=%u\n",
    (unsigned long)(micros()-t),(unsigned)sizeof(server),(unsigned)sizeof(net),(unsigned)REPLY_CAP,W,H,(unsigned)server_cfg.max_clients);
  heap_report("after_core_init");
  return open_listener();
}
static void wifi_start() {
  if(network_started||wifi_ssid.isEmpty())return;
  WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.setSleep(false);
  WiFi.begin(wifi_ssid.c_str(),wifi_pass.c_str());network_started=true;
  Serial.println("WIFI,connecting");heap_report("wifi_start");
}
static void cookie_print() {
  Serial.print("AUTH,");for(unsigned i=0;i<X11_COOKIE_LEN;++i)Serial.printf("%02x",auth_cookie[i]);Serial.println();
}
static void command(char *s) {
  while(*s==' ')++s;
  if(!strncmp(s,"SSID ",5)){wifi_ssid=String(s+5);prefs.putString("ssid",wifi_ssid);Serial.println("SSID,saved");}
  else if(!strncmp(s,"PASS ",5)){wifi_pass=String(s+5);prefs.putString("pass",wifi_pass);Serial.println("PASS,saved");}
  else if(!strcmp(s,"COOKIE NEW")){
    /* ESP-IDF documents esp_random as true-random when Wi-Fi/BT is enabled. Ensure the
     * radio is initialized before generating a persistent authentication secret. */
    if(WiFi.getMode()==WIFI_OFF&&!WiFi.mode(WIFI_STA)){Serial.println("COOKIE,failed_wifi_rng_init");return;}
    uint8_t next_cookie[X11_COOKIE_LEN];esp_fill_random(next_cookie,sizeof(next_cookie));
    size_t stored=prefs.putBytes("cookie",next_cookie,sizeof(next_cookie));
    if(stored!=sizeof(next_cookie)){memset(next_cookie,0,sizeof(next_cookie));Serial.println("COOKIE,failed_nvs_write");return;}
    memcpy(auth_cookie,next_cookie,sizeof(auth_cookie));memset(next_cookie,0,sizeof(next_cookie));
    have_cookie=true;Serial.println("COOKIE,saved");
  }
  else if(!strcmp(s,"AUTH")){if(have_cookie)cookie_print();else Serial.println("AUTH,missing");}
  else if(!strcmp(s,"START")){wifi_start();start_x11();}
  else if(!strcmp(s,"STATS")){if(core_started)net_stats();else heap_report("not_started");}
  else if(!strcmp(s,"TOUCH"))Serial.printf("TOUCHSTATE,on=%u,calibrated=%u,raw=%d,%d,xy=%d,%d,down=%u,presses=%lu,releases=%lu\n",
    touch_on?1u:0u,touch_calibrated?1u:0u,touch_raw_x,touch_raw_y,pointer_touch.x,pointer_touch.y,
    pointer_touch.down?1u:0u,(unsigned long)pointer_touch.presses,(unsigned long)pointer_touch.releases);
  else if(!strcmp(s,"TOUCHLOG ON")){touch_log=true;Serial.println("TOUCHLOG,on");}
  else if(!strcmp(s,"TOUCHLOG OFF")){touch_log=false;Serial.println("TOUCHLOG,off");}
  else if(!strcmp(s,"TOUCH ON")){touch_on=true;Serial.println("TOUCH,on");}
  else if(!strcmp(s,"TOUCH OFF")){touch_on=false;int changed=x11_touch_cancel(&pointer_touch);if(changed)x11_pointer_input(&server,pointer_touch.x,pointer_touch.y,0);Serial.println("TOUCH,off");}
  else if(!strcmp(s,"TOUCHCAL")){
    uint16_t c[8];Serial.println("TOUCHCAL,begin; tap each crosshair and lift");
    lcd.calibrateTouch(c,0xFFFF,0x0000,12);lcd.setTouchCalibrate(c);memcpy(touch_calibration,c,sizeof(c));
    touch_calibrated=true;prefs.putBytes("tcal",touch_calibration,sizeof(touch_calibration));
    Serial.print("TOUCHCAL,done");for(unsigned i=0;i<8;++i)Serial.printf(",%u",touch_calibration[i]);Serial.println();
  }
  else if(!strcmp(s,"HELP"))Serial.println("commands: SSID <name>, PASS <password>, COOKIE NEW, AUTH, START, STATS");
  else if(*s)Serial.println("COMMAND,unknown");
}
static void serial_poll() {
  while(Serial.available()){
    int c=Serial.read();if(c=='\r')continue;
    if(c=='\n'){serial_line[serial_len]=0;command(serial_line);serial_len=0;continue;}
    if(serial_len+1<sizeof(serial_line))serial_line[serial_len++]=(char)c;else serial_len=0;
  }
}

void setup() {
  Serial.begin(115200);delay(500);Serial.println("Woodland X11 experimental CYD; ST7789 GRAM; 16 client slots");
  Serial.printf("RESET_REASON,%d\n",(int)esp_reset_reason());Serial.flush();
  Serial.printf("PSRAM,found=%u\n",psramFound()?1u:0u);heap_report("boot");
  pinMode(CYD_TFT_BL,OUTPUT);digitalWrite(CYD_TFT_BL,HIGH);Serial.println("LCD_INIT_BEGIN");Serial.flush();
  bool lcd_ok=lcd.init();Serial.printf("LCD_INIT_DONE,ok=%u\n",lcd_ok?1u:0u);lcd.setRotation(1);heap_report("after_lcd");
  prefs.begin("cydx11",false);wifi_ssid=prefs.getString("ssid","");wifi_pass=prefs.getString("pass","");
  size_t tc=prefs.getBytes("tcal",touch_calibration,sizeof(touch_calibration));touch_calibrated=tc==sizeof(touch_calibration);
  if(touch_calibrated)lcd.setTouchCalibrate(touch_calibration);
  x11_touch_default_config(&pointer_touch_cfg,W,H);pointer_touch_cfg.release_ms=60;pointer_touch_cfg.jitter_px=1;
  x11_touch_init(&pointer_touch,&pointer_touch_cfg);last_touch_poll=millis();
  Serial.printf("TOUCH_INIT,xpt2046=%u,calibrated=%u,irq=%d,cs=%d,variant_bus=%d,host=%d\n",
    lcd.touch()!=nullptr?1u:0u,touch_calibrated?1u:0u,CYD_TP_IRQ,CYD_TP_CS,CYD_TP_SPI_BUS,VSPI_HOST);
  size_t c=prefs.getBytes("cookie",auth_cookie,sizeof(auth_cookie));have_cookie=c==sizeof(auth_cookie);
  if(wifi_ssid.isEmpty()||wifi_pass.isEmpty()||!have_cookie)Serial.println("Provision with SSID, PASS, COOKIE NEW; then START. Secrets are not echoed.");
  else wifi_start();
  last_heap_report=millis();
}
void loop() {
  serial_poll();
  if(network_started&&WiFi.status()==WL_CONNECTED&&!core_started){
    Serial.printf("WIFI,connected,ip=%s,rssi=%d\n",WiFi.localIP().toString().c_str(),WiFi.RSSI());
    heap_report("wifi_associated");start_x11();
  }
  if(listener>=0&&core_started){
    poll_accept();x11_net_pass(&net,io_ready);
    int open=x11_net_open(&net);if(open!=last_open_sample){last_open_sample=open;heap_report("client_count");}
    if(net.n.accepted!=last_accepted||net.n.setup_ok!=last_setup){
      last_accepted=net.n.accepted;last_setup=net.n.setup_ok;heap_report("client_progress");
      Serial.printf("CLIENTS,accepted=%lu,setup=%lu,open=%d,errors=%llu,requests=%llu\n",(unsigned long)last_accepted,(unsigned long)last_setup,
        x11_net_open(&net),(unsigned long long)net.n.errors,(unsigned long long)net.n.requests);
    }
  }
  touch_poll();
  if(millis()-last_heap_sample>=250){last_heap_sample=millis();heap_sample();}
  if(millis()-last_heap_report>10000){last_heap_report=millis();heap_report("periodic");}
  if(millis()-last_net_report>5000){last_net_report=millis();if(core_started)
    Serial.printf("LOAD,requests=%llu,errors=%llu,request_max_us=%lu,graphics_max_us=%lu,backpressure=%lu\n",
      (unsigned long long)net.n.requests,(unsigned long long)net.n.errors,(unsigned long)net.n.request_us_max,(unsigned long)net.n.graphics_us_max,(unsigned long)net.n.backpressure_waits);}
  if(listener<0)delay(20);else delay(1);
}
