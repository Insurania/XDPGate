#include "xdpg/server/net/udp_socket.h"

#include <cstring>
#include <sstream>

#if defined(_WIN32)
#include <mstcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace xdpg::server {
namespace {

#if defined(_WIN32)
class WinsockRuntime {
public:
    WinsockRuntime() {
        // Windows socket API 使用前必须初始化 Winsock。把它做成静态单例，
        // 可以避免每个 UdpSocket 重复 WSAStartup/WSACleanup。
        WSADATA data{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~WinsockRuntime() {
        if (ok_) {
            WSACleanup();
        }
    }

    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

WinsockRuntime& GetWinsockRuntime() {
    // 函数内 static 在 C++11 之后是线程安全初始化。
    // 当前 server 单线程使用，但这样写后续扩展也不会重复初始化 Winsock。
    static WinsockRuntime runtime;
    return runtime;
}

std::string LastSocketError() {
    return "WSA error " + std::to_string(WSAGetLastError());
}

bool IsWouldBlock() {
    // non-blocking recvfrom 没有数据时不是致命错误，上层会把它当作“本轮收包结束”。
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK;
}
#else
std::string LastSocketError() {
    return std::strerror(errno);
}

bool IsWouldBlock() {
    // Linux/BSD 上不同实现可能返回 EWOULDBLOCK 或 EAGAIN，两个都视为暂时无数据。
    return errno == EWOULDBLOCK || errno == EAGAIN;
}
#endif

}  // namespace

Endpoint::Endpoint() = default;

std::string Endpoint::ToString() const {
    if (!valid_) {
        return "<invalid>";
    }

    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};
    // 使用 getnameinfo 而不是手写 inet_ntop，是为了保留未来 IPv6 endpoint 的可能性。
    const int result = getnameinfo(addr(), length_, host, sizeof(host), service, sizeof(service),
                                   NI_NUMERICHOST | NI_NUMERICSERV);
    if (result != 0) {
        return "<unprintable>";
    }

    std::ostringstream out;
    out << host << ':' << service;
    return out.str();
}

UdpSocket::UdpSocket() = default;

UdpSocket::~UdpSocket() {
    Close();
}

bool UdpSocket::Open(std::uint16_t port, std::string* error) {
#if defined(_WIN32)
    if (!GetWinsockRuntime().ok()) {
        if (error != nullptr) {
            *error = "WSAStartup failed";
        }
        return false;
    }
#endif

    // Open 允许重复调用。先关闭旧 socket，避免端口和 fd/handle 泄漏。
    Close();
    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == kInvalidSocket) {
        if (error != nullptr) {
            *error = "socket() failed: " + LastSocketError();
        }
        return false;
    }

    // SO_REUSEADDR 主要方便开发时快速重启 server。
    // 注意它不是 SO_REUSEPORT；真正多 worker 分流会在后续阶段单独实现。
    int reuse = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    // 第一阶段只绑定 IPv4 INADDR_ANY。XDP 第一版也会先按 IPv4 UDP 做早期过滤。
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error != nullptr) {
            *error = "bind() failed: " + LastSocketError();
        }
        Close();
        return false;
    }

    return SetNonBlocking(error);
}

void UdpSocket::Close() {
    if (socket_ == kInvalidSocket) {
        return;
    }

#if defined(_WIN32)
    closesocket(socket_);
#else
    close(socket_);
#endif
    socket_ = kInvalidSocket;
}

int UdpSocket::Receive(std::uint8_t* buffer, std::size_t capacity, Endpoint* from,
                       std::string* error) {
    if (socket_ == kInvalidSocket) {
        if (error != nullptr) {
            *error = "socket is not open";
        }
        return -1;
    }

    // 每次 receive 都创建一个新的 Endpoint，避免上一次来源地址残留。
    Endpoint endpoint;
    const int received = recvfrom(socket_, reinterpret_cast<char*>(buffer),
                                  static_cast<int>(capacity), 0, endpoint.mutable_addr(),
                                  endpoint.mutable_length());
    if (received < 0) {
        if (IsWouldBlock()) {
            return 0;
        }
        if (error != nullptr) {
            *error = "recvfrom() failed: " + LastSocketError();
        }
        return -1;
    }

    endpoint.MarkValid();
    if (from != nullptr) {
        *from = endpoint;
    }
    return received;
}

bool UdpSocket::Send(const std::uint8_t* data, std::size_t size, const Endpoint& to,
                     std::string* error) {
    if (socket_ == kInvalidSocket || !to.valid()) {
        if (error != nullptr) {
            *error = "socket or endpoint is invalid";
        }
        return false;
    }

    // UDP datagram 要么作为一个 packet 发送，要么失败；如果 sendto 返回短写，
    // 这里也当成错误处理，避免上层误以为 snapshot 已完整发出。
    const int sent = sendto(socket_, reinterpret_cast<const char*>(data), static_cast<int>(size),
                            0, to.addr(), to.length());
    if (sent < 0 || static_cast<std::size_t>(sent) != size) {
        if (error != nullptr) {
            *error = "sendto() failed: " + LastSocketError();
        }
        return false;
    }
    return true;
}

bool UdpSocket::SetNonBlocking(std::string* error) {
#if defined(_WIN32)
    // Windows 用 ioctlsocket 设置 non-blocking。
    u_long mode = 1;
    if (ioctlsocket(socket_, FIONBIO, &mode) != 0) {
        if (error != nullptr) {
            *error = "ioctlsocket(FIONBIO) failed: " + LastSocketError();
        }
        return false;
    }
#else
    // Linux/Unix 用 fcntl 追加 O_NONBLOCK，保留原有 fd flags。
    const int flags = fcntl(socket_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) != 0) {
        if (error != nullptr) {
            *error = "fcntl(O_NONBLOCK) failed: " + LastSocketError();
        }
        return false;
    }
#endif
    return true;
}

}  // namespace xdpg::server
