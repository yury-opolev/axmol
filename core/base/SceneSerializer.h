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
#include <vector>

#include "base/Macros.h"

namespace ax
{

class Node;

/** Whether `node` is engine infrastructure rather than content.

    Currently: a Scene's default Camera. Such nodes are skipped when writing a scene file, and -
    just as importantly - must not be deleted or reparented by tooling. Scene holds its default
    camera in a RAW pointer whose only reference is the children array, so removing it destroys
    the camera and leaves Scene::_defaultCamera dangling; Scene::onProjectionChanged then
    dereferences it on the next window resize. Scene::removeAllChildren goes out of its way to
    retain the camera across removal, which is the same ownership fact seen from the other side.

    One predicate, so the "this is the engine's own furniture" rule has a single definition. */
AX_DLL bool isEngineManagedNode(Node* node);

/** Reads and writes node trees as JSON.

    FORMAT (version 1):

        {
          "format": "axmol-scene",
          "version": 1,
          "root": {
            "type": "ax::Sprite",
            "create": { "texturePath": "axmol.png" },
            "props":  { "position": {"x": 360, "y": 640}, "visible": true },
            "children": []
          }
        }

    A node may additionally carry `"unsupported": true` (no factory can rebuild this type - see
    below) or `"truncated": true` with `"truncatedType"` (the subtree was deeper than 128 levels).
    Both are readable by a version-1 reader that ignores unknown members, so neither bumps
    kFormatVersion.

    WHY "create" AND "props" ARE SEPARATE. `create` holds what NodeFactory needs BEFORE the node
    exists; `props` holds writable reflected properties applied after. Merging them breaks in both
    directions: a Sprite's texturePath cannot be applied afterwards (it is read-only, because
    assigning to it cannot retexture a live sprite), and a contentSize applied BEFORE a sprite's
    texture is set is immediately overwritten by the texture's own size.

    `props` is generated from the reflected properties that are writable - the same set
    node.properties reports - rather than from a hand-maintained list, so the format tracks the
    reflection layer instead of drifting away from it.

    ENGINE-MANAGED NODES ARE NOT CONTENT. Every Scene creates its own default Camera; such nodes
    are skipped when writing, because nothing can reconstruct one from reflected properties and
    loading a file into a live scene that already has its own camera would add a second.

    The test is identity against the parent Scene's `getDefaultCamera()`, deliberately NOT "is a
    Camera". A game's own second camera - a world/UI split is a standard pattern - is content, and
    an earlier version that matched every Camera silently dropped those from saves and made them
    undeletable through the bridge.

    A node deeper than 128 levels is written as an empty `ax::Node` carrying "truncated": true and
    "truncatedType", and reported through serialize()'s warnings. The depth bound exists because a
    scene file is data that can arrive from anywhere and both directions recurse; the marker is a
    plain node so that the resulting file still LOADS, which a truncated ax::Sprite (no `create`
    block) would not.

    UNREPRESENTABLE NODES. A node whose type has no registered factory is written with
    "unsupported": true, keeping its properties and children so the file loses nothing. Loading
    such a file FAILS by default, naming the type and its path. LoadOptions::allowDegraded
    substitutes a plain ax::Node instead and reports every substitution as a warning. Silent
    degradation is deliberately not available: a scene that loads "successfully" having dropped
    every sprite fails later, in a screenshot, with nothing left pointing at the cause.

    Threading: serialize() reads and deserialize() creates live Node objects, so both must run on
    the Axmol main thread. */
class AX_DLL SceneSerializer
{
public:
    static constexpr const char* kFormatName = "axmol-scene";
    static constexpr int kFormatVersion      = 1;

    /** Why a load failed, so a caller can map it onto its own error vocabulary instead of
        pattern-matching the message. "This build cannot make an ax::Menu" is a different thing
        for an agent to be told than "this file is malformed": one is answered by asking what CAN
        be made, the other by fixing the file. */
    enum class LoadFailure
    {
        None,
        Malformed,      ///< not a scene document, wrong version, or structurally invalid
        UnknownType,    ///< a node type with no registered factory (and allowDegraded was off)
        CreationFailed  ///< the type is known but the engine refused to build it
    };

    struct LoadOptions
    {
        /** Substitute a plain ax::Node for any type with no registered factory, instead of
            failing. Every substitution is reported through `outWarnings`. */
        bool allowDegraded = false;
    };

    /** Writes the subtree rooted at `root` into `outJson`. Returns false with `outError` set if
        `root` is null. Never throws.

        `outWarnings` reports what the file does NOT faithfully contain - currently only depth
        truncation. It is optional because most callers have nothing to do with it, but a caller
        that surfaces results to a human or an agent should pass it: a save that silently dropped
        a subtree looks exactly like one that did not. */
    static bool serialize(Node* root,
                          std::string& outJson,
                          std::string& outError,
                          std::vector<std::string>* outWarnings = nullptr);

    /** Builds only the CHILDREN of the document's root, leaving the root itself unbuilt.

        This is what "load this scene" means in practice. A whole-scene save records the game's own
        scene class as the root type - MainScene, GameScene, whatever the project subclassed - and
        no factory can or should rebuild that: it is application code with its own constructor,
        members and lifecycle, not a reconstructible data node. What the caller actually wants is
        to keep the running scene object and repopulate it, which is exactly this.

        Returns false with `outError` set on the same failures as deserialize(). On success every
        returned node is autoreleased and detached: attach them before the pool drains. */
    static bool deserializeChildren(std::string_view json,
                                    const LoadOptions& options,
                                    std::vector<Node*>& outChildren,
                                    std::string& outError,
                                    std::vector<std::string>& outWarnings,
                                    LoadFailure* outFailure = nullptr);

    /** Builds a DETACHED node tree from `json`.

        Returns nullptr with `outError` set on malformed JSON, a wrong format/version, an unknown
        node type (unless allowDegraded), or a creation parameter the factory rejects. Recoverable
        oddities - a property this build does not know, or one that is not writable - are reported
        through `outWarnings` and skipped, since a file written by a newer build should not be
        unloadable, but neither should its losses be invisible.

        The returned root is autoreleased: attach it to a parent before the current autorelease
        pool drains, or it is destroyed. */
    static Node* deserialize(std::string_view json,
                             const LoadOptions& options,
                             std::string& outError,
                             std::vector<std::string>& outWarnings,
                             LoadFailure* outFailure = nullptr);
};

}  // namespace ax
