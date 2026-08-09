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

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/Macros.h"
#include "math/Vec2.h"
#include "math/Vec3.h"
#include "math/Color.h"

namespace ax
{

class Node;

/** The engine type of a reflected property value. Mirrors the alternatives of PropertyValue. */
enum class PropertyType
{
    Bool,
    Int,
    Float,
    String,
    Vec2,
    Vec3,
    Color
};

/** The value of a single reflected property. The active alternative must match the property's
    declared PropertyType - PropertyProvider implementations are expected to validate this
    themselves via std::holds_alternative before trusting the value. */
using PropertyValue = std::variant<bool, int, float, std::string, Vec2, Vec3, Color4B>;

/** Describes one property that a PropertyProvider exposes, without carrying its value. */
struct PropertyInfo
{
    std::string name;
    PropertyType type;
    bool writable;
};

/** Flat description of one node. Contains no engine pointers, so it is safe to serialise or to
    hand across a process boundary. */
struct NodeInfo
{
    std::string id;        ///< opaque per-run handle - correlation only, never an input to a lookup
                           ///< and deliberately NOT the node's address (see opaqueNodeId)
    std::string path;       ///< canonical child-index path, e.g. "/0/2"
    std::string typeName;   ///< demangled RTTI type name, e.g. "ax::Sprite"
    std::string name;       ///< Node::getName()
    int tag         = -1;
    int childCount  = 0;
    Vec2 position;
    Vec2 contentSize;
    Vec2 anchorPoint;
    float scaleX       = 1.0f;
    float scaleY       = 1.0f;
    float rotation     = 0.0f;
    int localZOrder    = 0;
    float globalZOrder = 0.0f;
    bool visible        = true;
    Color4B color;  ///< rgb from Node::getColor(), a from Node::getOpacity() - opacity is carried
                    ///< in the alpha channel, and writing this property also writes opacity.
};

/** Supplies a named set of properties for a family of node types (e.g. all nodes, sprites,
    labels). NodeReflection consults every registered provider whose supports() accepts the
    node and dispatches get/set to the first one that recognises the property name. */
class AX_DLL PropertyProvider
{
public:
    virtual ~PropertyProvider() = default;

    /** Whether this provider knows how to read/write properties on the given node. */
    virtual bool supports(Node* node) const = 0;

    /** The properties this provider exposes, independent of any particular node instance. */
    virtual std::vector<PropertyInfo> list() const = 0;

    /** Reads a property by name. Returns false without touching `out` if the name is unknown
        to this provider. */
    virtual bool get(Node* node, std::string_view name, PropertyValue& out) const = 0;

    /** Writes a property by name. Returns false, and must leave the node unmodified, if the
        name is unknown, the property is read-only, or `value` holds the wrong alternative. */
    virtual bool set(Node* node, std::string_view name, const PropertyValue& value) const = 0;
};

/** Data-oriented reflection over the scene graph: tree traversal, type names, and per-node
    property get/set, with no UI dependency. Used by both the ImGui Inspector and headless
    tooling (e.g. an agent bridge) so the traversal and reflection logic exist exactly once.

    Nodes are addressed by child-index path (e.g. "/0/2/1"), never by pointer: a pointer is
    meaningless across a process restart, unstable across frames, and unsafe to dereference
    once the node is freed. A path is deterministic, human-readable, and safe to resolve against
    a possibly-changed tree - resolution simply fails if a segment is now out of range.

    Paths are indices into each node's SORTED (render) child order, not its insertion order:
    Node::sortAllChildren() reorders children in place whenever local z-order changes, and
    resolve(), describeTree(), and pathOf() all sort before indexing so a path always means the
    same thing visit() would draw. A path is therefore stable as long as z-order and tree
    structure do not change between computing it and resolving it.

    Threading: every method here reads or writes live Node objects and MUST be called on the
    Axmol main thread. Callers running on another thread - e.g. the Console listener thread that
    will drive this in Stage 3 - must marshal onto the main thread first, for example via
    Director::getInstance()->getScheduler()->runOnAxmolThread(...). addProvider() and
    removeProvider() are not synchronised against concurrent getProperty()/setProperty() calls;
    register or unregister providers during setup, before any other thread can reach this class. */
class AX_DLL NodeReflection
{
public:
    static NodeReflection* getInstance();
    static void destroyInstance();

