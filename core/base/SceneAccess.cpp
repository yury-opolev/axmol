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

#include "base/SceneAccess.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>

#include "2d/Scene.h"
#include "base/Director.h"
#include "base/Logging.h"
#include "base/Utils.h"
#include "platform/FileUtils.h"
#include "platform/RenderView.h"

namespace ax
{

// Declared the same way Console.cpp declares it, to avoid pulling in the whole axmol.h umbrella
// header just for a version string.
extern const char* axmolVersion(void);

bool pathEscapesWritableSandbox(std::string_view path)
{
    if (path.empty())
        return false;
    if (path.find("..") != std::string_view::npos)
        return true;
    if (path.front() == '/' || path.front() == '\\')
        return true;
    return path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
}

namespace
{

// Every synthetic touch needs an id unique enough that two touches in quick succession are never
// confused for the same gesture. injectTap/injectSwipe used to reseed with std::srand(time(0))
// and draw from std::rand() on every call - reseeding the process-wide RNG (which the running
// game may itself depend on for gameplay) as a side effect of a debug input-injection call, and
// producing IDENTICAL ids for any two calls within the same wall-clock second since time(nullptr)
// only has one-second resolution. A private, monotonically-increasing counter is simpler, has no
// effect on anything else in the process, and is still guaranteed unique per call.
intptr_t nextTouchId()
{
    static std::atomic<intptr_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

// Hard ceiling on the number of synthetic touch-move events a single injectSwipe() call can
// generate, regardless of how far apart (x1,y1) and (x2,y2) are. See injectSwipe()'s WHY comment:
// this is what turns an absurd or non-finite client-supplied coordinate into a bounded amount of
// work instead of an effectively (or literally) infinite one.
constexpr int kMaxSwipeSteps = 256;

}  // namespace

Node* DirectorSceneAccess::getRunningScene()
{
    return Director::getInstance()->getRunningScene();
}

bool DirectorSceneAccess::captureScreenshot(std::string_view file, std::string& outPath, std::string& outError)
{
    if (pathEscapesWritableSandbox(file))
    {
        outError = "file escapes the writable path";
        return false;
    }

    // WHY: utils::captureScreen silently ignores a second request while one is already pending -
    // it logs a warning and never invokes that second call's callback at all. Without this check,
    // captureScreenshot() would still return true with a path that will never be written, and the
    // client would have no way to know its request was simply dropped.
    if (_captureInProgress->exchange(true))
    {
        outError = "a screenshot capture is already in progress";
        return false;
    }

    const std::string filename = file.empty() ? "agent-screenshot.png" : std::string(file);
    outPath                    = FileUtils::getInstance()->getWritablePath() + filename;

    // WHY fire-and-forget rather than blocking for the real result: utils::captureScreen defers
    // the actual pixel readback to Director::EVENT_AFTER_DRAW, which - per Director::drawScene -
    // fires strictly *after* the scheduler callback we are necessarily running inside of (see
    // Scheduler::update, which drains marshalled main-thread work before the scene is even
    // rendered this frame). Blocking here for completion would therefore deadlock the Axmol main
    // thread against itself: nothing services EVENT_AFTER_DRAW until this call returns. The
    // eventual write also hops through the JobSystem and back via runOnAxmolThread on a *later*
    // frame, so even a same-thread callback could not resolve synchronously. `outPath` is fully
    // deterministic before any of that happens, so we can report it immediately; only the actual
    // disk write trails behind by a couple of frames. A failure past this point is logged but has
    // no synchronous caller left to report to. AgentRequestHandler surfaces this by adding
    // "pending": true to the result, so a client knows to poll for the file rather than assume it
    // already exists.
    auto inProgress = _captureInProgress;  // captured by value below - see the class comment on
                                            // _captureInProgress for why this isn't `this`.
    utils::captureScreen(
        [inProgress](bool ok, std::string_view path) {
            inProgress->store(false);
            if (!ok)
                AXLOGE("SceneAccess: failed to write screenshot to {}", path);
        },
        filename);

    return true;
}

bool DirectorSceneAccess::writeTextFile(std::string_view file,
                                        std::string_view contents,
                                        std::string& outPath,
                                        std::string& outError)
{
    if (file.empty())
    {
        outError = "file name is required";
        return false;
    }
    // Defence in depth, exactly as for captureScreenshot: AgentRequestHandler validates this
    // before calling, and this enforces it again so the rule cannot be bypassed by reaching the
    // implementation directly.
    if (pathEscapesWritableSandbox(file))
    {
        outError = "file escapes the writable path";
        return false;
    }

    auto* fileUtils = FileUtils::getInstance();
    outPath         = fileUtils->getWritablePath() + std::string(file);

    // writeStringToFile does not create intermediate directories, and `file` is allowed to name a
    // subdirectory (scenes/level1.scene.json) so that saving and loading agree on one name.
    const auto slash = outPath.find_last_of("/\\");
    if (slash != std::string::npos)
        fileUtils->createDirectories(outPath.substr(0, slash));

    if (!fileUtils->writeStringToFile(std::string(contents), outPath))
    {
        outError = "could not write " + outPath;
        return false;
    }
    return true;
}

bool DirectorSceneAccess::readTextFile(std::string_view file, std::string& outContents, std::string& outError)
{
    if (file.empty())
    {
        outError = "file name is required";
        return false;
    }
    // Read through FileUtils rather than the raw filesystem so a scene is found on the resource
    // search path (i.e. in Content/) like any other asset, and so a scene saved during development
    // loads the same way in a packaged build.
    auto* fileUtils = FileUtils::getInstance();
    if (!fileUtils->isFileExist(file))
    {
        // Fall back to the writable path, where scene.save just put it - a save/load round trip
        // in a single session should not require the file to have been copied into Content first.
        const std::string writablePath = fileUtils->getWritablePath() + std::string(file);
        if (!fileUtils->isFileExist(writablePath))
        {
            outError = "no such file: " + std::string(file);
            return false;
        }
        outContents = fileUtils->getStringFromFile(writablePath);
        return true;
    }

    outContents = fileUtils->getStringFromFile(file);
    return true;
}

void DirectorSceneAccess::injectTap(float x, float y)
{
    auto* renderView = Director::getInstance()->getRenderView();
    if (!renderView)
        return;

    // Defensive: see injectSwipe()'s identical guard. A tap doesn't loop, so a non-finite
    // coordinate can't hang anything here, but there is no reason to hand Infinity/NaN down into
    // RenderView's touch handling either.
    if (!std::isfinite(x) || !std::isfinite(y))
        return;

    // Mirrors Console::commandTouchSubCommandTap: a single synthetic touch id, immediately
    // begun and ended at the same point. Unlike Console, no runOnAxmolThread wrapping is needed
    // here - callers of SceneAccess are already required to be on the Axmol main thread (see the
    // class comment in SceneAccess.h), where Console's version instead runs on the console's own
    // socket thread and must marshal across.
    intptr_t touchId = nextTouchId();
    float tempX = x, tempY = y;
    renderView->handleTouchesBegin(1, &touchId, &tempX, &tempY);
    renderView->handleTouchesEnd(1, &touchId, &tempX, &tempY);
}

void DirectorSceneAccess::injectSwipe(float x1, float y1, float x2, float y2)
{
    auto* renderView = Director::getInstance()->getRenderView();
    if (!renderView)
        return;

    // Defensive: AgentRequestHandler's getRequiredNumber() already rejects non-finite JSON
    // numbers before they reach here (see the design doc §5 "untrusted input" requirement), but
    // SceneAccess is a reusable seam other callers can reach directly, so this class enforces the
    // same rule itself rather than trusting every caller to have validated first.
    if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2))
        return;

