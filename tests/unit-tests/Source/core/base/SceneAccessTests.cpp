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

#include <doctest.h>

#include "base/SceneAccess.h"

#include "2d/ActionInterval.h"
#include "2d/Node.h"
#include "base/Director.h"
#include "base/Scheduler.h"

// Unlike AgentRequestHandlerTests, which drives a fake so the suite needs no engine, these
// exercise the REAL DirectorSceneAccess. They are here because pausing is the one piece of it
// whose correctness is a claim about the engine's own scheduler, and a fake would only assert that
// the fake agrees with itself.

using namespace ax;

namespace
{

/// Gives `node` a running MoveBy that the scheduler will actually advance.
///
/// Two pieces of setup, both of which a test silently passes without:
///
///   - resumeTarget: runAction() adds the action PAUSED when the node is not `_running`, which a
///     detached test node never is. Without it the node stays put whether or not setPaused was
///     called, and the freeze assertion holds against every possible implementation.
///   - scheduleUpdate: the headless Director in this binary does not have the ActionManager on its
///     scheduler, so Scheduler::update() would advance nothing at all - see the CONTROL case at
///     the bottom of this file, which exists to keep that assumption honest. Scheduling it here
///     reproduces what Director::init does in a real game (PRIORITY_SYSTEM), which is precisely
///     the arrangement setPaused relies on.
Node* nodeWithARunningAction()
{
    auto* node = Node::create();
    node->retain();
    node->setPosition(Vec2::ZERO);
    node->runAction(MoveBy::create(1.0f, Vec2(100.0f, 0.0f)));

    auto* actionManager = Director::getInstance()->getActionManager();
    actionManager->resumeTarget(node);
    Director::getInstance()->getScheduler()->scheduleUpdate(actionManager, Scheduler::PRIORITY_SYSTEM, false);
    return node;
}

/// Advances the engine far enough for an ActionInterval to actually move.
///
/// Two ticks, not one: the first is ActionInterval's `_firstTick`, which only sets _elapsed = 0 and
/// calls update(0). A single-step helper would make "it did not move" true for every test whether
/// paused or not.
void stepEngine(float seconds)
{
    Director::getInstance()->getScheduler()->update(seconds * 0.5f);
    Director::getInstance()->getScheduler()->update(seconds * 0.5f);
}

}  // namespace

TEST_SUITE("core/base/SceneAccess")
{
    TEST_CASE("setPaused freezes running actions, and resume lets them continue")
    {
        // The property the whole method exists for. Worth pinning precisely because the
        // implementation is indirect: it pauses SCHEDULER TARGETS, and actions stop only because
        // Director schedules the ActionManager as one of them (at PRIORITY_SYSTEM, which
        // pauseAllTargets' minimum includes). An earlier version also called
        // ActionManager::pauseAllRunningActions(), which was redundant - and, because the
        // Vector<Node*> it returns RETAINS, kept every animating node alive for the whole pause.
        auto* node = nodeWithARunningAction();

        DirectorSceneAccess access;
        REQUIRE_FALSE(access.isPaused());

        access.setPaused(true);
        CHECK(access.isPaused());

        stepEngine(0.5f);
        CHECK(node->getPosition().x == doctest::Approx(0.0f));

        access.setPaused(false);
        CHECK_FALSE(access.isPaused());

        stepEngine(0.5f);
        CHECK(node->getPosition().x > 0.0f);

        node->removeFromParentAndCleanup(true);
        node->release();
    }

    TEST_CASE("pausing does not retain the nodes it freezes")
    {
        // The regression that removing pauseAllRunningActions() fixed. While the Vector<Node*> was
        // held, an agent could pause, delete an animating node, and resume - and the node stayed
        // alive and detached for the whole window, its lifetime silently extended by an
        // introspection call that promised only to stop time.
        auto* node = nodeWithARunningAction();

        const auto before = node->getReferenceCount();

        DirectorSceneAccess access;
        access.setPaused(true);
        CHECK(node->getReferenceCount() == before);

        access.setPaused(false);
        CHECK(node->getReferenceCount() == before);

        node->stopAllActions();
        node->release();
    }

    TEST_CASE("destroying the access object resumes a scene it had paused")
    {
        // Otherwise tearing the bridge down mid-pause leaves the game frozen for good: the window
        // keeps drawing a scene that never advances, and the thing that offered director.resume is
        // the thing that just went away.
        auto* node = nodeWithARunningAction();

        {
            DirectorSceneAccess access;
            access.setPaused(true);
            stepEngine(0.25f);
            REQUIRE(node->getPosition().x == doctest::Approx(0.0f));
        }

        stepEngine(0.25f);
        CHECK(node->getPosition().x > 0.0f);

        node->stopAllActions();
        node->release();
    }

    TEST_CASE("setPaused is idempotent")
    {
        // Pausing twice must not capture a second, overlapping set of targets - resuming once
        // would then leave the extras paused forever.
        auto* node = nodeWithARunningAction();

        DirectorSceneAccess access;
        access.setPaused(true);
        access.setPaused(true);
        access.setPaused(false);

        stepEngine(0.5f);
        CHECK(node->getPosition().x > 0.0f);

        node->stopAllActions();
        node->release();
    }
}

TEST_SUITE("core/base/SceneAccess-control")
{
    TEST_CASE("CONTROL: an action advances under Scheduler::update with no pause involved")
    {
        auto* node = Node::create();
        node->retain();
        node->setPosition(Vec2::ZERO);
        node->runAction(MoveBy::create(1.0f, Vec2(100.0f, 0.0f)));
        auto* actionManager = Director::getInstance()->getActionManager();
        actionManager->resumeTarget(node);
        Director::getInstance()->getScheduler()->scheduleUpdate(actionManager, Scheduler::PRIORITY_SYSTEM, false);
        // TWICE: ActionInterval's first tick only initialises (_firstTick sets _elapsed = 0 and
        // calls update(0)), so a single step never moves anything. A one-step test would have
        // "proved" that pausing works while proving only that actions do nothing in one frame.
        Director::getInstance()->getScheduler()->update(0.5f);
        Director::getInstance()->getScheduler()->update(0.5f);
        CHECK(node->getPosition().x > 0.0f);
        node->stopAllActions();
        node->release();
    }
}
