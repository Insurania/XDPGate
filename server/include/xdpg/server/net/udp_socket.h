#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace xdpg::server {

class Endpoint {
public:
    Endpoint();

    const sockaddr* addr() const {
        return reinterpret_cast<const sockaddr*>(&storage_);
    }
    sockaddr* mutable_addr() {
        return reinterpret_cast<sockaddr*>(&storage_);
    }
    socklen_t length() const { return length_; }
    socklen_t* mutable_length() { return &length_; }
    bool valid() const { return valid_; }
    void MarkValid() { valid_ = true; }

    std::string ToString() const;

private:
    sockaddr_storage storage_{};
    socklen_t length_ = sizeof(storage_);
    bool valid_ = false;
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool Open(std::uint16_t port, std::string* error);
    void Close();

    // 返回值语义：
    // >0: 收到的字节数；0: 当前没有可读 UDP 包；<0: socket 错误。
    int Receive(std::uint8_t* buffer, std::size_t capacity, Endpoint* from, std::string* error);
    bool Send(const std::uint8_t* data, std::size_t size, const Endpoint& to, std::string* error);

private:
#if defined(_WIN32)
    using SocketHandle = SOCKET;
    static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
    using SocketHandle = int;
    static constexpr SocketHandle kInvalidSocket = -1;
#endif

    bool SetNonBlocking(std::string* error);
    SocketHandle socket_ = kInvalidSocket;
};

}  // namespace xdpg::server

