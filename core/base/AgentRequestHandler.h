/****************************************************************************
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

#include <string>
#include <string_view>

#include "base/Macros.h"

namespace ax
{

class SceneAccess;

/** Pure request/response dispatcher for the agent bridge's JSON protocol (see
    docs/superpowers/specs/2026-08-08-agent-bridge-design.md §4 for the wire format, the method
    table and the closed error-code set). Owns method dispatch, parameter validation and error
    mapping, and touches the running engine only through NodeReflection and an injected
    SceneAccess - so it has no socket, no thread and no GPU dependency of its own, and can be
    exercised headlessly with a fake SceneAccess (see AgentRequestHandlerTests.cpp).

    AgentBridge (a separate component, not implemented here) owns the TCP listener, the
    newline-delimited framing, and marshalling each request onto the Axmol main thread before
    calling handle(); this class assumes that has already happened and never touches a thread or
    a socket itself. */
class AX_DLL AgentRequestHandler
{
public:
    /** `access` is not owned by this handler and must outlive it. */
    explicit AgentRequestHandler(SceneAccess* access);

    /** Takes one request JSON line and returns one response JSON line, with no trailing
        newline. NEVER throws: malformed input, missing parameters, an engine that refuses a
        write, anything at all - every failure becomes a well-formed
        `{"id":...,"ok":false,"error":{"code":...,"message":...}}` response rather than an
        exception or a crash. `id` is echoed back exactly as received (any JSON scalar), or JSON
        null if the request had none. */
    std::string handle(std::string_view requestJson);

private:
    SceneAccess* _access;
};

}  // namespace ax
