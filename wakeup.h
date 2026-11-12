#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>
#include <vector>

// A file descriptor that closes itself.
class FileDescriptor {
  public:
    FileDescriptor() = default;
    explicit FileDescriptor(int descriptor) : descriptor(descriptor) {}
    FileDescriptor(FileDescriptor&& other) noexcept : descriptor(other.release()) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept
    {
        reset(other.release());
        return *this;
    }
    ~FileDescriptor() { reset(); }
    [[nodiscard]] int get() const { return descriptor; }
    explicit operator bool() const { return descriptor >= 0; }
    [[nodiscard]] int release() { return std::exchange(descriptor, -1); }
    void reset(int next = -1)
    {
        if (descriptor >= 0)
            ::close(descriptor);
        descriptor = next;
    }

  private:
    int descriptor = -1;
};

// Wakes a poll() on another thread. Raising it twice before it is read wakes once.
class WakeSignal {
  public:
    WakeSignal() : event(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
    {
        if (!event)
            throw std::runtime_error("Could not create a wakeup descriptor");
    }
    [[nodiscard]] int fd() const { return event.get(); }
    void raise() const
    {
        const std::uint64_t value = 1;
        (void)!::write(event.get(), &value, sizeof(value));
    }
    void clear() const
    {
        std::uint64_t value;
        while (::read(event.get(), &value, sizeof(value)) > 0) {
        }
    }

  private:
    FileDescriptor event;
};

// What one thread hands to another; fd() becomes readable when there is something to take.
template <class Item> class Mailbox {
  public:
    [[nodiscard]] int fd() const { return signal.fd(); }
    void post(Item item)
    {
        {
            const std::scoped_lock lock(mutex);
            items.push_back(std::move(item));
        }
        signal.raise();
    }
    // Several at once, so the reader never sees part of a batch.
    void post(std::vector<Item> batch)
    {
        if (batch.empty())
            return;
        {
            const std::scoped_lock lock(mutex);
            std::ranges::move(batch, std::back_inserter(items));
        }
        signal.raise();
    }
    // Interrupts the reader's poll() with nothing new to take.
    void wake() const { signal.raise(); }
    [[nodiscard]] std::vector<Item> take()
    {
        signal.clear();
        const std::scoped_lock lock(mutex);
        return std::exchange(items, {});
    }

  private:
    WakeSignal signal;
    std::mutex mutex;
    std::vector<Item> items;
};
