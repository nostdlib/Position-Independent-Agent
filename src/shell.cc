#include "shell.h"

#if defined(PLATFORM_LINUX)
#include "platform/kernel/linux/syscall.h"
#include "platform/kernel/linux/system.h"
#elif defined(PLATFORM_ANDROID)
#include "platform/kernel/android/syscall.h"
#include "platform/kernel/android/system.h"
#elif defined(PLATFORM_MACOS)
#include "platform/kernel/macos/syscall.h"
#include "platform/kernel/macos/system.h"
#elif defined(PLATFORM_IOS)
#include "platform/kernel/ios/syscall.h"
#include "platform/kernel/ios/system.h"
#elif defined(PLATFORM_SOLARIS)
#include "platform/kernel/solaris/syscall.h"
#include "platform/kernel/solaris/system.h"
#elif defined(PLATFORM_FREEBSD)
#include "platform/kernel/freebsd/syscall.h"
#include "platform/kernel/freebsd/system.h"
#endif

// ============================================================================
// PTY Shell (Linux, Android, macOS, iOS, FreeBSD, Solaris)
// ============================================================================

#if !defined(PLATFORM_WINDOWS) && !defined(PLATFORM_UEFI)

constexpr INT16 POLLIN = 0x0001;

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
#if defined(ARCHITECTURE_MIPS64)
constexpr INT32 PTY_O_NOCTTY  = 0x800;
#else
constexpr INT32 PTY_O_NOCTTY  = 0x100;
#endif
constexpr INT32 PTY_O_CLOEXEC = 0x80000;
constexpr USIZE TIOCSPTLCK    = 0x40045431;
constexpr USIZE TIOCGPTN      = 0x80045430;
#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
constexpr INT32 PTY_O_NOCTTY  = 0x20000;
constexpr INT32 PTY_O_CLOEXEC = 0x1000000;
constexpr USIZE TIOCPTYUNLK   = 0x20007452;
constexpr USIZE TIOCPTYGNAME  = 0x40807453;
#elif defined(PLATFORM_FREEBSD)
constexpr INT32 PTY_O_NOCTTY  = 0x8000;
constexpr INT32 PTY_O_CLOEXEC = 0x100000;
constexpr USIZE TIOCGPTN      = 0x4004740f;
#elif defined(PLATFORM_SOLARIS)
constexpr INT32 PTY_O_NOCTTY  = 0x800;
constexpr INT32 PTY_O_CLOEXEC = 0x800000;
constexpr USIZE TIOCSPTLCK    = 0x40045431;
constexpr USIZE TIOCGPTN      = 0x80045430;
#endif

static SSIZE PtyOpen(const char *path, INT32 flags)
{
#if (defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)) && (defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32))
    return System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)path, (USIZE)flags, 0);
#else
    return System::Call(SYS_OPEN, (USIZE)path, (USIZE)flags, 0);
#endif
}

static SSIZE ShellPoll(SSIZE fd, SSIZE timeoutMs)
{
    Pollfd pfd;
    pfd.Fd = (INT32)fd;
    pfd.Events = POLLIN;
    pfd.Revents = 0;

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID)
    Timespec ts;
    ts.Sec = timeoutMs / 1000;
    ts.Nsec = (timeoutMs % 1000) * 1000000;
    SSIZE ret = System::Call(SYS_PPOLL, (USIZE)&pfd, (USIZE)1, (USIZE)&ts, 0, 0);
#elif defined(PLATFORM_SOLARIS)
    Timespec ts;
    ts.Sec = timeoutMs / 1000;
    ts.Nsec = (timeoutMs % 1000) * 1000000;
    SSIZE ret = System::Call(SYS_POLLSYS, (USIZE)&pfd, (USIZE)1, (USIZE)&ts, 0);
#else
    SSIZE ret = System::Call(SYS_POLL, (USIZE)&pfd, (USIZE)1, (USIZE)timeoutMs);
#endif

    if (ret > 0 && (pfd.Revents & (POLLIN | POLLHUP | POLLERR)))
        return 1;
    return ret;
}

