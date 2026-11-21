#pragma once

#include <cstdint>

namespace stride {

enum class Channel { Motion, Driver, OpenXR };
enum Flag : unsigned {
    Ready = 1, Submitted = 2, XrSelected = 4, Override = 8, Tracking = 16, Physical = 32
};

constexpr unsigned watchdogMs = 120;
constexpr unsigned statusTimeoutMs = 550;

struct Packet {
    float value = 0;
    float threshold = 0;
    unsigned flags = 0;
    std::uint64_t timestamp = 0;
    bool has(Flag flag) const { return (flags & flag) != 0; }
};

std::uint64_t clockMilliseconds();
Packet current(std::uint64_t bits, std::uint64_t now, unsigned maxAge);
std::uint64_t encode(Packet packet);

class Mailbox {
public:
    Mailbox(Channel channel, bool writer) : channel_(channel), writer_(writer) {}
    ~Mailbox();
    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;
    void disconnect();
    bool connect(std::uint64_t now);
    Packet read(std::uint64_t now, unsigned maxAge = watchdogMs);
    bool write(float value, float threshold, unsigned flags, std::uint64_t now);

private:
    Channel channel_;
    bool writer_;
    int fd_ = -1;
    std::uint64_t* word_ = nullptr;
    std::uint64_t nextAttempt_ = 0;
};

}
