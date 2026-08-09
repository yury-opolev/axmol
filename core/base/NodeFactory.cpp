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

#include "base/NodeFactory.h"

#include <algorithm>
#include <atomic>

#include "2d/Label.h"
#include "2d/Layer.h"  // LayerColor lives here
#include "2d/Node.h"
#include "2d/Sprite.h"
#include "base/ResourcePath.h"  // pathEscapesWritableSandbox - the one path-escape rule

namespace ax
{

namespace
{

std::atomic<NodeFactory*> s_instance{nullptr};

/// Reads an optional string parameter. Returns false only when the name IS present but holds
/// something other than a string - an absent optional parameter is not an error.
bool optionalString(const PropertyBag& params, std::string_view name, std::string& out, std::string& outError)
{
    const auto* value = findParam(params, name);
    if (!value)
        return true;
    if (!std::holds_alternative<std::string>(*value))
    {
        outError = std::string("\"").append(name).append("\" must be a string");
        return false;
    }
    out = std::get<std::string>(*value);
    return true;
}

/// Reads an optional number, accepting an int where a float is wanted: a caller that sends 24
/// rather than 24.0 for a font size means the same thing, and rejecting it would be pedantry
/// rather than safety.
bool optionalFloat(const PropertyBag& params, std::string_view name, float& out, std::string& outError)
{
    const auto* value = findParam(params, name);
    if (!value)
        return true;
    if (std::holds_alternative<float>(*value))
        out = std::get<float>(*value);
    else if (std::holds_alternative<int>(*value))
        out = static_cast<float>(std::get<int>(*value));
    else
    {
        outError = std::string("\"").append(name).append("\" must be a number");
        return false;
    }
    return true;
}

bool requiredString(const PropertyBag& params, std::string_view name, std::string& out, std::string& outError)
{
    const auto* value = findParam(params, name);
    if (!value)
    {
        // Name the parameter. "could not create sprite" sends a caller guessing; "requires
        // texturePath" tells them exactly what to add.
        outError = std::string("\"").append(name).append("\" is required");
        return false;
    }
    if (!std::holds_alternative<std::string>(*value))
    {
        outError = std::string("\"").append(name).append("\" must be a string");
        return false;
    }
    out = std::get<std::string>(*value);
    if (out.empty())
    {
        outError = std::string("\"").append(name).append("\" must not be empty");
        return false;
    }
    return true;
}

/// Rejects an asset path that leaves the project's resources.
///
/// A scene file is untrusted data and this layer is ungated, so it runs in release builds too.
/// FileUtils::fullPathForFilename returns an absolute path unchanged, which means an unchecked
/// texturePath is "render any image on this machine" - and with a screenshot tool on the other end
/// of the bridge, that is a file-disclosure primitive. Assets live under the resource root; a
/// value that starts at the filesystem root or climbs out of it is not an asset reference.
bool isResourceRelativePath(std::string_view path, std::string& outError)
{
    if (pathEscapesWritableSandbox(path))
    {
        outError = "\"" + std::string(path) + "\" must be a resource-relative path";
        return false;
    }
    return true;
}

bool endsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

const PropertyValue* findParam(const PropertyBag& params, std::string_view name)
{
    const auto it = std::find_if(params.begin(), params.end(),
                                 [name](const auto& entry) { return entry.first == name; });
    return it == params.end() ? nullptr : &it->second;
}

NodeFactory* NodeFactory::getInstance()
{
    // Same construction as NodeReflection::getInstance(), deliberately: these two are singletons of
    // the same kind, called from the same functions (serializeNode, handleNodeCreate), and having
    // one of them be thread-safe while the other is not would be a distinction nobody could
    // justify later. A compare-exchange rather than std::call_once because destroyInstance() has to
    // work - a once-flag cannot express "again".
    auto* existing = s_instance.load(std::memory_order_acquire);
    if (existing)
        return existing;

    auto* created = new NodeFactory();
    if (s_instance.compare_exchange_strong(existing, created, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return created;
    }

    delete created;
    return existing;
}

void NodeFactory::destroyInstance()
{
    delete s_instance.exchange(nullptr, std::memory_order_acq_rel);
}

NodeFactory::NodeFactory()
{
    registerType("ax::Node", {}, [](const PropertyBag&, std::string&) -> Node* { return Node::create(); });

    registerType("ax::Sprite", {"texturePath"},
                 [](const PropertyBag& params, std::string& outError) -> Node* {
                     std::string texturePath;
                     if (!requiredString(params, "texturePath", texturePath, outError))
                         return nullptr;
                     if (!isResourceRelativePath(texturePath, outError))
                         return nullptr;

                     auto* sprite = Sprite::create(texturePath);
                     if (!sprite)
                     {
                         // Sprite::create returns null for a missing or undecodable image. Say
                         // which path failed - the usual cause is a file that is not on the
                         // resource search path, which the caller can only fix if they know it.
                         outError = "could not create a sprite from \"" + texturePath + "\" (is it on the resource search path?)";
                         return nullptr;
                     }
                     return sprite;
                 });

    // fontName and fontSize are declared so the serializer captures them (a label's font is fixed
    // at construction and cannot be applied afterwards), but they are genuinely optional to
    // supply: a label with no font named is a valid thing to ask for, and Axmol has a platform
    // default for exactly that. Contrast texturePath, which is required because there is no such
    // thing as a sprite with no texture.
    registerType("ax::Label", {"fontName", "fontSize"},
                 [](const PropertyBag& params, std::string& outError) -> Node* {
                     std::string text;
                     std::string fontName;
                     float fontSize = 24.0f;
                     if (!optionalString(params, "text", text, outError) ||
                         !optionalString(params, "fontName", fontName, outError) ||
                         !optionalFloat(params, "fontSize", fontSize, outError))
                         return nullptr;

                     if (fontSize <= 0.0f)
                     {
                         outError = "\"fontSize\" must be greater than zero";
                         return nullptr;
                     }

                     if (!fontName.empty() && !isResourceRelativePath(fontName, outError))
                         return nullptr;

                     // A .ttf name is a font FILE and must go through createWithTTF;
                     // createWithSystemFont would treat it as a system font family name and
                     // silently render in the platform default instead.
                     Label* label = endsWith(fontName, ".ttf") ? Label::createWithTTF(text, fontName, fontSize)
                                                               : Label::createWithSystemFont(text, fontName, fontSize);
                     if (!label)
                     {
                         outError = fontName.empty() ? std::string("could not create a label")
                                                     : "could not create a label with font \"" + fontName + "\"";
                         return nullptr;
                     }
                     return label;
                 });

    registerType("ax::LayerColor", {}, [](const PropertyBag&, std::string&) -> Node* {
        // Colour and size are ordinary writable properties, so they are applied after
        // construction like any other - nothing needs to be known up front.
        return LayerColor::create();
    });
}

bool NodeFactory::registerType(std::string_view typeName, std::vector<std::string> creationParams, Creator creator)
{
    if (typeName.empty() || !creator || find(typeName))
        return false;

    _registrations.push_back({std::string(typeName), std::move(creationParams), std::move(creator)});
    return true;
}

const NodeFactory::Registration* NodeFactory::find(std::string_view typeName) const
{
    const auto it = std::find_if(_registrations.begin(), _registrations.end(),
                                 [typeName](const Registration& entry) { return entry.typeName == typeName; });
    return it == _registrations.end() ? nullptr : &*it;
}

bool NodeFactory::isRegistered(std::string_view typeName) const
{
    return find(typeName) != nullptr;
}

std::vector<std::string> NodeFactory::registeredTypes() const
{
    std::vector<std::string> names;
    names.reserve(_registrations.size());
    for (const auto& entry : _registrations)
        names.push_back(entry.typeName);
    return names;
}

std::vector<std::string> NodeFactory::creationParams(std::string_view typeName) const
{
    const auto* registration = find(typeName);
    return registration ? registration->creationParams : std::vector<std::string>{};
}

Node* NodeFactory::create(std::string_view typeName, const PropertyBag& params, std::string& outError) const
{
    const auto* registration = find(typeName);
    if (!registration)
    {
        outError = "unknown node type \"" + std::string(typeName) + "\"";
        return nullptr;
    }

    outError.clear();
    Node* node = registration->creator(params, outError);
    if (!node && outError.empty())
        outError = "could not create a node of type \"" + std::string(typeName) + "\"";
    return node;
}

}  // namespace ax