    /** Strips compiler-specific decoration from a typeid().name() so it reads as plain C++,
        e.g. "ax::Node" rather than "class ax::Node" (MSVC) or a mangled Itanium name. */
    static std::string demangle(const char* mangled);

    /** The demangled RTTI type name of `node`, e.g. "ax::Sprite". Returns "" if node is null. */
    static std::string getTypeName(Node* node);

    /** Describes a single node. `path` is stamped onto the result as-is; callers that already
        know the node's path (e.g. describeTree) pass it through instead of recomputing it. */
    NodeInfo describe(Node* node, std::string_view path = "/") const;

    /** Pre-order flattening of the subtree rooted at `root` (parent before children, children
        in child-index order). maxDepth < 0 means unlimited; 0 means root only; 1 means root
        plus its direct children; and so on. Returns an empty vector if `root` is null. */
    std::vector<NodeInfo> describeTree(Node* root, int maxDepth = -1) const;

    /** Resolves a child-index path against `root`. The root itself is "/". Returns nullptr for
        a null root, an empty path, a path not starting with '/', a non-numeric segment, a
        segment that overflows int, a negative index, or an out-of-range index. */
    Node* resolve(Node* root, std::string_view path) const;

    /** The child-index path from `root` to `node`, or "" if `node` is not `root` and not one of
        its descendants. pathOf(root, root) is "/". pathOf() followed by resolve() round-trips
        to the same node. */
    std::string pathOf(Node* root, Node* node) const;

    /** The union of PropertyInfo from every registered provider that supports `node`. Empty if
        `node` is null. */
    std::vector<PropertyInfo> listProperties(Node* node) const;

    /** Reads a property by consulting registered providers most-recently-registered-first (see
        the ordering comment on _providers); the first provider that both supports the node and
        recognises the name wins. Returns false without writing `out` if node is null or no
        provider recognises the name. */
    bool getProperty(Node* node, std::string_view name, PropertyValue& out) const;

    /** Writes a property the same way getProperty reads one. Returns false if node is null, the
        name is unrecognised, the property is read-only, or `value` holds the wrong variant
        alternative. Never throws, never coerces. Provider implementations of
        PropertyProvider::set are REQUIRED to leave the node completely unmodified when they
        reject a write - NodeReflection itself has no way to roll back a misbehaving provider, so
        this is a contract on implementors rather than a guarantee NodeReflection enforces. */
    bool setProperty(Node* node, std::string_view name, const PropertyValue& value);

    /** Registers a provider under `id`. Returns false, without replacing the existing one, if
        `id` is already registered. */
    bool addProvider(std::string_view id, std::unique_ptr<PropertyProvider> provider);

    /** Unregisters the provider with the given id, if any. */
    void removeProvider(std::string_view id);

private:
    NodeReflection();
    ~NodeReflection() = default;

    /// Recursive worker behind the public describeTree(Node*, int): `path` is the
    /// already-resolved path of `root` at this recursion level, and `depth` is `root`'s distance
    /// from the original root, so maxDepth can be compared directly against it.
    void describeTree(Node* node, int maxDepth, int depth, const std::string& path, std::vector<NodeInfo>& out) const;

    // Providers are consulted most-recently-registered-first: addProvider() inserts at the
    // FRONT of _providers, so the last provider registered for a given name always wins and can
    // override an earlier one. Without this rule the built-in NodePropertyProvider - which
    // supports() every node and claims all common property names - would make any later,
    // colliding provider unreachable. The constructor registers the built-ins in the order Node,
    // Label, Sprite; front-insertion then makes the effective consultation order Sprite, Label,
    // Node, i.e. specific providers are tried before the general one, and any provider a caller
    // registers afterwards is tried before all of the built-ins.
    std::vector<std::pair<std::string, std::unique_ptr<PropertyProvider>>> _providers;
};

}  // namespace ax