static BOOL PtyOpenPair(SSIZE &masterFd, SSIZE &slaveFd)
{
    masterFd = PtyOpen("/dev/ptmx", O_RDWR | PTY_O_NOCTTY | PTY_O_CLOEXEC);
    if (masterFd < 0)
        return false;

    char slavePath[128];

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_SOLARIS)
    INT32 unlock = 0;
    if (System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCSPTLCK, (USIZE)&unlock) < 0)
    {
        System::Call(SYS_CLOSE, (USIZE)masterFd);
        return false;
    }
    INT32 ptyNum = 0;
    if (System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCGPTN, (USIZE)&ptyNum) < 0)
    {
        System::Call(SYS_CLOSE, (USIZE)masterFd);
        return false;
    }
    // Build "/dev/pts/<N>"
    const char prefix[] = "/dev/pts/";
    USIZE i = 0;
    for (; prefix[i]; i++) slavePath[i] = prefix[i];
    if (ptyNum == 0) { slavePath[i++] = '0'; }
    else { char d[16]; USIZE n = 0; INT32 v = ptyNum; while (v > 0) { d[n++] = '0' + (v % 10); v /= 10; } while (n > 0) slavePath[i++] = d[--n]; }
    slavePath[i] = '\0';

#elif defined(PLATFORM_MACOS) || defined(PLATFORM_IOS)
    (void)System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCPTYUNLK, 0);
    if (System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCPTYGNAME, (USIZE)slavePath) < 0)
    {
        System::Call(SYS_CLOSE, (USIZE)masterFd);
        return false;
    }

#elif defined(PLATFORM_FREEBSD)
    INT32 ptyNum = 0;
    if (System::Call(SYS_IOCTL, (USIZE)masterFd, TIOCGPTN, (USIZE)&ptyNum) < 0)
    {
        System::Call(SYS_CLOSE, (USIZE)masterFd);
        return false;
    }
    const char prefix[] = "/dev/pts/";
    USIZE i = 0;
    for (; prefix[i]; i++) slavePath[i] = prefix[i];
    if (ptyNum == 0) { slavePath[i++] = '0'; }
    else { char d[16]; USIZE n = 0; INT32 v = ptyNum; while (v > 0) { d[n++] = '0' + (v % 10); v /= 10; } while (n > 0) slavePath[i++] = d[--n]; }
    slavePath[i] = '\0';
#endif

    slaveFd = PtyOpen(slavePath, O_RDWR | PTY_O_NOCTTY);
    if (slaveFd < 0)
    {
        System::Call(SYS_CLOSE, (USIZE)masterFd);
        return false;
    }
    return true;
}

// -- Shell implementation --

Shell::Shell(Process &&proc, SSIZE master) noexcept
    : process(static_cast<Process &&>(proc)), masterFd(master) {}

Shell::~Shell() noexcept
{
    if (masterFd >= 0)
        System::Call(SYS_CLOSE, (USIZE)masterFd);
}

Shell::Shell(Shell &&other) noexcept
    : process(static_cast<Process &&>(other.process)), masterFd(other.masterFd)
{
    other.masterFd = -1;
}

Result<USIZE, Error> Shell::Write(const char *data, USIZE length) noexcept
{
    SSIZE ret = System::Call(SYS_WRITE, (USIZE)masterFd, (USIZE)data, length);
    if (ret < 0)
        return Result<USIZE, Error>::Err(Error::Pipe_WriteFailed);
    return Result<USIZE, Error>::Ok((USIZE)ret);
}

Result<USIZE, Error> Shell::Read(char *buffer, USIZE capacity) noexcept
{
    USIZE totalRead = 0;

    while (totalRead < capacity - 1)
    {
        SSIZE pollRet = ShellPoll(masterFd, (totalRead == 0) ? 5000 : 100);
        if (pollRet <= 0)
            break;

        SSIZE ret = System::Call(SYS_READ, (USIZE)masterFd, (USIZE)(buffer + totalRead), (capacity - 1) - totalRead);
        if (ret <= 0)
            break;

        bool promptFound = false;
        for (SSIZE i = 0; i < ret; ++i)
        {
            if (buffer[totalRead + i] == Shell::EndOfLineChar)
            {
                promptFound = true;
                break;
            }
        }

        totalRead += (USIZE)ret;
        if (promptFound)
            break;
    }

    return Result<USIZE, Error>::Ok(totalRead);
}

Result<USIZE, Error> Shell::ReadError([[maybe_unused]] char *buffer, [[maybe_unused]] USIZE capacity) noexcept
{
    return Result<USIZE, Error>::Ok(0);
}

