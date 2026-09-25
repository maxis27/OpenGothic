#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Parsing of the network addresses given on the command line (-host <port>, -connect <ip:port>).
namespace NetAddress {

  struct Endpoint {
    std::string host; // IPv4 address or host name
    uint16_t    port = 0;
    };

  // Decimal port in range 1..65535; anything else (signs, spaces, trailing text) is refused.
  std::optional<uint16_t> parsePort(std::string_view text);
  // "host:port"; the host must not be empty. IPv6 literals are not supported (ENet is IPv4 only).
  std::optional<Endpoint> parseEndpoint(std::string_view text);
  }
