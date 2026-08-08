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

#include "base/SceneSerializer.h"

#include <algorithm>

#include "2d/Camera.h"
#include "2d/Node.h"
#include "base/NodeFactory.h"
#include "base/NodeReflection.h"
#include "base/PropertyJson.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace ax
{

namespace
{

/// Nodes the ENGINE owns, as opposed to content someone put in the scene.
///
/// Every Scene creates its own default Camera in initWithSize(). Writing it into a scene file
/// would be wrong twice over: nothing can meaningfully reconstruct one from reflected properties,
/// and loading the file into a live scene - which already has its own camera - would add a second.
/// A scene file describes content; the engine's own furniture is not content.
bool isEngineManaged(Node* node)
{
    return dynamic_cast<Camera*>(node) != nullptr;
}

/// Writes one node and its subtree. `path` is only used to make errors and warnings locatable.
rapidjson::Value serializeNode(Node* node, rapidjson::Document::AllocatorType& allocator)
{
    auto* reflection = NodeReflection::getInstance();
    auto* factory    = NodeFactory::getInstance();

    const std::string typeName = NodeReflection::getTypeName(node);

    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("type", jsonString(typeName, allocator), allocator);

    if (!factory->isRegistered(typeName))
    {
        // Recorded rather than dropped: the file keeps this node's properties and children, so
        // nothing is lost, and a reader can tell exactly what it cannot rebuild.
        obj.AddMember("unsupported", true, allocator);
    }

    rapidjson::Value create(rapidjson::kObjectType);
    for (const auto& name : factory->creationParams(typeName))
    {
        PropertyValue value;
        if (reflection->getProperty(node, name, value))
            create.AddMember(jsonString(name, allocator), encodePropertyValue(value, allocator), allocator);
    }
    obj.AddMember("create", create, allocator);

    rapidjson::Value props(rapidjson::kObjectType);
    for (const auto& info : reflection->listProperties(node))
    {
        // Read-only properties are skipped: they cannot be applied on load, and writing them
        // would suggest a fidelity the format does not have. The ones that DO matter for
        // reconstruction (texturePath, fontName, fontSize) are captured above, under "create",
        // which is exactly the split those properties exist for.
        if (!info.writable)
            continue;
        PropertyValue value;
        if (reflection->getProperty(node, info.name, value))
            props.AddMember(jsonString(info.name, allocator), encodePropertyValue(value, allocator), allocator);
    }
    obj.AddMember("props", props, allocator);

    rapidjson::Value children(rapidjson::kArrayType);
    // Sorted order, matching how NodeReflection addresses children by index. Note that indices in
    // the FILE are into this filtered list: engine-managed children (a Scene's default Camera) are
    // skipped, so a live path and a file path can differ by the number of skipped siblings before
    // it. Within the file the order is the render order, which is what matters for rebuilding.
    node->sortAllChildren();
    for (auto* child : node->getChildren())
    {
        if (child && !isEngineManaged(child))
            children.PushBack(serializeNode(child, allocator), allocator);
    }
    obj.AddMember("children", children, allocator);

    return obj;
}

std::string childPath(const std::string& parentPath, size_t index)
{
    return parentPath == "/" ? "/" + std::to_string(index) : parentPath + "/" + std::to_string(index);
}

/// Deeper than any real scene, shallow enough that the recursion below cannot exhaust the stack.
/// A scene file is DATA - it can arrive from anywhere, and this code is deliberately ungated so
/// release builds can load scenes - so its nesting must be bounded rather than trusted.
constexpr int kMaxDepth = 128;

Node* deserializeNode(const rapidjson::Value& json,
                      const SceneSerializer::LoadOptions& options,
                      const std::string& path,
                      int depth,
                      std::string& outError,
                      std::vector<std::string>& outWarnings)
{
    if (depth > kMaxDepth)
    {
        outError = "scene nesting is deeper than " + std::to_string(kMaxDepth) + " levels (at " + path + ")";
        return nullptr;
    }

    if (!json.IsObject())
    {
        outError = "node at " + path + " is not a JSON object";
        return nullptr;
    }

    const auto typeIt = json.FindMember("type");
    if (typeIt == json.MemberEnd() || !typeIt->value.IsString())
    {
        outError = "node at " + path + " has no \"type\" string";
        return nullptr;
    }
    const std::string typeName(typeIt->value.GetString(), typeIt->value.GetStringLength());

    auto* factory    = NodeFactory::getInstance();
    auto* reflection = NodeReflection::getInstance();

    std::string effectiveType = typeName;
    if (!factory->isRegistered(typeName))
    {
        if (!options.allowDegraded)
        {
            outError = "no factory registered for \"" + typeName + "\" (at " + path +
                       "); pass allowDegraded to substitute a plain ax::Node";
            return nullptr;
        }
        effectiveType = "ax::Node";
        outWarnings.push_back("substituted ax::Node for \"" + typeName + "\" at " + path);
    }

    PropertyBag params;
    if (const auto createIt = json.FindMember("create"); createIt != json.MemberEnd())
    {
        if (!createIt->value.IsObject())
        {
            outError = "\"create\" at " + path + " is not an object";
            return nullptr;
        }
        for (auto it = createIt->value.MemberBegin(); it != createIt->value.MemberEnd(); ++it)
        {
            PropertyValue value;
            if (!decodePropertyValueInferred(it->value, value))
            {
                outError = "creation parameter \"" + std::string(it->name.GetString()) + "\" at " + path +
                           " has an unsupported value shape";
                return nullptr;
            }
            params.emplace_back(std::string(it->name.GetString(), it->name.GetStringLength()), std::move(value));
        }
    }

    std::string createError;
    Node* node = factory->create(effectiveType, params, createError);
    if (!node)
    {
        outError = createError.empty() ? ("could not create " + effectiveType + " at " + path)
                                       : (createError + " (at " + path + ")");
        return nullptr;
    }

    if (const auto propsIt = json.FindMember("props"); propsIt != json.MemberEnd())
    {
        if (!propsIt->value.IsObject())
        {
            outError = "\"props\" at " + path + " is not an object";
            return nullptr;
        }

        const auto available = reflection->listProperties(node);
        for (auto it = propsIt->value.MemberBegin(); it != propsIt->value.MemberEnd(); ++it)
        {
            const std::string name(it->name.GetString(), it->name.GetStringLength());
            const auto info = std::find_if(available.begin(), available.end(),
                                           [&name](const PropertyInfo& candidate) { return candidate.name == name; });

            // A property this build does not have, or one it exposes as read-only, is reported
            // and skipped rather than fatal: a file written by a newer build should still load,
            // but what it lost must not be invisible.
            if (info == available.end())
            {
                outWarnings.push_back("ignored unknown property \"" + name + "\" at " + path);
                continue;
            }
            if (!info->writable)
            {
                outWarnings.push_back("ignored read-only property \"" + name + "\" at " + path);
                continue;
            }

            PropertyValue value;
            if (!decodePropertyValue(it->value, info->type, value))
            {
                outError = "property \"" + name + "\" at " + path + " does not match its declared type " +
                           propertyTypeName(info->type);
                return nullptr;
            }
            if (!reflection->setProperty(node, name, value))
                outWarnings.push_back("property \"" + name + "\" at " + path + " was rejected by the engine");
        }
    }

    if (const auto childrenIt = json.FindMember("children"); childrenIt != json.MemberEnd())
    {
        if (!childrenIt->value.IsArray())
        {
            outError = "\"children\" at " + path + " is not an array";
            return nullptr;
        }
        size_t index = 0;
        for (auto it = childrenIt->value.Begin(); it != childrenIt->value.End(); ++it, ++index)
        {
            Node* child = deserializeNode(*it, options, childPath(path, index), depth + 1, outError, outWarnings);
            if (!child)
            {
                // `node` is autoreleased and never attached, so abandoning it here releases it and
                // everything already added to it - no half-built tree escapes into the scene.
                return nullptr;
            }
            node->addChild(child);
        }
    }

    return node;
}

}  // namespace

namespace
{

/// Shared front half of both load paths: parse, validate the envelope, and hand back the "root"
/// value. Returns false with `outError` set on anything malformed.
bool parseEnvelope(std::string_view json,
                   rapidjson::Document& doc,
                   const rapidjson::Value*& outRoot,
                   std::string& outError)
{
    // ITERATIVE, not the default recursive-descent parser. axmol's bundled rapidjson defaults to
    // kParseNoFlags, whose parser recurses per nesting level - a ~165 KB document of nested
    // objects overflows the stack and kills the process. That is unrecoverable rather than an
    // error: this runs on the Axmol main thread, so no try/catch upstream can contain it.
    doc.Parse<rapidjson::kParseIterativeFlag>(json.data(), json.size());
    if (doc.HasParseError() || !doc.IsObject())
    {
        outError = "scene data is not a JSON object";
        return false;
    }

    const auto formatIt = doc.FindMember("format");
    if (formatIt == doc.MemberEnd() || !formatIt->value.IsString() ||
        std::string_view(formatIt->value.GetString(), formatIt->value.GetStringLength()) !=
            SceneSerializer::kFormatName)
    {
        outError = std::string("not an ") + SceneSerializer::kFormatName + " document";
        return false;
    }

    const auto versionIt = doc.FindMember("version");
    if (versionIt == doc.MemberEnd() || !versionIt->value.IsInt())
    {
        outError = "missing \"version\"";
        return false;
    }
    if (versionIt->value.GetInt() != SceneSerializer::kFormatVersion)
    {
        // No forwards compatibility is claimed, so say what was found rather than trying to read a
        // layout this build does not know.
        outError = "unsupported scene version " + std::to_string(versionIt->value.GetInt()) + "; this build reads " +
                   std::to_string(SceneSerializer::kFormatVersion);
        return false;
    }

    const auto rootIt = doc.FindMember("root");
    if (rootIt == doc.MemberEnd())
    {
        outError = "missing \"root\"";
        return false;
    }

    outRoot = &rootIt->value;
    return true;
}

}  // namespace

bool SceneSerializer::serialize(Node* root, std::string& outJson, std::string& outError)
{
    if (!root)
    {
        outError = "cannot serialize a null node";
        return false;
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();
    doc.AddMember("format", jsonString(kFormatName, allocator), allocator);
    doc.AddMember("version", kFormatVersion, allocator);
    doc.AddMember("root", serializeNode(root, allocator), allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    outJson.assign(buffer.GetString(), buffer.GetSize());
    outError.clear();
    return true;
}

Node* SceneSerializer::deserialize(std::string_view json,
                                   const LoadOptions& options,
                                   std::string& outError,
                                   std::vector<std::string>& outWarnings)
{
    outError.clear();
    outWarnings.clear();

    rapidjson::Document doc;
    const rapidjson::Value* root = nullptr;
    if (!parseEnvelope(json, doc, root, outError))
        return nullptr;

    return deserializeNode(*root, options, "/", 0, outError, outWarnings);
}

bool SceneSerializer::deserializeChildren(std::string_view json,
                                          const LoadOptions& options,
                                          std::vector<Node*>& outChildren,
                                          std::string& outError,
                                          std::vector<std::string>& outWarnings)
{
    outChildren.clear();
    outError.clear();
    outWarnings.clear();

    rapidjson::Document doc;
    const rapidjson::Value* root = nullptr;
    if (!parseEnvelope(json, doc, root, outError))
        return false;

    if (!root->IsObject())
    {
        outError = "node at / is not a JSON object";
        return false;
    }

    // Skipping the root is the point of this function, but it is only obviously correct when the
    // root could not have been rebuilt anyway (a game's own Scene subclass). If the root IS a type
    // this build can create, the caller has silently lost a node, so say so.
    if (const auto typeIt = root->FindMember("type");
        typeIt != root->MemberEnd() && typeIt->value.IsString())
    {
        const std::string rootType(typeIt->value.GetString(), typeIt->value.GetStringLength());
        if (NodeFactory::getInstance()->isRegistered(rootType))
        {
            outWarnings.push_back("loaded contents only: the document's root (" + rootType +
                                  ") was not recreated");
        }
    }

    const auto childrenIt = root->FindMember("children");
    if (childrenIt == root->MemberEnd())
        return true;  // A root with no children is an empty scene, not an error.

    if (!childrenIt->value.IsArray())
    {
        outError = "\"children\" at / is not an array";
        return false;
    }

    // The root node is deliberately never constructed here - see the header. Only its children
    // are built, so a scene whose root is the game's own Scene subclass still loads.
    size_t index = 0;
    for (auto it = childrenIt->value.Begin(); it != childrenIt->value.End(); ++it, ++index)
    {
        Node* child = deserializeNode(*it, options, childPath("/", index), 1, outError, outWarnings);
        if (!child)
        {
            // Everything built so far is autoreleased and unattached, so abandoning the vector
            // releases it: a partial scene never reaches the caller.
            outChildren.clear();
            return false;
        }
        outChildren.push_back(child);
    }
    return true;
}

}  // namespace ax
