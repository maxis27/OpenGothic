// NetAddress test: parsing of the -host <port> and -connect <ip:port> command line values.
// Usage: NetAddressTest. Exits with 0 on success.

#include "net/netaddress.h"

#include <cstdio>

using namespace NetAddress;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if(cond)
    return;
  std::fprintf(stderr, "FAILED: %s\n", what);
  ++failures;
  }

void testPort() {
  check(parsePort("1")==uint16_t(1),         "port 1");
  check(parsePort("47611")==uint16_t(47611), "port 47611");
  check(parsePort("65535")==uint16_t(65535), "port 65535");
  check(parsePort("00080")==uint16_t(80),    "port with leading zeros");

  check(!parsePort(""),       "empty port");
  check(!parsePort("0"),      "port 0");
  check(!parsePort("65536"),  "port above 65535");
  check(!parsePort("100000"), "port with six digits");
  check(!parsePort("-1"),     "negative port");
  check(!parsePort("+80"),    "port with sign");
  check(!parsePort(" 80"),    "port with space");
  check(!parsePort("80x"),    "port with trailing text");
  check(!parsePort("0x50"),   "hex port");
  }

void testEndpoint() {
  auto ep = parseEndpoint("127.0.0.1:47611");
  check(ep && ep->host=="127.0.0.1" && ep->port==47611, "IPv4 endpoint");

  ep = parseEndpoint("khorinis.example.org:1");
  check(ep && ep->host=="khorinis.example.org" && ep->port==1, "host name endpoint");

  check(!parseEndpoint("127.0.0.1"),       "endpoint without port");
  check(!parseEndpoint("127.0.0.1:"),      "endpoint with empty port");
  check(!parseEndpoint(":47611"),          "endpoint without host");
  check(!parseEndpoint("127.0.0.1:0"),     "endpoint with port 0");
  check(!parseEndpoint("127.0.0.1:70000"), "endpoint with port above 65535");
  check(!parseEndpoint("::1:47611"),       "IPv6 literal");
  check(!parseEndpoint("[::1]:47611"),     "bracketed IPv6 literal");
  check(!parseEndpoint(""),                "empty endpoint");
  }

}

int main() {
  testPort();
  testEndpoint();
  if(failures>0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
    }
  std::printf("NetAddress test passed\n");
  return 0;
  }
