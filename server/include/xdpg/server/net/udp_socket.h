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

// Endpoint 是 sockaddr_storage 的轻量封装。
// 现在 server 只绑定 IPv4 UDP，但这里使用 sockaddr_storage，是为了后续扩展 IPv6
// 或从不同 socket API 收到地址时不用改上层 DS 逻辑。
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

    // 用于日志输出，例如 127.0.0.1:50000。失败时返回占位字符串，不抛异常。
    std::string ToString() const;

private:
    sockaddr_storage storage_{};
    socklen_t length_ = sizeof(storage_);
    bool valid_ = false;
};

// 第一阶段的普通 UDP socket 封装。
// 目标不是隐藏所有平台差异，而是把 Windows Winsock / Linux fd 的差异挡在 net 层，
// 让 app 层只面对 Open/Receive/Send 三个动作。
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
    //
    // socket 被设置为 non-blocking。调用方可以在一帧内反复 Receive，
    // 直到返回 0，再去推进 simulation，避免单次只读一个包造成积压。
    int Receive(std::uint8_t* buffer, std::size_t capacity, Endpoint* from, std::string* error);

    // UDP sendto 可能失败，例如目标 endpoint 无效、网络不可达、socket 已关闭。
    // 第一阶段只记录错误，不做可靠重传。
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
