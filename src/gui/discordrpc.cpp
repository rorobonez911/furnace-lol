#include "gui.h"
#include "../engine/engine.h"
#include <imgui.h>
#include <fmt/printf.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>

static int discordSock=-1;
static bool discordReady=false;
static double discordLastUpdate=-999.0;
static time_t discordStartTime=0;
static int discordNonce=0;

struct DiscordHeader {
  uint32_t op;
  uint32_t len;
};

static bool discordWrite(int op, const char* json) {
  DiscordHeader h;
  h.op=(uint32_t)op;
  h.len=(uint32_t)strlen(json);
  if (write(discordSock,&h,sizeof(h))!=(ssize_t)sizeof(h)) return false;
  if (write(discordSock,json,(size_t)h.len)!=(ssize_t)h.len) return false;
  return true;
}

static bool discordRead() {
  DiscordHeader h;
  ssize_t r=recv(discordSock,&h,sizeof(h),MSG_DONTWAIT);
  if (r<=0) return r==0?false:true;
  if ((size_t)r<sizeof(h)) return true;
  char buf[8192];
  size_t toRead=h.len<sizeof(buf)-1?h.len:sizeof(buf)-1;
  r=recv(discordSock,buf,toRead,MSG_DONTWAIT);
  if (r<0) return true;
  buf[r]=0;
  if (h.op==1 && strstr(buf,"\"READY\"")) discordReady=true;
  return true;
}

static void discordConnect(const char* clientId) {
  if (discordSock>=0) { close(discordSock); discordSock=-1; discordReady=false; }
  discordSock=socket(AF_UNIX,SOCK_STREAM,0);
  if (discordSock<0) return;
  const char* runtimeDir=getenv("XDG_RUNTIME_DIR");
  char path[256];
  for (int i=0;i<10;i++) {
    if (runtimeDir) snprintf(path,sizeof(path),"%s/discord-ipc-%d",runtimeDir,i);
    else snprintf(path,sizeof(path),"/tmp/discord-ipc-%d",i);
    struct sockaddr_un addr;
    memset(&addr,0,sizeof(addr));
    addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path,path,sizeof(addr.sun_path)-1);
    if (connect(discordSock,(struct sockaddr*)&addr,sizeof(addr))==0) {
      fcntl(discordSock,F_SETFL,O_NONBLOCK);
      char hs[256];
      snprintf(hs,sizeof(hs),"{\"v\":1,\"client_id\":\"%s\"}",clientId);
      discordWrite(0,hs);
      return;
    }
  }
  close(discordSock);
  discordSock=-1;
}

void FurnaceGUI::initDiscord() {
  if (settings.discordClientId.empty()) return;
  discordStartTime=time(NULL);
  discordConnect(settings.discordClientId.c_str());
}

void FurnaceGUI::shutdownDiscord() {
  if (discordSock>=0) { close(discordSock); discordSock=-1; }
  discordReady=false;
}

void FurnaceGUI::updateDiscordPresence() {
  if (!settings.discordEnabled) return;
  if (discordSock<0) {
    if (settings.discordClientId.empty()) return;
    static double lastTry=-60.0;
    if (ImGui::GetTime()-lastTry<15.0) return;
    lastTry=ImGui::GetTime();
    discordStartTime=time(NULL);
    discordConnect(settings.discordClientId.c_str());
    if (discordSock<0) return;
  }
  if (!discordRead()) { close(discordSock); discordSock=-1; discordReady=false; return; }
  if (!discordReady) return;
  if (ImGui::GetTime()-discordLastUpdate<5.0) return;
  discordLastUpdate=ImGui::GetTime();

  const char* songName=e->song.name.empty()?"Untitled":e->song.name.c_str();
  const char* author=e->song.author.empty()?NULL:e->song.author.c_str();

  String chips;
  for (int i=0;i<e->song.systemLen;i++) {
    if (i>0) chips+=", ";
    chips+=e->getSystemName(e->song.system[i]);
    if (chips.size()>60) { chips+="..."; break; }
  }

  String details=fmt::sprintf("%.60s",songName);
  String state;
  if (author) state=fmt::sprintf("by %.40s",author);
  else if (!chips.empty()) state=chips;

  char json[2048];
  snprintf(json,sizeof(json),
    "{"
      "\"cmd\":\"SET_ACTIVITY\","
      "\"args\":{"
        "\"pid\":%d,"
        "\"activity\":{"
          "\"details\":\"%s\","
          "\"state\":\"%s\","
          "\"timestamps\":{\"start\":%lld},"
          "\"assets\":{"
            "\"large_image\":\"furnace\","
            "\"large_text\":\"%s — %s\""
          "}"
        "}"
      "},"
      "\"nonce\":\"%d\""
    "}",
    (int)getpid(),
    details.c_str(),
    state.c_str(),
    (long long)discordStartTime,
    DIV_VERSION,
    chips.empty()?"":chips.c_str(),
    ++discordNonce
  );

  if (!discordWrite(1,json)) {
    close(discordSock);
    discordSock=-1;
    discordReady=false;
  }
}

#else

void FurnaceGUI::initDiscord() {}
void FurnaceGUI::shutdownDiscord() {}
void FurnaceGUI::updateDiscordPresence() {}

#endif