    intptr_t touchId = nextTouchId();

    float beginX = x1, beginY = y1;
    renderView->handleTouchesBegin(1, &touchId, &beginX, &beginY);

    // Interpolate over a FIXED, hard-capped number of steps rather than the old approach of
    // decrementing a running distance by 1.0 per touch-move event until it dropped below 1: that
    // made the step count equal to the pixel distance between the two points, so a
    // client-supplied coordinate far outside any real screen turned one swipe into an enormous
    // (or, for a non-finite input, literally endless - `inf - 1 == inf`) number of synthetic
    // touch-move events pumped onto the render thread. This is the fix for exactly that: a
    // {"x1":0,"y1":0,"x2":1e39,"y2":0} swipe request permanently froze the game. kMaxSwipeSteps
    // bounds the work regardless of distance; the isfinite() guard above additionally means
    // `distance` below can never itself be non-finite by the time it's computed.
    const float dx       = std::abs(x2 - x1);
    const float dy       = std::abs(y2 - y1);
    const float distance = (std::max)(dx, dy);
    const int steps = static_cast<int>((std::min)((std::max)(distance, 1.0f), static_cast<float>(kMaxSwipeSteps)));

    for (int i = 1; i < steps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        float moveX   = x1 + (x2 - x1) * t;
        float moveY   = y1 + (y2 - y1) * t;
        renderView->handleTouchesMove(1, &touchId, &moveX, &moveY);
    }

    float endX = x2, endY = y2;
    renderView->handleTouchesEnd(1, &touchId, &endX, &endY);
}

void DirectorSceneAccess::setPaused(bool paused)
{
    auto* director = Director::getInstance();
    if (paused)
        director->pause();
    else
        director->resume();
}

bool DirectorSceneAccess::isPaused() const
{
    return Director::getInstance()->isPaused();
}

std::string DirectorSceneAccess::getEngineVersion() const
{
    return axmolVersion();
}

Vec2 DirectorSceneAccess::getDesignResolution() const
{
    auto* renderView = Director::getInstance()->getRenderView();
    return renderView ? renderView->getDesignResolutionSize() : Vec2::ZERO;
}

Vec2 DirectorSceneAccess::getFrameSize() const
{
    auto* renderView = Director::getInstance()->getRenderView();
    return renderView ? renderView->getFrameSize() : Vec2::ZERO;
}

float DirectorSceneAccess::getFrameRate() const
{
    return Director::getInstance()->getFrameRate();
}

}  // namespace ax
