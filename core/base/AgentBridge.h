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

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/Macros.h"
#include "yasio/io_watcher.hpp"

namespace ax
{

class AgentRequestHandler;
class SceneAccess;

/** AgentBridge listens on a TCP port and speaks the agent bridge's newline-delimited JSON
    protocol (see docs/superpowers/specs/2026-08-08-agent-bridge-design.md, especially §3 for
    the threading model and §4 for the wire format) to a single external client, marshalling
    every request onto the Axmol main thread before handing it to AgentRequestHandler. It
    contains no protocol knowledge beyond framing - method dispatch, validation and error
    mapping all live in AgentRequestHandler.

    *** DEVELOPMENT-ONLY DEBUG FEATURE - DO NOT ENABLE IN A SHIPPED BUILD. ***
    A connected client can inspect and mutate every node in the running scene, simulate input,
    and pause/resume the Director. Compiling this in (AX_ENABLE_AGENT_BRIDGE) is harmless by
    itself - the socket only opens if the application explicitly calls listen(), exactly like
    Console - but that call must never be reachable from a shipping build.

    WHY the default bind address is "127.0.0.1", unlike Console's "0.0.0.0": this API can mutate
    a running process, so binding loopback-only is the primary mitigation (design doc §5) rather
    than an afterthought. setBindAddress() exists for deliberate device debugging (e.g. Android
    over `adb forward`, which tunnels a device-local port to the host's loopback without ever
    exposing the bridge itself to the network) and logs a prominent warning whenever the
    configured address is not loopback.

    CONCURRENCY CHOICE: one connection at a time. If a second client connects while one is
    already active, the OLD connection is dropped in favour of the NEW one ("newest connection
    wins" - see acceptClient()) rather than the new one being refused. For a development tool,
    a bridge that a stale/half-dead connection can render permanently unreachable until the game
    is restarted is a worse failure mode than disturbing whatever was on the old connection. This
    keeps per-connection state (chiefly the partial-line accumulation buffer) a single
    std::string rather than a per-fd map, at the cost of only ever serving one agent at a time -
    which matches how this is actually used (one MCP server talking to one game process).

    SHUTDOWN: nothing about this class is automatic except being torn down. Director's destructor
    calls AgentBridge::destroyInstance() before releasing anything AgentBridge's listener thread
    could still reach (see Director.cpp) - this is the ONLY code path that guarantees the
    listener thread is joined before the engine goes away; an application that never lets
    ~Director run (e.g. a process killed outright) obviously cannot benefit from it, but that is
    true of every other engine subsystem's cleanup too. */
class AX_DLL AgentBridge
{
public:
    static AgentBridge* getInstance();
    static void destroyInstance();

    /** Starts listening on `port` on its own thread. Returns false if already listening, or if
        opening/binding/listening on the socket fails (the reason is logged). A no-op on
        Emscripten, which has no listening sockets, exactly like Console::listenOnTCP.

        Never called automatically: the application must opt in explicitly. */
    bool listen(int port);

    /** Stops listening, closes any active connection, and joins the listener thread before
        returning. Safe to call when not already listening (a no-op), and safe to call more than
        once - including concurrently-ish with a listen() that hasn't finished spawning the
        thread yet, which is exactly the race a prior version of this class got wrong (see the
        WHY comment on _running in the .cpp). */
    void stop();

    /** Sets the address a subsequent listen() will bind to. Has no effect on an already-listening
        bridge - call stop() first. Logs a prominent AXLOGW warning if `address` is not a
        loopback address, per the class comment. */
    void setBindAddress(std::string_view address);

    bool isListening() const { return _running; }
    int getPort() const { return _port; }

    /** Splits `buffer` on raw '\n' bytes into `outLines`, consuming each complete line (including
        blank ones, which are silently dropped rather than dispatched as empty requests) and
        stripping one trailing '\r' from each - so a telnet or PowerShell client sending CRLF line
        endings still produces clean JSON text. Whatever is left after the last '\n' (a partial
        line, or nothing) stays in `buffer` for the next call. Returns false - buffer contents
        undefined beyond "still capped at maxLineBytes" - if the unterminated remainder exceeds
        `maxLineBytes`; true otherwise, including when outLines ends up empty.

        Pure and socket-free by design so it can be unit-tested directly (see
        AgentBridgeFramingTests.cpp) without a TCP connection, a running engine, or an AgentBridge
        instance - hence static rather than a private detail of onClientReadable(). */
    static bool extractLines(std::string& buffer, size_t maxLineBytes, std::vector<std::string>& outLines);

private:
    AgentBridge();
    ~AgentBridge();
    AX_DISALLOW_COPY_AND_ASSIGN(AgentBridge);

    // Listener-thread entry point. Never lets an exception escape - see the try/catch wrapping
    // the whole body in the .cpp - because an uncaught exception at a std::thread's entry point
    // is std::terminate, and nothing upstream of this thread can catch it.
    void loop();
    void acceptClient();
    void closeClient();

    // Defensive fallback for a readiness notification the watcher ever fails to deliver for the
    // active connection (missed close, or - in principle - missed data). Called from loop() on a
    // time basis (see the .cpp), so it runs at most a few times a second.
    void probeClientLiveness();

    // Reads one chunk from the active client and hands it, plus whatever partial line was already
    // buffered, to extractLines(). Returns false if the connection should be closed (peer
    // disconnect, socket error, or an oversize line - see the .cpp for the size cap).
    bool onClientReadable();
    bool processBufferedLines();

    // Marshals one request line onto the Axmol main thread, blocks (responsively - see the .cpp)
    // for the result with a timeout, and writes the framed response. Returns false if the
    // connection should be closed (a send failure, or the bridge is stopping) - a malformed
    // *request*, by contrast, never closes the connection: it just produces an error response
    // from AgentRequestHandler::handle() like any other request.
    bool dispatchLine(std::string_view line);

    bool sendResponseLine(std::string_view body);
    bool sendAllTo(socket_native_type fd, const void* data, size_t length);
    void drainInboundBytes(socket_native_type fd);

    yasio::io_watcher _watcher;
    socket_native_type _listenfd;
    socket_native_type _clientfd;

    std::thread _thread;

    // WHY atomic (Console's equivalent fields are plain bool): both are read from whichever
    // thread calls stop()/isListening() while the listener thread concurrently reads/writes them
    // too - see the .cpp for the specific race this closes (a stop() that could skip its own
    // join()).
    std::atomic<bool> _running;
    std::atomic<bool> _endThread;

    std::string _bindAddress;
    int _port;

    // Accumulates bytes for the connection currently in _clientfd until extractLines() can pull a
    // full line out of it. Cleared whenever a new client is accepted.
    std::string _recvBuffer;

    // Set whenever _clientfd changes or genuinely receives bytes; read by probeClientLiveness()
    // so a connection that was just heard from isn't re-probed on the very next idle tick.
    std::chrono::steady_clock::time_point _lastActivity;

    std::unique_ptr<SceneAccess> _sceneAccess;
    std::unique_ptr<AgentRequestHandler> _handler;

    // Flipped false exactly once, by ~AgentBridge() after stop() has unconditionally joined the
    // listener thread - see the .cpp for the full reasoning (next to dispatchLine() and
    // ~AgentBridge()). Never reassigned as a shared_ptr (only its pointee is ever mutated), which
    // is what makes copying it from the listener thread safe without any additional locking: nothing
    // ever concurrently writes to the _handlerAlive member itself.
    std::shared_ptr<std::atomic<bool>> _handlerAlive;
};

}  // namespace ax
