/*
 *
 * Copyright 2019 Asylo authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "asylo/platform/host_call/serializer_functions.h"

#include <ifaddrs.h>

#include "asylo/platform/system_call/type_conversions/types_functions.h"
#include "asylo/util/status_macros.h"

namespace asylo {
namespace host_call {
namespace {

using primitives::Extent;
using primitives::PrimitiveStatus;

template <typename D, typename S>
D *MallocAndCopy(S *src, size_t len) {
  void *ret = malloc(len);
  memcpy(ret, reinterpret_cast<void *>(src), len);
  return reinterpret_cast<D *>(ret);
}

// Returns true if the sa_family is AF_INET or AF_INET6, false otherwise.
bool IpCompliant(const struct sockaddr *addr) {
  return (addr->sa_family == AF_INET) || (addr->sa_family == AF_INET6);
}

bool IsIfAddrSupported(const struct ifaddrs *entry) {
  if (entry->ifa_addr && !IpCompliant(entry->ifa_addr)) return false;
  if (entry->ifa_netmask && !IpCompliant(entry->ifa_netmask)) return false;
  if (entry->ifa_ifu.ifu_dstaddr && !IpCompliant(entry->ifa_ifu.ifu_dstaddr)) {
    return false;
  }
  return true;
}

// Gets the number of ifaddr nodes in a linked list of ifaddrs. Skips nodes that
// have unsupported AF families.
size_t GetNumIfAddrs(struct ifaddrs *addrs) {
  size_t ret = 0;
  for (struct ifaddrs *addr = addrs; addr != nullptr; addr = addr->ifa_next) {
    if (IsIfAddrSupported(addr)) ret++;
  }
  return ret;
}

size_t GetNumAddrinfos(struct addrinfo *addrs) {
  size_t ret = 0;
  for (struct addrinfo *addr = addrs; addr != nullptr; addr = addr->ai_next) {
    ret++;
  }
  return ret;
}

size_t GetSocklen(struct sockaddr *sock,
                  void (*abort_handler)(const char *message)) {
  if (!sock) return 0;
  if (sock->sa_family == AF_UNIX) return sizeof(struct sockaddr_un);
  if (sock->sa_family == AF_INET) return sizeof(struct sockaddr_in);
  if (sock->sa_family == AF_INET6) return sizeof(struct sockaddr_in6);
  abort_handler("GetSocklen: Unsupported sa_family encountered.");

  // Unreachable
  return 0;
}

// Converts a native sockaddr to a klinux sockaddr and pushes on the
// MessageWriter.
PrimitiveStatus PushkLinuxSockAddrToWriter(
    primitives::MessageWriter *writer, struct sockaddr *sock,
    void (*abort_handler)(const char *message)) {
  if (!writer) {
    return {error::GoogleError::INVALID_ARGUMENT,
            "PushkLinuxSockAddr: Null MessageWriter provided."};
  }
  if (!sock) {
    writer->PushByCopy(Extent{nullptr, 0});
    return PrimitiveStatus::OkStatus();
  }

  socklen_t klinux_sock_len =
      std::max(std::max(sizeof(klinux_sockaddr_un), sizeof(klinux_sockaddr_in)),
               sizeof(klinux_sockaddr_in6));
  auto klinux_sock = absl::make_unique<char[]>(klinux_sock_len);

  if (!TokLinuxSockAddr(sock, GetSocklen(sock, abort_handler),
                        reinterpret_cast<klinux_sockaddr *>(klinux_sock.get()),
                        &klinux_sock_len, abort_handler)) {
    return {error::GoogleError::INVALID_ARGUMENT,
            "TestGetAddrInfo: Couldn't convert sockaddr to klinux_sockaddr"};
  }
  writer->PushByCopy(Extent{klinux_sock.get(), klinux_sock_len});
  return PrimitiveStatus::OkStatus();
}

}  // namespace

bool DeserializeAddrinfo(primitives::MessageReader *in, struct addrinfo **out,
                         void (*abort_handler)(const char *message)) {
  if (!in || !out || in->empty()) return false;
  size_t num_addrs = in->next<size_t>();

  if (num_addrs == 0) {
    if (out) {
      *out = nullptr;
    }
    return true;
  }
  // 6 entries per addrinfo expected on |in| for deserialization.
  if (in->size() < num_addrs * 6) return false;

  struct addrinfo *prev_info = nullptr;
  for (int i = 0; i < num_addrs; i++) {
    auto info = static_cast<struct addrinfo *>(malloc(sizeof(struct addrinfo)));
    if (info == nullptr) {
      // Roll back info and linked list constructed until now.
      freeaddrinfo(info);
      freeaddrinfo(*out);
      return false;
    }
    memset(info, 0, sizeof(struct addrinfo));

    info->ai_flags = FromkLinuxAddressInfoFlag(in->next<int>());
    info->ai_family = FromkLinuxAfFamily(in->next<int>());
    info->ai_socktype = FromkLinuxSocketType(in->next<int>());
    info->ai_protocol = in->next<int>();
    Extent klinux_sockaddr_buf = in->next();
    Extent ai_canonname = in->next();

    if (info->ai_socktype == -1) {
      // Roll back info and linked list constructed until now.
      freeaddrinfo(info);
      freeaddrinfo(*out);
      return false;
    }

    // Optionally set ai_addr and ai_addrlen.
    if (!klinux_sockaddr_buf.empty()) {
      const struct klinux_sockaddr *klinux_sock =
          klinux_sockaddr_buf.As<struct klinux_sockaddr>();
      struct sockaddr_storage sock {};
      socklen_t socklen = sizeof(struct sockaddr_storage);
      if (!FromkLinuxSockAddr(klinux_sock, klinux_sockaddr_buf.size(),
                              reinterpret_cast<sockaddr *>(&sock), &socklen,
                              abort_handler)) {
        // Roll back info and linked list constructed until now.
        freeaddrinfo(info);
        freeaddrinfo(*out);
        return false;
      }
      info->ai_addrlen = socklen;
      info->ai_addr = MallocAndCopy<sockaddr>(&sock, socklen);
    } else {
      info->ai_addr = nullptr;
      info->ai_addrlen = 0;
    }

    // Optionally set ai_canonname.
    info->ai_canonname =
        ai_canonname.empty() ? nullptr : strdup(ai_canonname.As<char>());

    // Construct addrinfo linked list.
    info->ai_next = nullptr;
    if (!prev_info) {
      *out = info;
    } else {
      prev_info->ai_next = info;
    }
    prev_info = info;
  }

  return true;
}

bool DeserializeIfAddrs(primitives::MessageReader *in, struct ifaddrs **out,
                        void (*abort_handler)(const char *message)) {
  if (!in || !out || in->empty()) return false;
  size_t num_ifaddrs = in->next<size_t>();
  if (num_ifaddrs == 0) {
    if (out != nullptr) *out = nullptr;
    return true;
  }

  // 5 entries per ifaddr expected on |in| for deserialization.
  if (in->size() < num_ifaddrs * 5) return false;

  struct ifaddrs *prev_addrs = nullptr;
  for (int i = 0; i < num_ifaddrs; i++) {
    auto addrs =
        static_cast<struct ifaddrs *>(calloc(1, sizeof(struct ifaddrs)));
    if (addrs == nullptr) {
      // Roll back current ifaddrs node and linked list constructed until now.
      freeifaddrs(addrs);
      freeifaddrs(*out);
      return false;
    }

    Extent ifa_name_buf = in->next();
    auto klinux_ifa_flags = in->next<unsigned int>();
    Extent klinux_ifa_addr_buf = in->next();
    Extent klinux_ifa_netmask_buf = in->next();
    Extent klinux_ifa_dstaddr_buf = in->next();

    addrs->ifa_name = strdup(ifa_name_buf.As<char>());
    addrs->ifa_flags = FromkLinuxIffFlag(klinux_ifa_flags);
    addrs->ifa_data = nullptr;  // Unsupported

    // Optionally set addrs->ifa_addr.
    if (!klinux_ifa_addr_buf.empty()) {
      const struct klinux_sockaddr *klinux_sock =
          klinux_ifa_addr_buf.As<struct klinux_sockaddr>();
      struct sockaddr_storage sock {};
      socklen_t socklen = sizeof(struct sockaddr_storage);
      if (!FromkLinuxSockAddr(klinux_sock, klinux_ifa_addr_buf.size(),
                              reinterpret_cast<sockaddr *>(&sock), &socklen,
                              abort_handler)) {
        // Roll back info and linked list constructed until now.
        freeifaddrs(addrs);
        freeifaddrs(*out);
        return false;
      }
      addrs->ifa_addr = MallocAndCopy<sockaddr>(&sock, socklen);
    } else {
      addrs->ifa_addr = nullptr;
    }

    // Optionally set addrs->ifa_netmask.
    if (!klinux_ifa_netmask_buf.empty()) {
      const struct klinux_sockaddr *klinux_sock =
          klinux_ifa_netmask_buf.As<struct klinux_sockaddr>();
      struct sockaddr_storage sock {};
      socklen_t socklen = sizeof(struct sockaddr_storage);
      if (!FromkLinuxSockAddr(klinux_sock, klinux_ifa_netmask_buf.size(),
                              reinterpret_cast<sockaddr *>(&sock), &socklen,
                              abort_handler)) {
        // Roll back current ifaddrs node and linked list constructed until now.
        freeifaddrs(addrs);
        freeifaddrs(*out);
        return false;
      }
      addrs->ifa_netmask = MallocAndCopy<sockaddr>(&sock, socklen);
    } else {
      addrs->ifa_netmask = nullptr;
    }

    // Optionally set addrs->ifa_ifu.ifu_dstaddr.
    if (!klinux_ifa_dstaddr_buf.empty()) {
      const struct klinux_sockaddr *klinux_sock =
          klinux_ifa_dstaddr_buf.As<struct klinux_sockaddr>();
      struct sockaddr_storage sock {};
      socklen_t socklen = sizeof(struct sockaddr_storage);
      if (!FromkLinuxSockAddr(klinux_sock, klinux_ifa_dstaddr_buf.size(),
                              reinterpret_cast<sockaddr *>(&sock), &socklen,
                              abort_handler)) {
        // Roll back current ifaddrs node and linked list constructed until now.
        freeifaddrs(addrs);
        freeifaddrs(*out);
        return false;
      }
      addrs->ifa_ifu.ifu_dstaddr = MallocAndCopy<sockaddr>(&sock, socklen);
    } else {
      addrs->ifa_ifu.ifu_dstaddr = nullptr;
    }

    // Construct ifaddrs linked list.
    addrs->ifa_next = nullptr;
    if (!prev_addrs) {
      *out = addrs;
    } else {
      prev_addrs->ifa_next = addrs;
    }
    prev_addrs = addrs;
  }

  return true;
}

PrimitiveStatus SerializeAddrInfo(primitives::MessageWriter *writer,
                                  struct addrinfo *addrs,
                                  void (*abort_handler)(const char *message),
                                  bool explicit_klinux_conversion) {
  if (!writer) {
    return {error::GoogleError::INVALID_ARGUMENT,
            "SerializeAddrInfo: Null writer provided"};
  }
  writer->Push<uint64_t>(GetNumAddrinfos(addrs));

  for (struct addrinfo *addr = addrs; addr != nullptr; addr = addr->ai_next) {
    // We push 6 entries per addrinfo to output.
    writer->Push<int>(TokLinuxAddressInfoFlag(addr->ai_flags));
    writer->Push<int>(TokLinuxAfFamily(addr->ai_family));
    writer->Push<int>(TokLinuxSocketType(addr->ai_socktype));
    writer->Push<int>(addr->ai_protocol);

    if (!explicit_klinux_conversion) {
      writer->PushByCopy(
          Extent{reinterpret_cast<char *>(addr->ai_addr), addr->ai_addrlen});
    } else {
      socklen_t klinux_sock_len = std::max(
          std::max(sizeof(klinux_sockaddr_un), sizeof(klinux_sockaddr_in)),
          sizeof(klinux_sockaddr_in6));
      auto klinux_sock = absl::make_unique<char[]>(klinux_sock_len);

      if (!TokLinuxSockAddr(
              addr->ai_addr, addr->ai_addrlen,
              reinterpret_cast<klinux_sockaddr *>(klinux_sock.get()),
              &klinux_sock_len, abort_handler)) {
        return {
            error::GoogleError::INVALID_ARGUMENT,
            "SerializeAddrInfo: Couldn't convert sockaddr to klinux_sockaddr"};
      }
      writer->PushByCopy(Extent{klinux_sock.get(), klinux_sock_len});
    }

    writer->PushByCopy(Extent{
        addr->ai_canonname,
        addr->ai_canonname == nullptr ? 0 : strlen(addr->ai_canonname) + 1});
  }

  return PrimitiveStatus::OkStatus();
}

PrimitiveStatus SerializeIfAddrs(primitives::MessageWriter *writer,
                                 struct ifaddrs *ifaddr_list,
                                 void (*abort_handler)(const char *message),
                                 bool explicit_klinux_conversion) {
  if (!writer || !ifaddr_list) {
    return {error::GoogleError::INVALID_ARGUMENT,
            "SerializeIfAddrs: NULL MessageWriter or ifaddrs provided."};
  }
  writer->Push<uint64_t>(GetNumIfAddrs(ifaddr_list));

  for (struct ifaddrs *addr = ifaddr_list; addr != nullptr;
       addr = addr->ifa_next) {
    // If the entry is of a format we don't support, don't include it.
    if (!IsIfAddrSupported(addr)) continue;

    // We push 5 entries per ifaddr to output.
    writer->PushString(addr->ifa_name);
    writer->Push<uint32_t>(TokLinuxIffFlag(addr->ifa_flags));

    if (!explicit_klinux_conversion) {
      ASYLO_RETURN_IF_ERROR(writer->PushSockAddr(addr->ifa_addr));
      ASYLO_RETURN_IF_ERROR(writer->PushSockAddr(addr->ifa_netmask));
      ASYLO_RETURN_IF_ERROR(writer->PushSockAddr(addr->ifa_ifu.ifu_dstaddr));
    } else {
      ASYLO_RETURN_IF_ERROR(
          PushkLinuxSockAddrToWriter(writer, addr->ifa_addr, abort_handler));
      ASYLO_RETURN_IF_ERROR(
          PushkLinuxSockAddrToWriter(writer, addr->ifa_netmask, abort_handler));
      ASYLO_RETURN_IF_ERROR(PushkLinuxSockAddrToWriter(
          writer, addr->ifa_ifu.ifu_dstaddr, abort_handler));
    }
  }

  return PrimitiveStatus::OkStatus();
}

}  // namespace host_call
}  // namespace asylo
