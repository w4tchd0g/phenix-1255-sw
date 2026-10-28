// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors

#include "TransportNet.hpp"

#include <phenix1255/protocol.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace phx {

namespace {

constexpr int UDP_RCVBUF_BYTES = 8 << 20;

std::runtime_error sysError(const std::string &what)
{
    return std::runtime_error(what + ": " + std::strerror(errno));
}

void connectWithTimeout(int sock, const sockaddr_in &addr, int timeoutMs)
{
    const int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int ret = ::connect(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS)
        throw sysError("connect");

    if (ret < 0) {
        pollfd pfd{sock, POLLOUT, 0};
        ret = ::poll(&pfd, 1, timeoutMs);
        if (ret == 0)
            throw std::runtime_error("connect: timeout");
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
        if (ret < 0 || err != 0) {
            errno = err;
            throw sysError("connect");
        }
    }

    fcntl(sock, F_SETFL, flags);
}

} // namespace

std::string TransportNet::resolve(const std::string &host)
{
    addrinfo hints{};
    hints.ai_family = AF_INET; // the device is IPv4 only
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;

    const int ret = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (ret != 0 || res == nullptr)
        throw std::runtime_error("cannot resolve " + host + ": " + gai_strerror(ret));

    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(res->ai_addr)->sin_addr, buf,
              sizeof(buf));
    freeaddrinfo(res);
    return buf;
}

TransportNet::TransportNet(const std::string &host, uint16_t ctrlPort, uint16_t samplePort,
                           int connectTimeoutMs)
    : _address(resolve(host))
{
    sockaddr_in dev{};
    dev.sin_family = AF_INET;
    inet_pton(AF_INET, _address.c_str(), &dev.sin_addr);

    // Control channel
    _tcp = ::socket(AF_INET, SOCK_STREAM, 0);
    if (_tcp < 0)
        throw sysError("socket");
    dev.sin_port = htons(ctrlPort);
    try {
        connectWithTimeout(_tcp, dev, connectTimeoutMs);
    } catch (...) {
        ::close(_tcp);
        throw;
    }
    const int one = 1;
    setsockopt(_tcp, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(_tcp, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    // Sample channel: bound to an ephemeral port, connected to the device so
    // that only its packets are received.
    _udp = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (_udp < 0) {
        ::close(_tcp);
        throw sysError("socket");
    }
    int rcvbuf = UDP_RCVBUF_BYTES;
    setsockopt(_udp, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    dev.sin_port = htons(samplePort);
    if (::bind(_udp, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0 ||
        ::connect(_udp, reinterpret_cast<sockaddr *>(&dev), sizeof(dev)) < 0) {
        ::close(_tcp);
        ::close(_udp);
        throw sysError("udp");
    }
    socklen_t len = sizeof(local);
    getsockname(_udp, reinterpret_cast<sockaddr *>(&local), &len);
    _rxPort = ntohs(local.sin_port);
}

TransportNet::~TransportNet()
{
    shutdown();
    if (_tcp >= 0)
        ::close(_tcp);
    if (_udp >= 0)
        ::close(_udp);
}

void TransportNet::shutdown()
{
    _shutdown = true;
    if (_tcp >= 0)
        ::shutdown(_tcp, SHUT_RDWR);
}

void TransportNet::sendControl(const uint8_t *msg, size_t len)
{
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    while (len > 0) {
        const ssize_t n = ::send(_tcp, msg, len, flags);
        if (n <= 0)
            throw sysError("control send");
        msg += n;
        len -= static_cast<size_t>(n);
    }
}

bool TransportNet::readExact(uint8_t *buf, size_t len, int timeoutMs)
{
    while (len > 0) {
        if (_shutdown)
            throw std::runtime_error("transport shut down");
        pollfd pfd{_tcp, POLLIN, 0};
        const int ret = ::poll(&pfd, 1, timeoutMs);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            throw sysError("control poll");
        }
        if (ret == 0)
            return false;
        const ssize_t n = ::recv(_tcp, buf, len, 0);
        if (n == 0)
            throw std::runtime_error("connection closed by device");
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            throw sysError("control recv");
        }
        buf += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool TransportNet::recvControl(std::vector<uint8_t> &msg, int timeoutMs)
{
    phx_ctrl_hdr hdr;
    if (!readExact(reinterpret_cast<uint8_t *>(&hdr), sizeof(hdr), timeoutMs))
        return false;
    if (hdr.magic != PHX_CTRL_MAGIC || hdr.length > PHX_CTRL_MAX_PAYLOAD)
        throw std::runtime_error("invalid control message header");

    msg.resize(sizeof(hdr) + hdr.length);
    std::memcpy(msg.data(), &hdr, sizeof(hdr));
    // Once a header has arrived, the payload follows immediately
    if (hdr.length > 0 && !readExact(msg.data() + sizeof(hdr), hdr.length, 2000))
        throw std::runtime_error("control message truncated");
    return true;
}

void TransportNet::sendSamples(const uint8_t *pkt, size_t len)
{
    // Lost UDP packets are reported by the device counters
    (void)::send(_udp, pkt, len, 0);
}

size_t TransportNet::recvSamples(uint8_t *buf, size_t maxLen, int timeoutMs)
{
    pollfd pfd{_udp, POLLIN, 0};
    const int ret = ::poll(&pfd, 1, timeoutMs);
    if (ret <= 0)
        return 0;
    const ssize_t n = ::recv(_udp, buf, maxLen, 0);
    return (n > 0) ? static_cast<size_t>(n) : 0;
}

} // namespace phx