Result<Shell, Error> Shell::Create() noexcept
{
    SSIZE masterFd, slaveFd;
    if (!PtyOpenPair(masterFd, slaveFd))
        return Result<Shell, Error>::Err(Error::Pipe_CreateFailed);

    const CHAR *args[] = {"/bin/sh", nullptr};
    auto processResult = Process::Create("/bin/sh", args, slaveFd, slaveFd, slaveFd);
    System::Call(SYS_CLOSE, (USIZE)slaveFd);

    if (!processResult)
    {
        System::Call(SYS_CLOSE, (USIZE)masterFd);
        return Result<Shell, Error>::Err(Error::Process_CreateFailed);
    }

    return Result<Shell, Error>::Ok(
        Shell(static_cast<Process &&>(processResult.Value()), masterFd));
}

// ============================================================================
// Pipe Shell (Windows / UEFI)
// ============================================================================

#else

Shell::Shell(Process &&proc, Pipe &&inP, Pipe &&outP, Pipe &&errP) noexcept
    : process(static_cast<Process &&>(proc)),
      stdinPipe(static_cast<Pipe &&>(inP)),
      stdoutPipe(static_cast<Pipe &&>(outP)),
      stderrPipe(static_cast<Pipe &&>(errP)) {}

Shell::~Shell() noexcept = default;
Shell::Shell(Shell &&other) noexcept = default;

Result<USIZE, Error> Shell::Write(const char *data, USIZE length) noexcept
{
    return stdinPipe.Write(Span<const UINT8>((const UINT8 *)data, length));
}

Result<USIZE, Error> Shell::Read(char *buffer, USIZE capacity) noexcept
{
    USIZE totalRead = 0;

    while (totalRead < capacity - 1)
    {
        auto result = stdoutPipe.Read(Span<UINT8>((UINT8 *)(buffer + totalRead), (capacity - 1) - totalRead));
        if (!result)
        {
            if (totalRead > 0)
                return Result<USIZE, Error>::Ok(totalRead);
            return Result<USIZE, Error>::Err(result.Error());
        }
        USIZE bytesRead = result.Value();
        if (bytesRead == 0)
            break;

        bool promptFound = false;
        for (USIZE i = 0; i < bytesRead; ++i)
        {
            if (buffer[totalRead + i] == Shell::EndOfLineChar)
            {
                promptFound = true;
                break;
            }
        }
        totalRead += bytesRead;
        if (promptFound)
            break;
    }
    return Result<USIZE, Error>::Ok(totalRead);
}

Result<USIZE, Error> Shell::ReadError(char *buffer, USIZE capacity) noexcept
{
    USIZE totalRead = 0;

    while (totalRead < capacity - 1)
    {
        char byte;
        auto result = stderrPipe.Read(Span<UINT8>((UINT8 *)&byte, 1));
        if (!result)
        {
            if (totalRead > 0)
                return Result<USIZE, Error>::Ok(totalRead);
            return Result<USIZE, Error>::Err(result.Error());
        }
        if (result.Value() == 0)
            break;
        buffer[totalRead++] = byte;
        if (byte == EndOfLineChar)
            break;
    }
    return Result<USIZE, Error>::Ok(totalRead);
}

Result<Shell, Error> Shell::Create() noexcept
{
    auto stdinResult = Pipe::Create();
    auto stdoutResult = Pipe::Create();
    auto stderrResult = Pipe::Create();

    if (!stdinResult || !stdoutResult || !stderrResult)
        return Result<Shell, Error>::Err(Error::Pipe_CreateFailed);

    auto stdinPipe = static_cast<Pipe &&>(stdinResult.Value());
    auto stdoutPipe = static_cast<Pipe &&>(stdoutResult.Value());
    auto stderrPipe = static_cast<Pipe &&>(stderrResult.Value());

    const CHAR *args[] = {"cmd.exe", nullptr};
    auto processResult = Process::Create("cmd.exe", args,
        stdinPipe.ReadEnd(), stdoutPipe.WriteEnd(), stderrPipe.WriteEnd());

    if (!processResult)
        return Result<Shell, Error>::Err(Error::Process_CreateFailed);

    (void)stdinPipe.CloseRead();
    (void)stdoutPipe.CloseWrite();
    (void)stderrPipe.CloseWrite();

    return Result<Shell, Error>::Ok(
        Shell(
            static_cast<Process &&>(processResult.Value()),
            static_cast<Pipe &&>(stdinPipe),
            static_cast<Pipe &&>(stdoutPipe),
            static_cast<Pipe &&>(stderrPipe)));
}

#endif
