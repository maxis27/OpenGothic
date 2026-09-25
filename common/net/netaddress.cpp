#include "netaddress.h"

using namespace NetAddress;

std::optional<uint16_t> NetAddress::parsePort(std::string_view text) {
  if(text.empty() || text.size()>5)
    return std::nullopt;
  uint32_t port = 0;
  for(char c:text) {
    if(c<'0' || c>'9')
      return std::nullopt;
    port = port*10 + uint32_t(c-'0');
    }
  if(port==0 || port>0xFFFF)
    return std::nullopt;
  return uint16_t(port);
  }

std::optional<Endpoint> NetAddress::parseEndpoint(std::string_view text) {
  const size_t colon = text.rfind(':');
  if(colon==std::string_view::npos)
    return std::nullopt;
  const std::string_view host = text.substr(0, colon);
  if(host.empty() || host.find(':')!=std::string_view::npos)
    return std::nullopt;
  const auto port = parsePort(text.substr(colon+1));
  if(!port)
    return std::nullopt;
  return Endpoint{std::string(host), *port};
  }
