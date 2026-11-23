#include "ipc.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

#ifdef __linux__
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace stride {
static_assert(sizeof(std::uint64_t) == 8);

std::uint64_t clockMilliseconds() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::uint64_t encode(Packet packet) {
    const auto quantize = [](float value, unsigned max) {
        return std::isfinite(value) ? static_cast<unsigned>(std::lround(std::clamp(value, 0.0f, 1.0f) * max)) : 0u;
    };
    return (packet.timestamp << 24) | ((packet.flags & 63u) << 18) |
           (quantize(packet.threshold, 255) << 10) | quantize(packet.value, 1023);
}

Packet current(std::uint64_t bits, std::uint64_t now, unsigned maxAge) {
    const auto timestamp = bits >> 24;
    if (!timestamp || timestamp > now || now - timestamp >= maxAge) return {};
    return {static_cast<float>(bits & 1023u) / 1023, static_cast<float>((bits >> 10) & 255u) / 255,
            static_cast<unsigned>((bits >> 18) & 63u), timestamp};
}

#ifdef __linux__
static_assert(__atomic_always_lock_free(sizeof(std::uint64_t), nullptr));

Mailbox::~Mailbox() { disconnect(); }

void Mailbox::disconnect() {
    if (!word_) return;
    if (writer_) __atomic_store_n(word_, 0, __ATOMIC_RELEASE);
    munmap(word_, sizeof(*word_));
    if (writer_) flock(fd_, LOCK_UN);
    close(fd_);
    word_ = nullptr;
    fd_ = -1;
    nextAttempt_ = 0;
}

bool Mailbox::connect(std::uint64_t now) {
    if (word_) return true;
    if (now < nextAttempt_) return false;
    nextAttempt_ = now + 125;
    const auto uid = geteuid();
    const char* alternate = std::getenv("FRAMESTRIDE_RUNTIME_DIR");
    const std::string base = alternate && *alternate ? alternate : "/run/user/" + std::to_string(uid);
    const std::string directory = base + "/framestride";
    if (writer_) mkdir(directory.c_str(), 0700);
    const int dir = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return false;
    struct stat status{};
    const bool privateDir = fstat(dir, &status) == 0 && status.st_uid == uid && (status.st_mode & 077) == 0;
    constexpr const char* names[] = {"motion.v4", "driver.v4", "openxr.v4"};
    const int fd = privateDir ? openat(dir, names[static_cast<unsigned>(channel_)],
                                      (writer_ ? O_RDWR | O_CREAT : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK,
                                      0600) : -1;
    close(dir);
    if (fd < 0) return false;
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != uid || (status.st_mode & 077) != 0 ||
        (writer_ && (flock(fd, LOCK_EX | LOCK_NB) != 0 ||
                    (status.st_size != sizeof(std::uint64_t) && ftruncate(fd, sizeof(std::uint64_t)) != 0))) ||
        (!writer_ && status.st_size != sizeof(std::uint64_t))) {
        close(fd);
        return false;
    }
    void* address = mmap(nullptr, sizeof(std::uint64_t), writer_ ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) {
        close(fd);
        return false;
    }
    fd_ = fd;
    word_ = static_cast<std::uint64_t*>(address);
    if (writer_) __atomic_store_n(word_, 0, __ATOMIC_RELEASE);
    return true;
}

Packet Mailbox::read(std::uint64_t now, unsigned age) {
    return connect(now) ? current(__atomic_load_n(word_, __ATOMIC_ACQUIRE), now, age) : Packet{};
}

bool Mailbox::write(float value, float threshold, unsigned flags, std::uint64_t now) {
    if (!writer_ || !connect(now)) return false;
    __atomic_store_n(word_, encode({value, threshold, flags, now}), __ATOMIC_RELEASE);
    return true;
}
#else
Mailbox::~Mailbox() = default;
#endif

}
