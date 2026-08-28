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

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/Macros.h"
#include "base/NodeReflection.h"

namespace ax
{

class Node;

/** Named construction parameters, in the same value space as reflected properties so a caller
    that can read a property can pass it straight back as a creation parameter. */
using PropertyBag = std::vector<std::pair<std::string, PropertyValue>>;

/** Looks up `name` in `params`. Returns nullptr when absent. */
AX_DLL const PropertyValue* findParam(const PropertyBag& params, std::string_view name);

/** Creates nodes from a type name plus construction parameters.

    WHY THIS IS SEPARATE FROM PropertyProvider. A provider answers "what can I read and write on a
    node that already exists". Construction asks a different question: what must be known BEFORE
    the object exists. A Sprite needs its texture at construction - which is exactly why the
    sprite provider exposes "texturePath" as READ-ONLY, since assigning to it cannot retexture a
    live sprite. Folding creation into PropertyProvider would force every provider to carry a
    create() it has no way to implement, so the two registries stay parallel: NodeReflection for
    living nodes, NodeFactory for bringing them into being.

    Registration is expected during setup. Like NodeReflection, this class is not synchronised:
    register types before any other thread can reach it, and call create() on the Axmol main
    thread, since the nodes it returns are live engine objects. */
class AX_DLL NodeFactory
{
public:
    static NodeFactory* getInstance();
    static void destroyInstance();

    /** Builds a node from `params`, or returns nullptr with `outError` set to a reason a caller
        can act on. Implementations must not throw. */
    using Creator = std::function<Node*(const PropertyBag& params, std::string& outError)>;

    /** Registers a creator under a type name (e.g. "ax::Sprite"), declaring which parameters a
        serializer must record for this type to be reconstructible.

        `creationParams` are read back through NodeReflection when a node is serialized, so every
        name listed here must be a property some provider exposes for that type - otherwise a
        round trip silently loses the information needed to rebuild it.

        `requiredParams` is the subset of `creationParams` without which `creator` REFUSES to build -
        the ones it puts through requiredString and friends rather than the optional variants. It has
        to be declared here because requiredness otherwise lives only inside the creator's body, where
        nothing but a call can discover it, and a SERIALIZER needs to know before it writes: a node
        whose required parameter cannot be read back off it is a node the file could not rebuild, and
        writing it as though it could produces a file that saves cleanly and never loads. That is not
        hypothetical - see SceneSerializer's degradation path, which exists because a MeshRenderer
        built in code has no modelPath to record.

        Keep it beside the creator it describes. It duplicates what the creator's own requiredString
        calls say, and the only defence against the two drifting apart is that they are three lines
        from each other.

        Returns false, leaving the existing registration untouched, if `typeName` is already
        registered or `creator` is empty. */
    bool registerType(std::string_view typeName,
                      std::vector<std::string> creationParams,
                      std::vector<std::string> requiredParams,
                      Creator creator);

    /** Whether a creator is registered for this exact type name. */
    bool isRegistered(std::string_view typeName) const;

    /** Registered type names, in registration order. This is what makes the registry's limits
        discoverable rather than something a caller has to find by trial and error. */
    std::vector<std::string> registeredTypes() const;

    /** The parameter names a serializer must capture for `typeName`, or an empty vector if the
        type is unregistered or needs none. */
    std::vector<std::string> creationParams(std::string_view typeName) const;

    /** The subset of creationParams() that `typeName` cannot be built without - see registerType.

        A serializer asks this to find out whether what it just read off a node is enough to rebuild
        it, WITHOUT building one: constructing a node to find out would load the very asset the
        question is about. Empty for an unregistered type, and for the many types that need nothing. */
    std::vector<std::string> requiredCreationParams(std::string_view typeName) const;

    /** Creates a node. Returns nullptr with `outError` set if the type is unknown, a required
        parameter is missing or of the wrong type, or the engine refused to build the object
        (e.g. a texture that does not exist).

        The returned node is autoreleased, following the usual Axmol convention: attach it to a
        parent before the current autorelease pool drains, or it is destroyed. */
    Node* create(std::string_view typeName, const PropertyBag& params, std::string& outError) const;

private:
    NodeFactory();
    ~NodeFactory() = default;

    struct Registration
    {
        std::string typeName;
        std::vector<std::string> creationParams;
        std::vector<std::string> requiredParams;
        Creator creator;
    };

    const Registration* find(std::string_view typeName) const;

    std::vector<Registration> _registrations;
};

}  // namespace ax
