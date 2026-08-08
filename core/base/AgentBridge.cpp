/****************************************************************************
 Copyright (c) 2013-2016 Chukong Technologies Inc.
 Copyright (c) 2017-2018 Xiamen Yaji Software Co., Ltd.
 Copyright (c) 2019-present Axmol Engine contributors (see AUTHORS.md).

 https://axmol.dev/

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/

#include "base/AgentBridge.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <thread>
#include <vector>

#include "base/AgentRequestHandler.h"
#include "base/Director.h"
#include "base/Scheduler.h"
#include "base/SceneAccess.h"
#include "platform/PlatformConfig.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include "yasio/xxsocket.hpp"

namespace ax
{

namespace
{

// A scene tree for a moderately complex screen is many kilobytes of JSON (design doc §1, the
// whole reason this bridge doesn't reuse Console). 8 KB keeps the common case - one recv() per
// request line - while staying far below any sane stack/heap budget for a per-connection
// buffer; contrast with Console::parseCommand's char buf[512] read ONE BYTE AT A TIME, which is
// fine for short human-typed commands but would make a many-kilobyte request line pay for a
// recv() syscall per byte.
constexpr size_t kRecvChunkBytes = 8192;

// Caps how large _recvBuffer is allowed to grow while still waiting for a '\n'. Without this, a
// client that never sends a newline (malicious or just broken) turns the accumulation buffer
// into an unbounded memory-exhaustion vector - see design doc §5, "malformed JSON, wrong types,
// absurd depths and unknown methods must all produce error responses, never a crash". A real
// scene.tree response can be large, but not anywhere near this large.
//
// NOTE the cap is enforced by extractLines() AFTER onClientReadable() has already appended the
// latest recv() chunk, so the buffer can transiently reach kMaxRequestLineBytes +
// kRecvChunkBytes before this is caught, not exactly kMaxRequestLineBytes. That slack is bounded
// (at most one extra chunk) and is not itself a meaningful memory-exhaustion vector, so this is
// documented rather than "fixed" by pre-checking before every append.
constexpr size_t kMaxRequestLineBytes = 1024 * 1024;

// How long dispatchLine() is willing to wait in total for the marshalled call to complete on the
// Axmol main thread before giving up and answering engine_timeout (design doc §3): "a stalled or
// paused game must not wedge the agent [forever]".
constexpr std::chrono::seconds kEngineCallTimeout{5};

// The wait for that 5 seconds happens in small increments rather than one blocking call, so a
// shutdown in progress (_endThread) can be noticed and abandoned promptly - see dispatchLine().
constexpr std::chrono::milliseconds kDispatchPollInterval{100};

// Total budget for sendAllTo() to finish flushing one response before giving up on the
// connection - see its WHY comment for the blocking-socket hazard this replaces.
constexpr std::chrono::seconds kSendDeadline{10};
constexpr std::chrono::milliseconds kSendRetryInterval{20};

// Hard ceilings for drainInboundBytes() - see its WHY comment. A client that streams
// continuously must not be able to keep this loop (and, transitively, stop()'s join()) running
// forever.
constexpr size_t kDrainByteBudget = 1024 * 1024;
constexpr int kDrainIterationCap  = 4096;

// How often probeClientLiveness() actually does any work, throttled off of _lastActivity - see
// its WHY comment.
constexpr std::chrono::milliseconds kLivenessProbeInterval{900};

AgentBridge* sSharedAgentBridge = nullptr;

// "localhost" is deliberately NOT in this set: yasio's ip::endpoint parses IP literals only (see
// listen()'s explicit endpoint-validity check) and does not resolve hostnames, so treating
// "localhost" as loopback here would have suppressed the non-loopback warning for an address that
// was actually just going to fail to bind, silently. Rejecting it with a clear error at listen()
// time is the fix; this function no longer needs to know about it.
bool isLoopbackAddress(std::string_view address)
{
    return address == "127.0.0.1" || address == "::1";
}

// Builds one of this class's own protocol-level error responses (as opposed to the ones
// AgentRequestHandler::handle() itself produces for a well-formed-but-invalid *request*). These
// all happen before or around handle() ever running - an oversize line, a stalled engine - so
// there is no parsed "id" to echo; JSON null is what
// AgentRequestHandler uses for the same situation (see its handle(), which defaults idValue to
// null before it ever finds an "id" to copy). Kept in the same compact, key-ordered shape
// (id, ok, error{code,message}) that AgentRequestHandler::buildError produces, via rapidjson for
// the same reason it uses rapidjson: no hand-rolled escaping of `message`.
std::string buildBridgeError(std::string_view code, std::string_view message)
{
    rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();

    doc.AddMember("id", rapidjson::Value(rapidjson::kNullType), allocator);
    doc.AddMember("ok", false, allocator);

    rapidjson::Value errorObj(rapidjson::kObjectType);
    errorObj.AddMember(
        "code", rapidjson::Value(code.data(), static_cast<rapidjson::SizeType>(code.size()), allocator), allocator);
    errorObj.AddMember(
        "message", rapidjson::Value(message.data(), static_cast<rapidjson::SizeType>(message.size()), allocator),
        allocator);
    doc.AddMember("error", errorObj, allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return std::string(buffer.GetString(), buffer.GetLength());
}

// Returns true and sets `outPending` to the number of bytes currently sitting in `fd`'s receive
// buffer, ready to read without blocking. Returns false only for a genuine ioctl failure (bad
// fd, etc.) - a healthy idle connection reports outPending == 0 too, so callers that mean to use
// this as a liveness signal must combine it with a readiness check (see the class comment on
// probeClientLiveness()); a bare zero on its own only ever means "no data waiting."
bool queryPendingBytes(socket_native_type fd, long& outPending)
{
#if defined(_WIN32)
    u_long pending = 0;
    if (ioctlsocket(fd, FIONREAD, &pending) != 0)
        return false;
#else
    int pending = 0;
    if (ioctl(fd, FIONREAD, &pending) < 0)
        return false;
#endif
    outPending = static_cast<long>(pending);
    return true;
}

}  // namespace

AgentBridge* AgentBridge::getInstance()
{
    if (!sSharedAgentBridge)
        sSharedAgentBridge = new AgentBridge();
    return sSharedAgentBridge;
}

void AgentBridge::destroyInstance()
{
    delete sSharedAgentBridge;
    sSharedAgentBridge = nullptr;
}

AgentBridge::AgentBridge()
    : _listenfd(-1)
    , _clientfd(-1)
    , _running(false)
    , _endThread(false)
    , _bindAddress("127.0.0.1")
    , _port(-1)
    , _lastActivity(std::chrono::steady_clock::now())
    , _sceneAccess(std::make_unique<DirectorSceneAccess>())
    , _handler(std::make_unique<AgentRequestHandler>(_sceneAccess.get()))
    , _handlerAlive(std::make_shared<std::atomic<bool>>(true))
{}

AgentBridge::~AgentBridge()
{
    // stop() unconditionally joins the listener thread (see its WHY comment) before this
    // destructor body finishes, so by the time we reach the line below, nothing can still be
    // running dispatchLine() on the listener thread. Only AFTER that is it safe to flip
    // _handlerAlive false and let the member destructors below (which destroy _handler and
    // _sceneAccess) proceed: any request-handling lambda already marshalled onto the Axmol main
    // thread before this point, but not yet executed, will see *alive == false and skip touching
    // `handler` entirely - see dispatchLine()'s capture comment. _handlerAlive's own shared_ptr
    // member is never reassigned anywhere in this class (only its pointee, here and in that
    // lambda), which is what makes this store race-free with the listener thread's copy of it in
    // dispatchLine() without any additional lock: two threads may freely copy/read the same
    // never-reassigned shared_ptr concurrently.
    stop();
    *_handlerAlive = false;
}

bool AgentBridge::listen(int port)
{
#ifndef __EMSCRIPTEN__
    if (_running || _thread.joinable())
    {
        AXLOGW("AgentBridge: already listening (or stop() hasn't finished). Call stop() first");
        return false;
    }

    using namespace yasio::inet;

    // Same construction sequence as Console::listenOnTCP: an xxsocket wraps the raw handle only
    // long enough to open/bind/listen, then release_handle() hands the fd to this class to own
    // and manage with plain socket calls from here on (see loop()/closeClient()). Unlike
    // Console, there is no traverse_local_address()-driven "0.0.0.0"/"::" fallback for an empty
    // bind address, because _bindAddress is never empty - it defaults to the loopback address
    // (see the class comment) rather than "auto-detect every interface".
    ip::endpoint ep{_bindAddress.c_str(), static_cast<u_short>(port)};
    if (!ep)
    {
        // ip::endpoint parses IP literals only - it never resolves hostnames. A bind address
        // like "localhost" would silently produce an unspecified endpoint here and go on to fail
        // deep inside pserve() with a generic errno, which is confusing and (before this check
        // existed) could slip past setBindAddress()'s loopback check entirely. Fail clearly,
        // right here, instead.
        AXLOGW(
            "AgentBridge: bind address '{}' is not a literal IPv4/IPv6 address (hostnames such "
            "as 'localhost' are not resolved) - use '127.0.0.1' or '::1'",
            _bindAddress);
        return false;
    }

    xxsocket sock;
    if (sock.pserve(ep) != 0)
    {
        const int ec = xxsocket::get_last_errno();
        AXLOGW("AgentBridge: open server failed, ec:{}, {}", ec, xxsocket::strerror(ec));
        return false;
    }

    AXLOGI("AgentBridge: listening on {}", ep.to_string());

    _listenfd = sock.release_handle();
    _port     = port;

    // Set here, synchronously on the CALLING thread, before the listener thread is even spawned -
    // deliberately not inside loop() (where a previous version of this class set it). Setting it
    // inside loop() left a window, right after std::thread construction below, during which
    // _running was still false even though _thread was already joinable: a stop() landing in that
    // window would see `if (_running)` as false and skip the join() entirely, so ~AgentBridge
    // could go on to destroy a still-joinable std::thread - std::terminate. See also stop(),
    // which no longer gates its join on _running at all, for the other half of this fix.
    _running = true;
    _thread  = std::thread(&AgentBridge::loop, this);
    return true;
#else
    (void)port;
    return false;
#endif
}

void AgentBridge::stop()
{
    _endThread = true;
    _watcher.wakeup();

    // Unconditional, NOT gated on _running - see listen()'s WHY comment for the exact race that
    // made a gated join unsafe. std::thread::joinable() is thread-safe to query on its own object
    // from the thread that owns it (there is exactly one such owner here) and is true from
    // construction until join()/detach(), independent of whether the underlying thread function
    // has gotten around to running any of loop()'s body yet - so this is the race-free way to ask
    // "do I need to join before this AgentBridge (and its std::thread member) can be destroyed or
    // reused for a future listen()".
    if (_thread.joinable())
        _thread.join();

    _endThread = false;
    _port      = -1;
}

void AgentBridge::setBindAddress(std::string_view address)
{
    _bindAddress = address;

    if (!isLoopbackAddress(_bindAddress))
    {
        AXLOGW(
            "AgentBridge: bind address '{}' is NOT loopback. This debug bridge grants full "
            "inspection and mutation of the running scene to whatever connects to it - it will "
            "now be reachable from the network. This must never happen in a build reachable by "
            "an untrusted network.",
            _bindAddress);
    }
}

bool AgentBridge::extractLines(std::string& buffer, size_t maxLineBytes, std::vector<std::string>& outLines)
{
    for (;;)
    {
        const auto pos = buffer.find('\n');
        if (pos == std::string::npos)
            break;

        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);

        // The protocol is newline-delimited JSON, not text with a line-ending convention, but a
        // client that sends CRLF anyway (a telnet or PowerShell client will) shouldn't be
        // punished for it - and must not have that '\r' end up inside the JSON text handed to
        // AgentRequestHandler::handle().
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // A JSON string's own embedded newline is escaped as the two characters '\' 'n', which
        // contains no raw 0x0A byte at all - so it can never cause this raw-byte split to fire
        // early inside a string literal; nothing special has to be done for that case here.

        // Blank lines (bare "\n", or "\r\n") are skipped rather than dispatched as an empty
        // request - there is nothing useful to send AgentRequestHandler::handle() for one, and a
        // client's keep-alive newline or copy-paste artifact shouldn't cost a round trip.
        if (!line.empty())
            outLines.push_back(std::move(line));
    }

    return buffer.size() <= maxLineBytes;
}

void AgentBridge::loop()
{
    using yasio::inet::xxsocket;

    // Register READ ONLY, never socket_event::error. yasio's poll_io_watcher maps
    // socket_event::error onto POLLERR in the pollfd *events* (request) field, but POLLERR,
    // POLLHUP and POLLNVAL are output-only flags: WSAPoll rejects them in events and fails the
    // entire call, so poll_io() returns -1 on every iteration and this loop never accepts or
    // reads anything. (The failure is invisible from the client side, because the OS still
    // completes the TCP handshake from the listen backlog without this process ever calling
    // accept() - connections appear to succeed and then simply never get a reply.)
    // Checking is_ready(fd, socket_event::error) below is still correct and necessary: poll
    // reports those conditions in revents whether or not they were requested.
    _watcher.mod_event(_listenfd, yasio::socket_event::read, 0);

    // Short, fixed poll timeout - unlike Console::loop()'s 300-second fallback, which exists
    // purely so _endThread eventually gets rechecked if a wakeup() were ever missed. This being
    // short is what keeps probeClientLiveness() (called unconditionally at the bottom of every
    // iteration, throttled internally - see its WHY comment) actually reachable at a useful
    // cadence even when nready is never 0: a client fd stuck reporting POLLHUP/POLLERR every
    // single poll_io() call keeps nready > 0 forever, which would otherwise starve any
    // liveness-probing logic gated solely on "nready == 0" and spin this thread at 100% CPU.
    const int64_t timeout_usec = 1 * std::micro::den;  // 1 second

    // The whole loop is wrapped in try/catch(...) because loop() is a std::thread entry point: an
    // exception that reaches here uncaught is std::terminate on this thread, full stop, with no
    // upstream handler anywhere that could ever catch it (CRITICAL 2). dispatchLine() and the
    // marshalled lambda it queues (IMPORTANT 7) already convert every failure they specifically
    // anticipate into a normal response instead of throwing, so landing in one of these catches at
    // all would mean something neither of those anticipated (e.g. std::bad_alloc from
    // _recvBuffer.append() or std::promise's own allocation) - logged and treated as fatal to this
    // listener thread (which the cleanup below still runs for, exactly as if _endThread had been
    // set normally), not to the whole process.
    try
    {
        while (!_endThread)
        {
            int nready = _watcher.poll_io(timeout_usec);

            if (nready == -1)
            {
                if (xxsocket::get_last_errno() != EINTR)
                    AXLOGW("AgentBridge: abnormal error in poll_io()");
            }
            else if (nready > 0 && !_endThread)
            {
                if (_watcher.is_ready(_listenfd, yasio::socket_event::read))
                {
                    acceptClient();
                    --nready;
                }

                if (nready > 0 && _clientfd != static_cast<socket_native_type>(-1))
                {
                    if (_watcher.is_ready(_clientfd, yasio::socket_event::error))
                    {
                        // POLLHUP/POLLERR/POLLNVAL on the client fd: it's dead or errored. This
                        // must be checked explicitly, not inferred from POLLIN - a fd that is
                        // ready only for an error condition would otherwise never be handled by
                        // either branch here, leaving nready > 0 forever and spinning this thread
                        // at 100% CPU.
                        closeClient();
                    }
                    else if (_watcher.is_ready(_clientfd, yasio::socket_event::read))
                    {
                        if (!onClientReadable())
                            closeClient();
                    }
                }
            }

            // Deliberately unconditional (not "only when nready == 0") - see the WHY comment
            // above this loop. probeClientLiveness() throttles its own real work via
            // _lastActivity, so calling it every iteration is cheap.
            probeClientLiveness();
        }
    }
    catch (const std::exception& e)
    {
        AXLOGE("AgentBridge: listener thread caught an unexpected exception, stopping: {}", e.what());
    }
    catch (...)
    {
        AXLOGE("AgentBridge: listener thread caught an unexpected non-standard exception, stopping");
    }

    closeClient();
    if (_listenfd != static_cast<socket_native_type>(-1))
    {
        closesocket(_listenfd);
        _listenfd = -1;
    }

    _running = false;
}

void AgentBridge::acceptClient()
{
    using yasio::inet::xxsocket;

    socket_native_type fd = ::accept(_listenfd, nullptr, nullptr);
    if (fd == static_cast<socket_native_type>(-1))
        return;

    // Accepted sockets inherit the listening socket's blocking mode, and this class opens that
    // socket via xxsocket::pserve() -> reopen() (not popen()), which never applies
    // set_nonblocking(). Left blocking, a send() that fills the peer's receive window - e.g. a
    // client that requested a large scene.tree and then simply stopped reading - would park this
    // thread inside the send() syscall itself, where _watcher.wakeup() cannot interrupt it,
    // hanging stop()'s join() on the MAIN thread indefinitely. Making the socket non-blocking
    // turns that into a prompt EWOULDBLOCK instead; sendAllTo() retries with its own bounded
    // deadline rather than trusting the OS to ever make the socket writable again.
    xxsocket::set_nonblocking(fd, true);

#if AX_TARGET_PLATFORM == AX_PLATFORM_IOS
    // See Console::addClient's identical guard: without this, writing to a socket the peer has
    // already closed raises SIGPIPE, whose default action terminates the process.
    int set = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&set, sizeof(int));
#endif

    if (_clientfd != static_cast<socket_native_type>(-1))
    {
        // Newest connection wins (see class comment). Refusing the new connection instead was
        // tried and rejected: a peer that disconnects without this process observing that
        // promptly would otherwise leave _clientfd permanently occupied by a dead connection,
        // making the bridge unreachable until the whole game process is restarted - far worse,
        // for a development tool, than dropping whatever the previous client was doing.
        AXLOGW(
            "AgentBridge: a new connection arrived while one was already active; dropping the "
            "previous connection in its favour");
        closeClient();
    }

    _clientfd     = fd;
    _recvBuffer.clear();
    _lastActivity = std::chrono::steady_clock::now();
    // READ ONLY - see loop()'s comment: requesting socket_event::error would put POLLERR into the
    // pollfd events field, which WSAPoll rejects, breaking every poll. Error and hangup arrive in
    // revents regardless, and loop() checks for them there.
    _watcher.mod_event(_clientfd, yasio::socket_event::read, 0);
}

void AgentBridge::closeClient()
{
    if (_clientfd == static_cast<socket_native_type>(-1))
        return;

    _watcher.mod_event(_clientfd, 0, yasio::socket_event::read | yasio::socket_event::error);

    // WHY drain-then-shutdown rather than a bare closesocket(): closing a socket that still has
    // unread bytes sitting in its receive buffer makes the OS discard them non-gracefully - on
    // Windows in particular, that means an RST instead of a FIN, and an RST also discards
    // whatever this side just wrote that the peer hasn't read yet (TCP has no way to say "my
    // send is fine, only my receive is being abandoned" once a hard reset happens). That is
    // exactly the situation "newest connection wins" (acceptClient()) creates routinely: the
    // displaced connection's own still-unread request bytes are sitting right there. Draining
    // them first, then half-closing only the send direction with shutdown(SD_SEND) - a graceful
    // "nothing more is coming" - lets a response already written reach the peer as a clean EOF
    // instead of a connection-reset error.
    drainInboundBytes(_clientfd);
    ::shutdown(_clientfd, SD_SEND);
    closesocket(_clientfd);
    _clientfd = -1;
    _recvBuffer.clear();
}

void AgentBridge::drainInboundBytes(socket_native_type fd)
{
    using yasio::inet::xxsocket;

    // Bounded on three independent axes so this can never hang the listener thread - and, via
    // stop()'s join(), the MAIN thread: FIONREAD alone (the original version of this loop) exits
    // only once the peer stops sending, so a client that streams continuously kept it spinning
    // forever. kDrainByteBudget/kDrainIterationCap are both generous for the actual use case
    // (draining a few stray in-flight request bytes from a displaced connection) while still
    // being hard, finite ceilings; _endThread additionally lets a shutdown in progress abandon a
    // slow drain immediately rather than paying up to the full budget.
    size_t totalDrained = 0;
    char discard[512];
    for (int iteration = 0; iteration < kDrainIterationCap; ++iteration)
    {
        if (_endThread || totalDrained >= kDrainByteBudget)
            return;

        long pending = 0;
        if (!queryPendingBytes(fd, pending) || pending <= 0)
            return;

        const long remainingBudget = static_cast<long>(kDrainByteBudget - totalDrained);
        const int toRead =
            static_cast<int>((std::min)({static_cast<long>(sizeof(discard)), pending, remainingBudget}));
        if (toRead <= 0)
            return;

        const int n = xxsocket::recv(fd, discard, toRead, 0);
        if (n <= 0)
            return;
        totalDrained += static_cast<size_t>(n);
    }
}

bool AgentBridge::onClientReadable()
{
    using yasio::inet::xxsocket;

    // Console.cpp:584-601 learned (bug #18620) not to trust "the watcher says this fd is
    // readable" as proof there is data to read: a peer that goes away also makes the fd
    // readable. Console's fix, adopted verbatim here: readable + zero bytes actually pending
    // means closed, checked with the same ioctl before ever calling recv().
    long pending = 0;
    if (!queryPendingBytes(_clientfd, pending))
    {
        AXLOGW("AgentBridge: abnormal error in ioctl()/ioctlsocket()");
        return false;
    }
    if (pending == 0)
        return false;  // Readable with nothing pending: the peer is gone (fix #18620).

    char chunk[kRecvChunkBytes];
    const int n = xxsocket::recv(_clientfd, chunk, static_cast<int>(sizeof(chunk)), 0);
    if (n == 0)
        return false;  // Peer disconnected mid-request or between requests - either way, done.
    if (n < 0)
    {
        const int ec = xxsocket::get_last_errno();
        if (xxsocket::not_recv_error(ec))
            return true;  // EWOULDBLOCK/EAGAIN/EINTR: try again on the next ready notification.

        AXLOGW("AgentBridge: recv failed, ec:{}, {}", ec, xxsocket::strerror(ec));
        return false;
    }

    _recvBuffer.append(chunk, static_cast<size_t>(n));
    _lastActivity = std::chrono::steady_clock::now();
    return processBufferedLines();
}

void AgentBridge::probeClientLiveness()
{
    if (_clientfd == static_cast<socket_native_type>(-1))
        return;

    using yasio::inet::xxsocket;

    // Throttle to roughly once per idle second rather than re-probing a connection that was heard
    // from moments ago, or on every single loop() iteration now that this is called
    // unconditionally (see loop()'s WHY comment on IMPORTANT 10) - there is nothing to find that
    // onClientReadable() wouldn't already have caught.
    const auto now = std::chrono::steady_clock::now();
    if (now - _lastActivity < kLivenessProbeInterval)
        return;

    // A healthy, merely-idle connection is NOT readable and reports 0 bytes pending - the same
    // "0 pending" value a dead one reports. The only way to tell them apart (and the reason
    // onClientReadable()'s check above is gated on the watcher already having said "readable")
    // is to check readability ourselves first: handle_read_ready() is a zero-timeout select() on
    // just this fd, independent of _watcher, so it still catches a close/data event _watcher
    // missed - which is the whole point of this defensive fallback.
    if (xxsocket::handle_read_ready(_clientfd, std::chrono::microseconds(0)) <= 0)
        return;  // Not readable: connection is fine, just quiet.

    long pending = 0;
    if (!queryPendingBytes(_clientfd, pending) || pending == 0)
    {
        AXLOGW("AgentBridge: idle-connection liveness probe found the peer gone; closing");
        closeClient();
        return;
    }

    // Readable with real bytes pending and _watcher never told loop() so: whatever event this
    // was got missed. Re-arm the watcher so the next poll_io() picks it up the normal way, rather
    // than leaving those bytes to sit until the next probe interval. READ ONLY - see loop().
    _watcher.mod_event(_clientfd, yasio::socket_event::read, 0);
}

bool AgentBridge::processBufferedLines()
{
    std::vector<std::string> lines;
    if (!extractLines(_recvBuffer, kMaxRequestLineBytes, lines))
    {
        // See kMaxRequestLineBytes's comment for the exact bound. Reporting this as a normal
        // framed response, rather than dropping the connection silently, gives a well-behaved
        // client a chance to notice why it was disconnected; the connection is still closed
        // afterwards, since whatever comes after the unterminated prefix already sent can no
        // longer be framed correctly.
        sendResponseLine(buildBridgeError("invalid_request", "request line exceeds the maximum size"));
        return false;
    }

    for (auto& line : lines)
    {
        // Checked before EVERY line, not just once per recv() chunk: a single 8 KB chunk can
        // hold roughly 270 short pipelined requests, and without this a shutdown in progress
        // would otherwise pay up to the full 5-second engine_timeout wait per remaining line -
        // multiple minutes of the MAIN thread blocked inside stop()'s join(). Abandoning
        // immediately means the connection is simply dropped rather than answered, which is
        // correct: nothing is listening for a coherent response mid-shutdown anyway.
        if (_endThread)
            return false;

        if (!dispatchLine(line))
            return false;
    }

    return true;
}

bool AgentBridge::dispatchLine(std::string_view line)
{
    if (_endThread)
        return false;  // See processBufferedLines()'s identical check - also guards the
                        // Director::getInstance() call just below from ever running while this
                        // bridge is stopping.

    auto promise                    = std::make_shared<std::promise<std::string>>();
    std::future<std::string> future = promise->get_future();

    // NodeReflection, every Node, and Director may only be touched on the Axmol main thread
    // (design doc §3) - but this whole function runs on the listener thread. So the actual call
    // into AgentRequestHandler is marshalled onto the main thread via runOnAxmolThread(), and
    // this thread waits (responsively - see below) on `future` for the result.
    //
    // What's captured by value and why: `requestLine` and `promise` need no explanation (the
    // lambda must own its own copy of the request text, and must be able to signal back). `alive`
    // and `handler` are the pair that makes this safe even if AgentBridge is torn down while this
    // lambda is still queued - see the long comment on _handlerAlive in ~AgentBridge(). Capturing
    // the raw `AgentRequestHandler*` (rather than `this`) means the lambda never touches
    // AgentBridge itself, only the two things it actually needs.
    std::string requestLine(line);
    std::shared_ptr<std::atomic<bool>> alive = _handlerAlive;
    AgentRequestHandler* handler             = _handler.get();
    Director::getInstance()->getScheduler()->runOnAxmolThread(
        [handler, alive, requestLine = std::move(requestLine), promise]() {
            // Runs on the AXMOL MAIN THREAD. An exception escaping this lambda would propagate
            // out of Scheduler::update() and unwind the main loop itself, taking down every other
            // action still queued behind it - handle() is documented never to throw, but
            // "documented" isn't "enforced": a std::bad_alloc from a multi-megabyte scene.tree
            // result, or a future PropertyProvider that isn't as careful, would otherwise be
            // catastrophic here in a way it would never be for a plain failed request. Converting
            // it into a normal internal_error response keeps that failure contained to this one
            // request.
            try
            {
                if (*alive)
                    promise->set_value(handler->handle(requestLine));
                else
                    // AgentBridge has stopped since this was queued (see ~AgentBridge()); nobody
                    // is waiting on `future` any more either way, so this value is never
                    // observed - this branch exists purely to avoid touching `handler`.
                    promise->set_value(std::string());
            }
            catch (const std::exception& e)
            {
                promise->set_value(buildBridgeError("internal_error", std::string("handler threw: ") + e.what()));
            }
            catch (...)
            {
                promise->set_value(buildBridgeError("internal_error", "handler threw a non-standard exception"));
            }
        });

    // Waited for in kDispatchPollInterval increments rather than one blocking wait_for(5s): this
    // is what lets the _endThread check below notice a shutdown in progress and abandon a
    // still-in-flight request within roughly one increment instead of up to the full 5 seconds -
    // see processBufferedLines()'s identical concern.
    const auto deadline = std::chrono::steady_clock::now() + kEngineCallTimeout;
    std::future_status status;
    for (;;)
    {
        status = future.wait_for(kDispatchPollInterval);
        if (status == std::future_status::ready)
            break;
        if (_endThread)
            return false;  // Shutting down: abandon rather than wait out the rest of the timeout.
        if (std::chrono::steady_clock::now() >= deadline)
            break;
    }

    if (status != std::future_status::ready)
    {
        // A stalled or paused game must never wedge the client forever (design doc §3). The
        // marshalled lambda above may still be queued, or mid-flight, on the main thread; letting
        // it run to completion later is harmless (see the capture comment above) - its result
        // just goes unread, because this connection has already moved on.
        return sendResponseLine(buildBridgeError("engine_timeout", "the engine did not respond within 5 seconds"));
    }

    // future.get() rethrows whatever the shared state completed with. The lambda above never lets
    // an exception escape into set_value(), so the only way get() can throw here is a BROKEN
    // PROMISE: the promise (and the lambda that owned it) was destroyed without ever calling
    // set_value(), which is exactly what happens if the Scheduler is torn down (Director
    // shutting down) with this action still queued. That must not crash the listener thread.
    try
    {
        return sendResponseLine(future.get());
    }
    catch (const std::future_error&)
    {
        return sendResponseLine(
            buildBridgeError("internal_error", "the engine shut down before this request completed"));
    }
}

bool AgentBridge::sendResponseLine(std::string_view body)
{
    std::string framed;
    framed.reserve(body.size() + 1);
    framed.append(body);
    framed.push_back('\n');
    return sendAllTo(_clientfd, framed.data(), framed.size());
}

bool AgentBridge::sendAllTo(socket_native_type fd, const void* data, size_t length)
{
    using yasio::inet::xxsocket;

    // WHY a deadline loop rather than "keep calling send() until it's all gone": the accepted
    // socket is non-blocking (see acceptClient()), so a peer that stops reading (or a huge
    // scene.tree response against a slow connection) makes send() return EWOULDBLOCK instead of
    // blocking the syscall itself - but retrying that in a bare `continue` (the previous version
    // of this function) busy-spins the CPU and never gives up, which is just as capable of
    // hanging stop()'s join() as a blocking socket was. Sleeping briefly between retries and
    // abandoning the connection past a fixed total deadline bounds both.
    const auto deadline = std::chrono::steady_clock::now() + kSendDeadline;
    const char* p       = static_cast<const char*>(data);
    size_t sent         = 0;
    while (sent < length)
    {
        if (_endThread)
            return false;  // Shutting down: don't keep trying to flush to a stalled peer.

        const int n = xxsocket::send(fd, p + sent, static_cast<int>(length - sent), 0);
        if (n > 0)
        {
            sent += static_cast<size_t>(n);
            continue;
        }

        const int ec = xxsocket::get_last_errno();
        if (xxsocket::not_send_error(ec))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                AXLOGW("AgentBridge: send() deadline exceeded; abandoning connection");
                return false;
            }
            std::this_thread::sleep_for(kSendRetryInterval);
            continue;
        }

        AXLOGW("AgentBridge: send failed, ec:{}, {}", ec, xxsocket::strerror(ec));
        return false;
    }
    return true;
}

}  // namespace ax
