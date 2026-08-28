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
#include <variant>

#include "2d/Camera.h"
#include "2d/Node.h"
#include "2d/Scene.h"
#include "base/NodeFactory.h"
#include "base/NodeReflection.h"
#include "base/PropertyJson.h"
#if defined(AX_ENABLE_3D)
// For hasGeneratedChildren: a loaded model's children follow from its model path.
#    include "3d/MeshRenderer.h"
#endif

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace ax
{

bool isEngineManagedNode(Node* node)
{
    // A Scene's OWN default camera, not every camera. The first version of this was
    // `dynamic_cast<Camera*>(node) != nullptr`, which also caught cameras the game created - and a
    // world/UI camera split is an ordinary Axmol pattern. Those were then silently dropped from
    // every save and refused by node.delete and node.reparent, i.e. a rule written to protect one
    // engine-owned object quietly took ownership of the game's objects too.
    //
    // The thing that actually makes _defaultCamera special is that Scene holds it as a raw pointer
    // and recreates it on removal, so identity against the parent Scene is the right test.
    auto* scene = dynamic_cast<Scene*>(node->getParent());
    return scene && scene->getDefaultCamera() == node;
}

namespace
{

/// Whether this node's children exist only because it loaded a model, and so must not be written.
///
/// A model with several named objects becomes one child MeshRenderer per object. Those children
/// are not authored content: they follow from the model path recorded on the parent, and loading
/// it again recreates them exactly. Only the parent carries that path, so writing them out emits
/// MeshRenderers with an empty modelPath - which the loader then rejects, making the file
/// unloadable while the save reported success.
///
/// Keyed on a non-empty model path rather than on "is a MeshRenderer with children": a
/// MeshRenderer built in code and given children deliberately is ordinary content, and dropping
/// those would lose real work.
bool hasGeneratedChildren(Node* node)
{
#if defined(AX_ENABLE_3D)
    auto* mesh = dynamic_cast<MeshRenderer*>(node);
    return mesh && !mesh->getModelPath().empty();
#else
    (void)node;
    return false;
#endif
}

/// Deeper than any real scene, shallow enough that the recursion below cannot exhaust the stack.
/// A scene file is DATA - it can arrive from anywhere, and this code is deliberately ungated so
/// release builds can load scenes - so its nesting must be bounded rather than trusted.
constexpr int kMaxDepth = 128;

/// Position inside the DOCUMENT, for error and warning messages only.
///
/// Deliberately rooted at "#" rather than "/", because it is not a scene path and must not be
/// mistaken for one. The two index spaces do not agree: a document lists children in the order
/// they were written and omits engine-managed nodes, while a scene path indexes SORTED child
/// order in a parent that may already have children of its own. Telling an agent that something
/// went wrong "at /0/2" would invite it to address /0/2 in the live scene and quietly get a
/// different node.
std::string documentPath(const std::string& parentPath, size_t index)
{
    return parentPath + "/" + std::to_string(index);
}

/// Whether `have` - the creation parameters just read off a live node - would satisfy a factory that
/// demands `required`. Names the first one that would not, so the warning can say which.
///
/// ASKED RATHER THAN TRIED. The direct way to find out is to build the node and see, but building it
/// means loading the very asset the question is about - and the question is being asked precisely
/// because that asset may not exist. So the test mirrors NodeFactory's requiredString instead:
/// present, and if it is a string, not empty. An empty asset path is not a path.
bool hasEveryRequiredParam(const std::vector<std::string>& required,
                           const PropertyBag& have,
                           std::string& outMissing)
{
    for (const auto& name : required)
    {
        const PropertyValue* value = findParam(have, name);
        if (value == nullptr)
        {
            outMissing = name;
            return false;
        }
        if (const auto* text = std::get_if<std::string>(value); text != nullptr && text->empty())
        {
            outMissing = name;
            return false;
        }
    }
    return true;
}

/// Writes one node and its subtree.
///
/// `depth` is bounded for the same reason the load side is: an agent can build an arbitrarily deep
/// chain with a loop of node.create calls, and a save would then recurse once per level on the
/// Axmol main thread. Guarding only the load direction would leave the stack-overflow hole open
/// from the other end. A subtree deeper than the cap is truncated with a marker rather than
/// failing the whole save, since the caller's scene is not something they can be asked to fix.
///
/// Two details make the marker actually loadable, which is the whole point of degrading instead of
/// failing:
///
///   - It is emitted at `depth >= kMaxDepth`, not `>`. The marker is itself a node in the
///     document, so writing it at depth kMaxDepth + 1 produced a file the loader then rejected
///     for being one level too deep - a save that reported success and could never be read back.
///   - Its `type` is ax::Node, with the real type recorded alongside. Keeping the original type
///     would emit, say, an ax::Sprite with no `create` block, which fails on load with
///     "texturePath is required" - again unloadable, and for a reason that names the wrong
///     problem.
rapidjson::Value serializeNode(Node* node,
                               rapidjson::Document::AllocatorType& allocator,
                               std::vector<std::string>& outWarnings,
                               const std::string& path = "#",
                               int depth               = 0)
{
    if (depth >= kMaxDepth)
    {
        const std::string typeName = NodeReflection::getTypeName(node);
        rapidjson::Value truncated(rapidjson::kObjectType);
        truncated.AddMember("type", jsonString("ax::Node", allocator), allocator);
        truncated.AddMember("truncated", true, allocator);
        truncated.AddMember("truncatedType", jsonString(typeName, allocator), allocator);
        outWarnings.push_back("truncated at " + std::to_string(kMaxDepth) + " levels deep: \"" + typeName +
                              "\" at " + path + " was written as an empty ax::Node, and its subtree was dropped");
        return truncated;
    }

    auto* reflection = NodeReflection::getInstance();
    auto* factory    = NodeFactory::getInstance();

    const std::string typeName = NodeReflection::getTypeName(node);

    rapidjson::Value obj(rapidjson::kObjectType);

    // WHAT THE FACTORY WOULD NEED, read off the live node BEFORE anything is written - because
    // whether it is complete decides what type this node is written as.
    PropertyBag creation;
    for (const auto& name : factory->creationParams(typeName))
    {
        PropertyValue value;
        if (reflection->getProperty(node, name, value))
            creation.emplace_back(name, std::move(value));
    }

    // A NODE OF A KNOWN TYPE THAT STILL CANNOT BE REBUILT.
    //
    // A registered type is normally enough to guarantee a round trip - that is what registration
    // means. It is not enough when the node was not built from the asset its factory takes: a
    // MeshRenderer whose geometry was generated in code has no modelPath, so reflection reports an
    // empty string, and an empty string is exactly what the factory REFUSES ("modelPath must not be
    // empty"). Writing it anyway produced a file that saved cleanly, reported success, and could
    // then never be loaded by any option - allowDegraded included, since that covers an unknown
    // TYPE and this type is perfectly well known.
    //
    // So it is written as the same plain marker a too-deep subtree gets, and for the identical
    // reason given there: the marker has to be a node the loader can actually build, or degrading is
    // just a different way of writing a broken file. The real type travels alongside it, and the
    // props and children are kept, so nothing is lost but the geometry that was never in the file to
    // begin with.
    std::string unrecoverable;
    const bool rebuildable = !factory->isRegistered(typeName) ||
                             hasEveryRequiredParam(factory->requiredCreationParams(typeName), creation, unrecoverable);

    if (!rebuildable)
    {
        obj.AddMember("type", jsonString("ax::Node", allocator), allocator);
        obj.AddMember("degraded", true, allocator);
        obj.AddMember("degradedType", jsonString(typeName, allocator), allocator);
        obj.AddMember("create", rapidjson::Value(rapidjson::kObjectType), allocator);
        outWarnings.push_back("wrote \"" + typeName + "\" at " + path + " as a plain ax::Node: its \"" +
                              unrecoverable +
                              "\" is not recoverable from the node, so a rebuilt one would have been refused");
    }
    else
    {
        obj.AddMember("type", jsonString(typeName, allocator), allocator);

        if (!factory->isRegistered(typeName))
        {
            // Recorded rather than dropped: the file keeps this node's properties and children, so
            // nothing is lost, and a reader can tell exactly what it cannot rebuild.
            obj.AddMember("unsupported", true, allocator);
        }

        rapidjson::Value create(rapidjson::kObjectType);
        for (auto& [name, value] : creation)
        {
            create.AddMember(jsonString(name, allocator), encodePropertyValue(value, allocator), allocator);
        }
        obj.AddMember("create", create, allocator);
    }

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
    size_t index = 0;
    // A node whose children were BUILT BY LOADING ITS OWN MODEL contributes none of them to the
    // file. Loading a model with several named objects produces one child MeshRenderer per object,
    // so those children are not content anyone authored - they are a consequence of the model
    // path already recorded above, and loading it again recreates them.
    //
    // Writing them anyway was actively broken, not merely wasteful: only the root carries the
    // model path, so each generated child was emitted as an ax::MeshRenderer with an EMPTY
    // modelPath, and the loader rejected the file it had just written with "modelPath must not be
    // empty". The save reported success. Every such scene was unloadable.
    if (!hasGeneratedChildren(node))
    {
        for (auto* child : node->getChildren())
        {
            if (child && !isEngineManagedNode(child))
            {
                children.PushBack(serializeNode(child, allocator, outWarnings, documentPath(path, index), depth + 1),
                                  allocator);
                ++index;
            }
        }
    }
    obj.AddMember("children", children, allocator);

    return obj;
}

Node* deserializeNode(const rapidjson::Value& json,
                      const SceneSerializer::LoadOptions& options,
                      const std::string& path,
                      int depth,
                      std::string& outError,
                      std::vector<std::string>& outWarnings,
                      SceneSerializer::LoadFailure& outFailure)
{
    outFailure = SceneSerializer::LoadFailure::Malformed;

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
            outFailure = SceneSerializer::LoadFailure::UnknownType;
            outError   = "no factory registered for \"" + typeName + "\" (at " + path +
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
        outFailure = SceneSerializer::LoadFailure::CreationFailed;
        outError   = createError.empty() ? ("could not create " + effectiveType + " at " + path)
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
            Node* child = deserializeNode(*it, options, documentPath(path, index), depth + 1, outError, outWarnings, outFailure);
            if (!child)
            {
                // `node` is autoreleased and never attached, so abandoning it here releases it and
                // everything already added to it - no half-built tree escapes into the scene.
                return nullptr;
            }
            node->addChild(child);
        }
    }

    outFailure = SceneSerializer::LoadFailure::None;
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

bool SceneSerializer::serialize(Node* root,
                                std::string& outJson,
                                std::string& outError,
                                std::vector<std::string>* outWarnings)
{
    std::vector<std::string> warnings;
    const auto report = [&]() {
        if (outWarnings)
            *outWarnings = warnings;
    };

    if (!root)
    {
        outError = "cannot serialize a null node";
        report();
        return false;
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();
    doc.AddMember("format", jsonString(kFormatName, allocator), allocator);
    doc.AddMember("version", kFormatVersion, allocator);
    doc.AddMember("root", serializeNode(root, allocator, warnings), allocator);
    report();

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
                                   std::vector<std::string>& outWarnings,
                                   LoadFailure* outFailure)
{
    outError.clear();
    outWarnings.clear();

    LoadFailure failure = LoadFailure::Malformed;
    const auto report   = [&]() {
        if (outFailure)
            *outFailure = failure;
    };

    rapidjson::Document doc;
    const rapidjson::Value* root = nullptr;
    if (!parseEnvelope(json, doc, root, outError))
    {
        report();
        return nullptr;
    }

    Node* node = deserializeNode(*root, options, "#", 0, outError, outWarnings, failure);
    report();
    return node;
}

bool SceneSerializer::deserializeChildren(std::string_view json,
                                          const LoadOptions& options,
                                          std::vector<Node*>& outChildren,
                                          std::string& outError,
                                          std::vector<std::string>& outWarnings,
                                          LoadFailure* outFailure)
{
    outChildren.clear();
    outError.clear();
    outWarnings.clear();

    LoadFailure failure = LoadFailure::Malformed;
    const auto report   = [&]() {
        if (outFailure)
            *outFailure = failure;
    };

    rapidjson::Document doc;
    const rapidjson::Value* root = nullptr;
    if (!parseEnvelope(json, doc, root, outError))
    {
        report();
        return false;
    }

    if (!root->IsObject())
    {
        outError = "node at # is not a JSON object";
        report();
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
    {
        failure = LoadFailure::None;
        report();
        return true;  // A root with no children is an empty scene, not an error.
    }

    if (!childrenIt->value.IsArray())
    {
        outError = "\"children\" at # is not an array";
        report();
        return false;
    }

    // The root node is deliberately never constructed here - see the header. Only its children
    // are built, so a scene whose root is the game's own Scene subclass still loads.
    size_t index = 0;
    for (auto it = childrenIt->value.Begin(); it != childrenIt->value.End(); ++it, ++index)
    {
        Node* child = deserializeNode(*it, options, documentPath("#", index), 1, outError, outWarnings, failure);
        if (!child)
        {
            // Everything built so far is autoreleased and unattached, so abandoning the vector
            // releases it: a partial scene never reaches the caller.
            outChildren.clear();
            report();
            return false;
        }
        outChildren.push_back(child);
    }

    failure = LoadFailure::None;
    report();
    return true;
}

}  // namespace ax
