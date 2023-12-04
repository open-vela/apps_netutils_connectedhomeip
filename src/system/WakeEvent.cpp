/*
 *
 *    Copyright (c) 2020-2021 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *      This file declares the abstraction of one-directional, anonymous
 *      data stream built on top of two file descriptors.
 */

#include <system/SystemConfig.h>

#if CHIP_SYSTEM_CONFIG_USE_SOCKETS && !CHIP_SYSTEM_CONFIG_USE_LIBEV

#include <system/WakeEvent.h>

// Include additional CHIP headers
#include <lib/support/CodeUtils.h>
#include <system/SystemError.h>
#include <system/SystemLayer.h>

// Include system and language headers
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if !CHIP_SYSTEM_CONFIG_USE_POSIX_PIPE
#if CHIP_SYSTEM_CONFIG_USE_ZEPHYR_EVENTFD
#include <zephyr/posix/sys/eventfd.h>
#else
#include <sys/eventfd.h>
#endif
#endif // !CHIP_SYSTEM_CONFIG_USE_POSIX_PIPE

namespace chip {
namespace System {

#if CHIP_SYSTEM_CONFIG_USE_POSIX_PIPE

namespace {
inline int SetNonBlockingMode(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
} // anonymous namespace

constexpr const char kChipEventFifoPath[] = "/var/wake_event_fifo";

CHIP_ERROR WakeEvent::Open(LayerSockets & systemLayer)
{
    enum
    {
        FD_READ  = 0,
        FD_WRITE = 1
    };
    int fds[2];

#ifdef CHIP_SYSTEM_CONFIG_WAKE_EVENT_USE_FIFO
    /* In order to support cross-thread and process event notifications */
    VerifyOrReturnError(::mkfifo(kChipEventFifoPath, 0666) == 0 || errno == EEXIST, CHIP_ERROR_OPEN_FAILED);

    fds[FD_READ] = ::open(kChipEventFifoPath, O_RDONLY | O_NONBLOCK);
    if (fds[FD_READ] == -1)
    {
        ChipLogError(chipSystemLayer, "System wake event failed to open fifo: %" CHIP_ERROR_FORMAT,
                     CHIP_ERROR_POSIX(errno).Format());
        return CHIP_ERROR_POSIX(errno);
    }

    fds[FD_WRITE] = ::open(kChipEventFifoPath, O_WRONLY | O_NONBLOCK);
    if (fds[FD_WRITE] == -1)
    {
        ChipLogError(chipSystemLayer, "System wake event failed to open fifo: %" CHIP_ERROR_FORMAT,
                     CHIP_ERROR_POSIX(errno).Format());
        return CHIP_ERROR_POSIX(errno);
    }
#else
    if (::pipe(fds) < 0)
        return CHIP_ERROR_POSIX(errno);

    if (SetNonBlockingMode(fds[FD_READ]) < 0)
        return CHIP_ERROR_POSIX(errno);

    if (SetNonBlockingMode(fds[FD_WRITE]) < 0)
        return CHIP_ERROR_POSIX(errno);
#endif

    mReadFD  = fds[FD_READ];
    mWriteFD = fds[FD_WRITE];

    ReturnErrorOnFailure(systemLayer.StartWatchingSocket(mReadFD, &mReadWatch));
    ReturnErrorOnFailure(systemLayer.SetCallback(mReadWatch, Confirm, reinterpret_cast<intptr_t>(this)));
    ReturnErrorOnFailure(systemLayer.RequestCallbackOnPendingRead(mReadWatch));

    return CHIP_NO_ERROR;
}

void WakeEvent::Close(LayerSockets & systemLayer)
{
    systemLayer.StopWatchingSocket(&mReadWatch);
    VerifyOrDie(::close(mReadFD) == 0);
    VerifyOrDie(::close(mWriteFD) == 0);
    mReadFD  = -1;
    mWriteFD = -1;
}

void WakeEvent::Confirm() const
{
    uint8_t buffer[128];
    ssize_t res;

    do
    {
        res = ::read(mReadFD, buffer, sizeof(buffer));
        if (res < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            ChipLogError(chipSystemLayer, "System wake event confirm failed: %" CHIP_ERROR_FORMAT,
                         CHIP_ERROR_POSIX(errno).Format());
            return;
        }
    } while (res == sizeof(buffer));
}

CHIP_ERROR WakeEvent::Notify() const
{
    char byte = 1;

    if (::write(mWriteFD, &byte, 1) < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
#ifdef CHIP_SYSTEM_CONFIG_WAKE_EVENT_USE_FIFO
        int WriteFD = ::open(kChipEventFifoPath, O_WRONLY);
        if (WriteFD == -1)
        {
            ChipLogError(chipSystemLayer, "Notify event failed to open fifo: %" CHIP_ERROR_FORMAT,
                         CHIP_ERROR_POSIX(errno).Format());
            return CHIP_ERROR_POSIX(errno);
        }

        if (::write(WriteFD, &byte, 1) < 0)
        {
            ::close(WriteFD);
            ChipLogError(chipSystemLayer, "Failed to notify event by fifo: %" CHIP_ERROR_FORMAT, CHIP_ERROR_POSIX(errno).Format());
            return CHIP_ERROR_POSIX(errno);
        }

        ::close(WriteFD);
#else
        return CHIP_ERROR_POSIX(errno);
#endif
    }

    return CHIP_NO_ERROR;
}

#else // CHIP_SYSTEM_CONFIG_USE_POSIX_PIPE

namespace {

#if CHIP_SYSTEM_CONFIG_USE_ZEPHYR_EVENTFD

int ReadEvent(int eventFd)
{
    eventfd_t value;

    return eventfd_read(eventFd, &value);
}

int WriteEvent(int eventFd)
{
    return eventfd_write(eventFd, 1);
}

#else

ssize_t ReadEvent(int eventFd)
{
    uint64_t value;

    return ::read(eventFd, &value, sizeof(value));
}

ssize_t WriteEvent(int eventFd)
{
    uint64_t value = 1;

    return ::write(eventFd, &value, sizeof(value));
}

#endif

} // namespace

CHIP_ERROR WakeEvent::Open(LayerSockets & systemLayer)
{
    mReadFD = ::eventfd(0, 0);
    if (mReadFD == -1)
    {
        return CHIP_ERROR_POSIX(errno);
    }

    ReturnErrorOnFailure(systemLayer.StartWatchingSocket(mReadFD, &mReadWatch));
    ReturnErrorOnFailure(systemLayer.SetCallback(mReadWatch, Confirm, reinterpret_cast<intptr_t>(this)));
    ReturnErrorOnFailure(systemLayer.RequestCallbackOnPendingRead(mReadWatch));

    return CHIP_NO_ERROR;
}

void WakeEvent::Close(LayerSockets & systemLayer)
{
    systemLayer.StopWatchingSocket(&mReadWatch);
    VerifyOrDie(::close(mReadFD) == 0);
    mReadFD = -1;
}

void WakeEvent::Confirm() const
{
    if (ReadEvent(mReadFD) < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        ChipLogError(chipSystemLayer, "System wake event confirm failed: %" CHIP_ERROR_FORMAT, CHIP_ERROR_POSIX(errno).Format());
    }
}

CHIP_ERROR WakeEvent::Notify() const
{
    if (WriteEvent(mReadFD) < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        return CHIP_ERROR_POSIX(errno);
    }

    return CHIP_NO_ERROR;
}

#endif // CHIP_SYSTEM_CONFIG_USE_POSIX_PIPE

} // namespace System
} // namespace chip

#endif // CHIP_SYSTEM_CONFIG_USE_SOCKETS && !CHIP_SYSTEM_CONFIG_USE_LIBEV
