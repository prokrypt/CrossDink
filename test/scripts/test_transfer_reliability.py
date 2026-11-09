"""Execute focused web server paths with host doubles, without booting the UI.

Compile the actual definitions from the production translation unit so the
discovery and idle-WebSocket tests exercise the code the firmware builds.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "src/network/CrossPointWebServer.cpp"


def between(source, first, following):
    return source[source.index(first):source.index(following, source.index(first))]


class ReliabilityTest(unittest.TestCase):
    def compile_and_run(self, code):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.cpp").write_text(code)
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(path / "test.cpp"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True, timeout=10)

    def test_discovery_and_idle_connections(self):
        source = WEB.read_text()
        discovery = between(source, "void CrossPointWebServer::openDiscoverySocket()",
                            "bool PendingAwareWebServer::requestPending()")
        idle = between(source, "void closeIdleWsClients(", "#endif")
        self.compile_and_run(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <string>
#include <unistd.h>
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
constexpr uint16_t LOCAL_UDP_PORT=8134;
constexpr const char* NET_HOSTNAME="crossdink";
constexpr unsigned WEBSOCKETS_SERVER_CLIENT_MAX=5;
constexpr uint32_t WS_IDLE_TIMEOUT_MS=120000;
uint32_t wsLastActivity[WEBSOCKETS_SERVER_CLIENT_MAX]={}, now=1000;
uint32_t millis() { return now; }
struct String : std::string {
  String(const char* s) : std::string(s ? s : "") {}
  bool isEmpty() const { return empty(); }
};
struct WebSocketsServer {
  bool connected[5]={}; unsigned disconnects=0;
  bool clientIsConnected(unsigned i) { return connected[i]; }
  void disconnect(unsigned i) { connected[i]=false; wsLastActivity[i]=0; ++disconnects; }
};
struct { const char* name="reader"; const char* getHostname() { return name; } } WiFi;
class CrossPointWebServer {
 public:
  int discoverySocket=-1; uint16_t wsPort=81;
  void openDiscoverySocket(); void closeDiscoverySocket(); void answerDiscovery() const;
};
''' + discovery + idle + r'''
int main() {
  CrossPointWebServer web; web.openDiscoverySocket(); assert(web.discoverySocket>=0);
  const int fd=web.discoverySocket; web.openDiscoverySocket(); assert(web.discoverySocket==fd);
  int client=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); assert(client>=0);
  assert(fcntl(client,F_SETFL,O_NONBLOCK)>=0);
  sockaddr_in target={}; target.sin_family=AF_INET; target.sin_port=htons(LOCAL_UDP_PORT);
  target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  auto send=[&](const char* text,size_t len) { assert(sendto(client,text,len,0,(sockaddr*)&target,sizeof(target))==static_cast<int>(len)); };
  auto waitFor=[](int sock) { pollfd ready{sock,POLLIN,0}; assert(poll(&ready,1,1000)==1); };
  char reply[128];
  // Reject oversized and embedded-NUL packets, and still answer the next datagram.
  std::string longPacket(2000,'x'); send(longPacket.data(),longPacket.size());
  send("hello\0bad",9); send("hello",5);
  waitFor(fd); web.answerDiscovery(); assert(recv(client,reply,sizeof(reply),0)<0);
  waitFor(fd); web.answerDiscovery(); assert(recv(client,reply,sizeof(reply),0)<0);
  waitFor(fd); web.answerDiscovery(); waitFor(client);
  int size=recv(client,reply,sizeof(reply),0); assert(size>0);
  assert(std::string(reply,size)=="crosspoint (on reader);81");
  // No hostname: the default name answers.
  WiFi.name=""; send("hello",5); waitFor(fd); web.answerDiscovery(); waitFor(client);
  size=recv(client,reply,sizeof(reply),0); assert(std::string(reply,size)=="crosspoint (on crossdink);81");
  web.answerDiscovery(); web.closeDiscoverySocket(); assert(web.discoverySocket==-1);
  web.closeDiscoverySocket(); close(client);
  WebSocketsServer ws; ws.connected[0]=true; closeIdleWsClients(ws); assert(wsLastActivity[0]==now);
  now+=WS_IDLE_TIMEOUT_MS; closeIdleWsClients(ws); assert(ws.disconnects==0);
  ++now; closeIdleWsClients(ws); assert(ws.disconnects==1);
  ws.connected[1]=true; wsLastActivity[1]=now; now+=100000;
  wsLastActivity[1]=now; now+=100000; closeIdleWsClients(ws); assert(ws.disconnects==1);
  // Unsigned time differences work across millis() wrap.
  ws.connected[2]=true; wsLastActivity[2]=UINT32_MAX-60000; now=60001;
  closeIdleWsClients(ws); assert(!ws.connected[2]);
}
''')


if __name__ == "__main__":
    unittest.main()
