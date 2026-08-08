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

    struct LoadOptions
    {
        /** Substitute a plain ax::Node for any type with no registered factory, instead of
            failing. Every substitution is reported through `outWarnings`. */
        bool allowDegraded = false;
    };

    /** Writes the subtree rooted at `root` into `outJson`. Returns false with `outError` set if
        `root` is null. Never throws. */
    static bool serialize(Node* root, std::string& outJson, std::string& outError);

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
                                    std::vector<std::string>& outWarnings);

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
                             std::vector<std::string>& outWarnings);
};

}  // namespace ax
