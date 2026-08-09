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

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include "base/Macros.h"
#include "base/ResourcePath.h"
#include "base/Vector.h"
#include "math/Vec2.h"

namespace ax
{

class Node;

// NOTE: pathEscapesWritableSandbox now lives in ResourcePath.h (included above) - an UNGATED
// header, because NodeFactory needs the same rule for asset paths read out of untrusted scene
// files and is compiled into every build. Including it here keeps existing callers working.

/** Everything AgentRequestHandler needs from a live engine, abstracted behind an interface so
    the handler can be unit-tested with no Director, no window and no GPU. AgentBridge (a
    separate component, not implemented here) is responsible for calling every method of the
    real implementation on the Axmol main thread - see NodeReflection's threading note, which
    applies here for the same reason: these methods ultimately touch live Node objects. */
class AX_DLL SceneAccess
{
public:
    virtual ~SceneAccess() = default;

    /** The scene currently being run by the Director, or nullptr if none. Returned as a bare
        Node* - rooting a NodeReflection traversal - rather than a Scene*, since every consumer
        only ever needs it as the root of the tree. */
    virtual Node* getRunningScene() = 0;

    /** Captures the current frame to `file` inside the writable path. `file` empty means use a
        default name. On success returns true and sets `outPath` to the absolute path written;
        on failure returns false and sets `outError` to a human-readable reason. Path-escape
        rejection (absolute paths, "..") is validated by AgentRequestHandler before this is ever
        called, but implementations are expected to enforce it too - see DirectorSceneAccess. */
    virtual bool captureScreenshot(std::string_view file, std::string& outPath, std::string& outError) = 0;

    /** Synthesizes a tap (touch-down immediately followed by touch-up) at the given point, in
        the same coordinate space Console's `touch tap` uses. */
    virtual void injectTap(float x, float y) = 0;

    /** Synthesizes a touch-down at (x1,y1), a series of touch-move events tracing a straight
        line, and a touch-up at (x2,y2) - the same shape Console's `touch swipe` produces. */
    virtual void injectSwipe(float x1, float y1, float x2, float y2) = 0;

    /** Freezes or resumes the running scene's actions and scheduled updates, so it can be
        inspected or screenshotted without changing underneath the caller.

        NOT Director::pause(). That would be a trap: Director::pause() sets _paused, and
        Director::drawScene only calls Scheduler::update() when !_paused - while
        Scheduler::update() is the ONLY thing that drains the queue runOnAxmolThread posts to.
        Since AgentBridge marshals every request through that queue, pausing the Director stops
        the bridge from ever executing another request, including the resume that would undo it.
        Freezing the scene's targets instead leaves the pump running. */
    virtual void setPaused(bool paused) = 0;

    /** Whether the scene is currently frozen by setPaused(). */
    virtual bool isPaused() const = 0;

    /** The engine version string, e.g. what Console's `version` command prints. */
    virtual std::string getEngineVersion() const = 0;

    /** The design resolution size, in points. */
    virtual Vec2 getDesignResolution() const = 0;

    /** The window/frame size, in points. */
    virtual Vec2 getFrameSize() const = 0;

    /** The Director's current frame rate. */
    virtual float getFrameRate() const = 0;

    /** Writes `contents` to `file` inside the writable path, creating or truncating it. Same
        sandbox rule as captureScreenshot: `file` is validated by AgentRequestHandler before this
        is called, and implementations enforce it again. On success sets `outPath` to the absolute
        path written.

        The engine deliberately cannot write anywhere else. Making a saved scene land in the
        PROJECT is the host-side tooling's job (it copies the file out), not the engine's - a
        bridge reachable from a socket that could write anywhere on disk would be a materially
        different security proposition. */
    virtual bool writeTextFile(std::string_view file, std::string_view contents, std::string& outPath,
                               std::string& outError) = 0;

    /** Reads `file` through the engine's resource resolution, so a name like
        "scenes/level1.scene.json" is found on the search path (i.e. in Content/) exactly as any
        other asset would be. Returns false with `outError` set if it does not exist. */
    virtual bool readTextFile(std::string_view file, std::string& outContents, std::string& outError) = 0;
};

/** Real implementation of SceneAccess, wrapping ax::Director. Uses utils::captureScreen (see
    core/base/Utils.h) for screenshots and RenderView's synthetic-touch entry points for input
    injection, the same primitives Console.cpp's `touch tap`/`touch swipe` commands use.

    This is a debug-only seam into a live engine: every method here must be called on the Axmol
    main thread (see the class comment on SceneAccess), and it must never be reachable from a
    shipped build - see the design doc §5 for the full security rationale. */
class AX_DLL DirectorSceneAccess : public SceneAccess
{
public:
    /** Resumes the scene if this object paused it. Being destroyed while holding the game frozen
        would leave it frozen for good - the thing that offered director.resume is gone. */
    ~DirectorSceneAccess() override;

    Node* getRunningScene() override;
    bool captureScreenshot(std::string_view file, std::string& outPath, std::string& outError) override;
    void injectTap(float x, float y) override;
    void injectSwipe(float x1, float y1, float x2, float y2) override;
    void setPaused(bool paused) override;
    bool isPaused() const override;
    bool writeTextFile(std::string_view file, std::string_view contents, std::string& outPath,
                       std::string& outError) override;
    bool readTextFile(std::string_view file, std::string& outContents, std::string& outError) override;
    std::string getEngineVersion() const override;
    Vec2 getDesignResolution() const override;
    Vec2 getFrameSize() const override;
    float getFrameRate() const override;

private:
    // True while a previously-started capture's utils::captureScreen callback hasn't fired yet.
    // utils::captureScreen silently no-ops (with only a log warning, no callback) a second
    // request made while one is already pending, which would otherwise make captureScreenshot()
    // report success for a file that is never actually written - see captureScreenshot()'s WHY
    // comment. A shared_ptr<atomic<bool>> rather than a plain bool so the completion callback -
    // which utils::captureScreen fires later, asynchronously - can capture the flag BY VALUE and
    // safely outlive this DirectorSceneAccess if it is ever destroyed before the callback runs,
    // instead of capturing `this` and risking a dangling-pointer callback (the same hazard, and
    // the same fix, as AgentBridge's _handlerAlive).
    std::shared_ptr<std::atomic<bool>> _captureInProgress = std::make_shared<std::atomic<bool>>(false);

    // What setPaused(true) froze, so setPaused(false) can resume exactly that and nothing else.
    // Scheduler targets only - the ActionManager is itself one of them, so actions come along for
    // free. Holding the Vector<Node*> that ActionManager::pauseAllRunningActions() returns would
    // RETAIN every node with a running action for the duration of the pause; see setPaused().
    bool _paused = false;
    std::set<void*> _pausedTargets;
};

}  // namespace ax
