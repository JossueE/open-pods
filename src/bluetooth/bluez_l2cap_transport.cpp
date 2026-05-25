#include "bluetooth/bluez_l2cap_transport.hpp"
#include "bluetooth/aacp.hpp"

#if __has_include(<bluetooth/bluetooth.h>) && __has_include(<bluetooth/l2cap.h>)
#    include <bluetooth/bluetooth.h>
#    include <bluetooth/l2cap.h>
#    include <fcntl.h>
#    include <poll.h>
#    include <sys/socket.h>
#    include <unistd.h>
#    define OPENPODS_HAS_BLUEZ_L2CAP 1
#else
#    define OPENPODS_HAS_BLUEZ_L2CAP 0
#endif

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

BluezL2capTransport::~BluezL2capTransport()
{
    close();
}

bool BluezL2capTransport::connect(std::string_view mac_address, uint16_t psm)
{
#if !OPENPODS_HAS_BLUEZ_L2CAP
    (void)mac_address;
    (void)psm;
    std::cerr << "BlueZ L2CAP headers are not available\n";
    return false;
#else
    close();

    const int fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (fd < 0) {
        std::cerr << "Failed to create L2CAP socket: " << std::strerror(errno) << '\n';
        return false;
    }

    // BlueZ 5.86+ requires an explicit security level on BR/EDR L2CAP sockets.
    // Without it, connect() succeeds but the kernel drops the channel before the
    // first send() with ENOTCONN.
    bt_security security {};
    security.level = BT_SECURITY_MEDIUM;
    security.key_size = 0;
    if (::setsockopt(fd, SOL_BLUETOOTH, BT_SECURITY, &security, sizeof(security)) < 0) {
        std::cerr << "Failed to set L2CAP security level: " << std::strerror(errno) << '\n';
        ::close(fd);
        return false;
    }

    sockaddr_l2 address {};
    address.l2_family = AF_BLUETOOTH;
    address.l2_psm = htobs(psm);
    address.l2_bdaddr_type = BDADDR_BREDR;

    const std::string mac(mac_address);
    if (str2ba(mac.c_str(), &address.l2_bdaddr) != 0) {
        std::cerr << "Invalid Bluetooth address: " << mac << '\n';
        ::close(fd);
        return false;
    }

    // Use a non-blocking connect with poll-based timeout so we don't hang
    // indefinitely when the AirPods are out of range.
    const int original_flags = ::fcntl(fd, F_GETFL, 0);
    if (original_flags < 0) {
        ::close(fd);
        return false;
    }
    if (::fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return false;
    }

    int connect_result = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connect_result < 0 && errno != EINPROGRESS) {
        std::cerr << "L2CAP connect failed: " << std::strerror(errno) << '\n';
        ::close(fd);
        return false;
    }

    if (connect_result < 0) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;

        const auto timeout_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(CONNECT_TIMEOUT).count()
        );
        const int poll_result = ::poll(&pfd, 1, timeout_ms);
        if (poll_result <= 0) {
            std::cerr << "L2CAP connect timed out\n";
            ::close(fd);
            return false;
        }

        int socket_error = 0;
        socklen_t error_len = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) < 0
            || socket_error != 0) {
            std::cerr << "L2CAP connect failed: "
                      << std::strerror(socket_error == 0 ? errno : socket_error) << '\n';
            ::close(fd);
            return false;
        }
    }

    if (::fcntl(fd, F_SETFL, original_flags) < 0) {
        ::close(fd);
        return false;
    }

    // BlueZ takes a moment after connect() to fully establish the channel; until
    // then the peer CID is 0 and any send() returns ENOTCONN. Poll until the CID
    // becomes non-zero or the timeout elapses.
    const auto cid_deadline = std::chrono::steady_clock::now() + CONNECT_TIMEOUT;
    while (std::chrono::steady_clock::now() < cid_deadline) {
        sockaddr_l2 peer {};
        socklen_t peer_len = sizeof(peer);
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0
            && peer.l2_cid != 0) {
            break;
        }

        if (errno == ENOTCONN) {
            std::cerr << "L2CAP peer disconnected during setup\n";
            ::close(fd);
            return false;
        }

        std::this_thread::sleep_for(POLL_INTERVAL);
    }

    socket_fd_ = fd;
    return true;
#endif
}

bool BluezL2capTransport::send(const std::vector<uint8_t>& packet)
{
#if !OPENPODS_HAS_BLUEZ_L2CAP
    (void)packet;
    return false;
#else
    if (socket_fd_ < 0) {
        return false;
    }

    const auto sent = ::send(socket_fd_, packet.data(), packet.size(), 0);
    return sent >= 0 && static_cast<std::size_t>(sent) == packet.size();
#endif
}

std::optional<std::vector<uint8_t>> BluezL2capTransport::receive(std::size_t max_bytes)
{
#if !OPENPODS_HAS_BLUEZ_L2CAP
    (void)max_bytes;
    return std::nullopt;
#else
    if (socket_fd_ < 0 || max_bytes == 0) {
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(max_bytes);
    const auto received = ::recv(socket_fd_, buffer.data(), buffer.size(), 0);
    if (received <= 0) {
        return std::nullopt;
    }

    buffer.resize(static_cast<std::size_t>(received));
    return buffer;
#endif
}

void BluezL2capTransport::close()
{
#if OPENPODS_HAS_BLUEZ_L2CAP
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
#else
    socket_fd_ = -1;
#endif
}
