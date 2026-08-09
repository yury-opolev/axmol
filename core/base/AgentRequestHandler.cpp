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

#include "base/AgentRequestHandler.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "2d/Node.h"
#include "base/NodeFactory.h"
#include "base/NodeReflection.h"
#include "base/SceneSerializer.h"
// The JSON <-> PropertyValue codec lives in PropertyJson so that this bridge and SceneSerializer
// share exactly one encoding: written twice, the wire format and the scene-file format would
// drift, and a scene would round-trip differently depending on which door it came through.
#include "base/PropertyJson.h"
#include "base/SceneAccess.h"
#include "math/Color.h"
#include "math/Vec2.h"
#include "math/Vec3.h"
#include "platform/PlatformConfig.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace ax
{

namespace
{

rapidjson::Value nodeInfoToJson(const NodeInfo& info, rapidjson::Document::AllocatorType& allocator)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("id", jsonString(info.id, allocator), allocator);
    obj.AddMember("path", jsonString(info.path, allocator), allocator);
    obj.AddMember("typeName", jsonString(info.typeName, allocator), allocator);
    obj.AddMember("name", jsonString(info.name, allocator), allocator);
    obj.AddMember("tag", info.tag, allocator);
    obj.AddMember("childCount", info.childCount, allocator);
    obj.AddMember("position", vec2ToJson(info.position, allocator), allocator);
    obj.AddMember("contentSize", vec2ToJson(info.contentSize, allocator), allocator);
    obj.AddMember("anchorPoint", vec2ToJson(info.anchorPoint, allocator), allocator);
    obj.AddMember("scaleX", info.scaleX, allocator);
    obj.AddMember("scaleY", info.scaleY, allocator);
    obj.AddMember("rotation", info.rotation, allocator);
    obj.AddMember("localZOrder", info.localZOrder, allocator);
    obj.AddMember("globalZOrder", info.globalZOrder, allocator);
    obj.AddMember("visible", info.visible, allocator);
    obj.AddMember("color", colorToJson(info.color, allocator), allocator);
    return obj;
}

/// `describeTree()` always roots its own output at "/" for whatever node it was given (see the
/// doc comment on NodeReflection::describeTree) - it has no idea that node might itself sit deep
/// inside the real tree. `prefix` is the absolute path (from the running scene) of that node;
/// this rewrites one of describeTree's node-relative paths into the same absolute addressing
/// space every other method (node.describe/get/set) uses, so a client can take a path straight
/// out of a scene.tree result and feed it back in without translating it first.
std::string combinePath(std::string_view prefix, std::string_view relative)
{
    if (relative == "/")
        return std::string(prefix);
    if (prefix == "/")
        return std::string(relative);
    return std::string(prefix) + std::string(relative);
}

// -------------------------------------------------------------------------------------------
// Request parameter helpers. All return false - meaning "not present, or present with the wrong
// JSON type" - so every call site can react the same way: invalid_params.
// -------------------------------------------------------------------------------------------

bool getRequiredString(const rapidjson::Value& params, const char* name, std::string& out)
{
    auto it = params.FindMember(name);
    if (it == params.MemberEnd() || !it->value.IsString())
        return false;
    out.assign(it->value.GetString(), it->value.GetStringLength());
    return true;
}

bool getRequiredNumber(const rapidjson::Value& params, const char* name, float& out)
{
    auto it = params.FindMember(name);
    if (it == params.MemberEnd() || !it->value.IsNumber())
        return false;
    out = it->value.GetFloat();

    // A JSON number with no finite float representation (anything past +-FLT_MAX) becomes
    // +-Infinity via GetFloat(); reject it here rather than let it reach input.tap/input.swipe -
    // this is the fix for a reproducible permanent freeze: {"x1":0,"y1":0,"x2":1e39,"y2":0} made
    // SceneAccess::injectSwipe's old distance-decrementing loop never terminate, because
    // `inf - 1 == inf`. Every caller of getRequiredNumber() already maps a `false` return to
    // invalid_params, so this doesn't need its own error path.
    if (!std::isfinite(out))
        return false;
    return true;
}

// -------------------------------------------------------------------------------------------
// Method handlers. Each takes the request's "params" object and the response Document (so it
// can allocate through the same allocator the final result is serialised with), and reports
// failure via `code`/`message` rather than throwing - see the class comment on
// AgentRequestHandler on why this is never allowed to throw.
// -------------------------------------------------------------------------------------------

/// Resolves `path` against the current running scene. Sets `code`/`message` and returns nullptr
/// for either of the two ways this can fail: no scene at all (no_scene), or a scene with no node
/// at that path (invalid_path). Shared by every method that addresses a node.
Node* resolveNode(SceneAccess* access, std::string_view path, std::string& code, std::string& message)
{
    Node* scene = access->getRunningScene();
    if (!scene)
    {
        code    = "no_scene";
        message = "no running scene";
        return nullptr;
    }

    Node* node = NodeReflection::getInstance()->resolve(scene, path);
    if (!node)
    {
        code    = "invalid_path";
        message = "no node at path: " + std::string(path);
        return nullptr;
    }
    return node;
}

bool handleAppInfo(SceneAccess* access,
                    const rapidjson::Value& /*params*/,
                    rapidjson::Document& doc,
                    rapidjson::Value& result,
                    std::string& /*code*/,
                    std::string& /*message*/)
{
#if AX_TARGET_PLATFORM == AX_PLATFORM_WIN32
    static constexpr const char* kPlatformName = "win32";
#elif AX_TARGET_PLATFORM == AX_PLATFORM_WINRT
    static constexpr const char* kPlatformName = "winrt";
#elif AX_TARGET_PLATFORM == AX_PLATFORM_MAC
    static constexpr const char* kPlatformName = "macos";
#elif AX_TARGET_PLATFORM == AX_PLATFORM_IOS
    static constexpr const char* kPlatformName = "ios";
#elif AX_TARGET_PLATFORM == AX_PLATFORM_TVOS
    static constexpr const char* kPlatformName = "tvos";
#elif AX_TARGET_PLATFORM == AX_PLATFORM_ANDROID
    static constexpr const char* kPlatformName = "android";
#elif AX_TARGET_PLATFORM == AX_PLATFORM_LINUX
    static constexpr const char* kPlatformName = "linux";
#elif AX_TARGET_PLATFORM == AX_PLATFORM_WASM
    static constexpr const char* kPlatformName = "wasm";
#else
    static constexpr const char* kPlatformName = "unknown";
#endif

    auto& allocator = doc.GetAllocator();
    result.SetObject();
    result.AddMember("engineVersion", jsonString(access->getEngineVersion(), allocator), allocator);
    result.AddMember("platform", jsonString(kPlatformName, allocator), allocator);
    result.AddMember("designResolution", vec2ToJson(access->getDesignResolution(), allocator), allocator);
    result.AddMember("frameSize", vec2ToJson(access->getFrameSize(), allocator), allocator);
    result.AddMember("fps", access->getFrameRate(), allocator);
    result.AddMember("paused", access->isPaused(), allocator);
    return true;
}

bool handleSceneTree(SceneAccess* access,
                      const rapidjson::Value& params,
                      rapidjson::Document& doc,
                      rapidjson::Value& result,
                      std::string& code,
                      std::string& message)
{
    std::string path = "/";
    if (auto it = params.FindMember("path"); it != params.MemberEnd())
    {
        if (!it->value.IsString())
        {
            code    = "invalid_params";
            message = "\"path\" must be a string";
            return false;
        }
        path.assign(it->value.GetString(), it->value.GetStringLength());
    }

    int depth = -1;
    if (auto it = params.FindMember("depth"); it != params.MemberEnd())
    {
        if (!it->value.IsInt())
        {
            code    = "invalid_params";
            message = "\"depth\" must be an integer";
            return false;
        }
        depth = it->value.GetInt();
    }

    Node* start = resolveNode(access, path, code, message);
    if (!start)
        return false;

    auto tree = NodeReflection::getInstance()->describeTree(start, depth);

    auto& allocator = doc.GetAllocator();
    result.SetArray();
    for (auto& info : tree)
    {
        info.path = combinePath(path, info.path);
        result.PushBack(nodeInfoToJson(info, allocator), allocator);
    }
    return true;
}

bool handleNodeDescribe(SceneAccess* access,
                         const rapidjson::Value& params,
                         rapidjson::Document& doc,
                         rapidjson::Value& result,
                         std::string& code,
                         std::string& message)
{
    std::string path;
    if (!getRequiredString(params, "path", path))
    {
        code    = "invalid_params";
        message = "\"path\" is required";
        return false;
    }

    Node* node = resolveNode(access, path, code, message);
    if (!node)
        return false;

    const auto info = NodeReflection::getInstance()->describe(node, path);
    result           = nodeInfoToJson(info, doc.GetAllocator());
    return true;
}

bool handleNodeProperties(SceneAccess* access,
                           const rapidjson::Value& params,
                           rapidjson::Document& doc,
                           rapidjson::Value& result,
                           std::string& code,
                           std::string& message)
{
    std::string path;
    if (!getRequiredString(params, "path", path))
    {
        code    = "invalid_params";
        message = "\"path\" is required";
        return false;
    }

    Node* node = resolveNode(access, path, code, message);
    if (!node)
        return false;

    auto& allocator = doc.GetAllocator();
    result.SetArray();
    for (const auto& property : NodeReflection::getInstance()->listProperties(node))
    {
        rapidjson::Value entry(rapidjson::kObjectType);
        entry.AddMember("name", jsonString(property.name, allocator), allocator);
        entry.AddMember("type", jsonString(propertyTypeName(property.type), allocator), allocator);
        entry.AddMember("writable", property.writable, allocator);
        result.PushBack(entry, allocator);
    }
    return true;
}

bool handleNodeGet(SceneAccess* access,
                    const rapidjson::Value& params,
                    rapidjson::Document& doc,
                    rapidjson::Value& result,
                    std::string& code,
                    std::string& message)
{
    std::string path, name;
    if (!getRequiredString(params, "path", path) || !getRequiredString(params, "name", name))
    {
        code    = "invalid_params";
        message = "\"path\" and \"name\" are required";
        return false;
    }

    Node* node = resolveNode(access, path, code, message);
    if (!node)
        return false;

    PropertyValue value;
    if (!NodeReflection::getInstance()->getProperty(node, name, value))
    {
        code    = "unknown_property";
        message = "no such property: " + name;
        return false;
    }

    auto& allocator = doc.GetAllocator();
    result.SetObject();
    result.AddMember("name", jsonString(name, allocator), allocator);
    result.AddMember("type", jsonString(propertyTypeName(propertyTypeOfValue(value)), allocator), allocator);
    result.AddMember("value", encodePropertyValue(value, allocator), allocator);
    return true;
}

bool handleNodeSet(SceneAccess* access,
                    const rapidjson::Value& params,
                    rapidjson::Document& doc,
                    rapidjson::Value& result,
                    std::string& code,
                    std::string& message)
{
    std::string path, name;
    if (!getRequiredString(params, "path", path) || !getRequiredString(params, "name", name))
    {
        code    = "invalid_params";
        message = "\"path\" and \"name\" are required";
        return false;
    }

    auto valueIt = params.FindMember("value");
    if (valueIt == params.MemberEnd())
    {
        code    = "invalid_params";
        message = "\"value\" is required";
        return false;
    }

    Node* node = resolveNode(access, path, code, message);
    if (!node)
        return false;

    auto* reflection = NodeReflection::getInstance();

    // setProperty() only returns bool, so the property's declared type and writability have to
    // be looked up separately in order to (a) tell unknown_property apart from readonly_property
    // - see the class comment on AgentRequestHandler - and (b) know which PropertyType to decode
    // the incoming JSON "value" against in the first place.
    std::optional<PropertyInfo> info;
    for (const auto& property : reflection->listProperties(node))
    {
        if (property.name == name)
        {
            info = property;
            break;
        }
    }

    if (!info)
    {
        code    = "unknown_property";
        message = "no such property: " + name;
        return false;
    }
    if (!info->writable)
    {
        code    = "readonly_property";
        message = "property is read-only: " + name;
        return false;
    }

    PropertyValue decoded;
    if (!decodePropertyValue(valueIt->value, info->type, decoded))
    {
        code    = "type_mismatch";
        message = "\"value\" does not match the declared type of \"" + name + "\"";
        return false;
    }

    if (!reflection->setProperty(node, name, decoded))
    {
        // The value's shape matched the declared PropertyType (decodePropertyValue succeeded)
        // but the provider still refused the write - e.g. NodeReflection's own "opacity"
        // property rejects out-of-range ints without ever touching the node. unknown_property and
        // readonly_property are already ruled out above, and the closed error-code set (design
        // doc §4) has no dedicated "well-typed but semantically invalid" code, so type_mismatch is
        // the closest fit: it tells the caller their value was rejected, not miswritten.
        code    = "type_mismatch";
        message = "value was rejected for \"" + name + "\"";
        return false;
    }

    result.SetObject();

    // A successful write can CHANGE THIS NODE'S OWN PATH. Paths index sorted child order, and
    // writing localZOrder (or anything else that marks the parent's child list dirty) re-sorts
    // the siblings, so the index that addressed this node a moment ago may now address a
    // different one. Returning the post-write path lets a client follow the node instead of
    // silently reading its sibling on the next call.
    Node* scene = access->getRunningScene();
    const std::string newPath = scene ? NodeReflection::getInstance()->pathOf(scene, node) : std::string();
    result.AddMember("path", rapidjson::Value(newPath.c_str(), doc.GetAllocator()).Move(), doc.GetAllocator());
    return true;
}

bool handleInputTap(SceneAccess* access,
                     const rapidjson::Value& params,
                     rapidjson::Document& /*doc*/,
                     rapidjson::Value& result,
                     std::string& code,
                     std::string& message)
{
    float x, y;
    if (!getRequiredNumber(params, "x", x) || !getRequiredNumber(params, "y", y))
    {
        code    = "invalid_params";
        message = "\"x\" and \"y\" are required numbers";
        return false;
    }

    access->injectTap(x, y);
    result.SetObject();
    return true;
}

bool handleInputSwipe(SceneAccess* access,
                       const rapidjson::Value& params,
                       rapidjson::Document& /*doc*/,
                       rapidjson::Value& result,
                       std::string& code,
                       std::string& message)
{
    float x1, y1, x2, y2;
    if (!getRequiredNumber(params, "x1", x1) || !getRequiredNumber(params, "y1", y1) ||
        !getRequiredNumber(params, "x2", x2) || !getRequiredNumber(params, "y2", y2))
    {
        code    = "invalid_params";
        message = "\"x1\", \"y1\", \"x2\" and \"y2\" are required numbers";
        return false;
    }

    access->injectSwipe(x1, y1, x2, y2);
    result.SetObject();
    return true;
}

bool handleScreenshot(SceneAccess* access,
                       const rapidjson::Value& params,
                       rapidjson::Document& doc,
                       rapidjson::Value& result,
                       std::string& code,
                       std::string& message)
{
    std::string file;
    if (auto it = params.FindMember("file"); it != params.MemberEnd())
    {
        if (!it->value.IsString())
        {
            code    = "invalid_params";
            message = "\"file\" must be a string";
            return false;
        }
        file.assign(it->value.GetString(), it->value.GetStringLength());

        // A client must never be able to write outside FileUtils::getWritablePath() (design doc
        // §5), so a traversing or absolute "file" is rejected here, before SceneAccess is ever
        // called - see also DirectorSceneAccess::captureScreenshot, which calls the same shared
        // pathEscapesWritableSandbox() as defense in depth for callers that reach it directly.
        if (pathEscapesWritableSandbox(file))
        {
            code    = "invalid_params";
            message = "\"file\" must not escape the writable path";
            return false;
        }
    }

    std::string outPath, outError;
    if (!access->captureScreenshot(file, outPath, outError))
    {
        code    = "internal_error";
        message = outError.empty() ? "screenshot capture failed" : outError;
        return false;
    }

    result.SetObject();
    result.AddMember("path", jsonString(outPath, doc.GetAllocator()), doc.GetAllocator());
    // captureScreenshot() is always fire-and-forget (see its WHY comment in SceneAccess.cpp): the
    // file at `path` is not guaranteed to exist yet when this response is sent. "pending" tells a
    // client to poll rather than assume the write has already completed.
    result.AddMember("pending", true, doc.GetAllocator());
    return true;
}

/// Applies a "props" object to a freshly created node. Shared by node.create and (indirectly)
/// scene.load, so a property written one way behaves identically written the other.
bool applyProps(Node* node, const rapidjson::Value& props, std::string& code, std::string& message)
{
    if (!props.IsObject())
    {
        code    = "invalid_params";
        message = "\"props\" must be an object";
        return false;
    }

    auto* reflection      = NodeReflection::getInstance();
    const auto available  = reflection->listProperties(node);
    for (auto it = props.MemberBegin(); it != props.MemberEnd(); ++it)
    {
        const std::string name(it->name.GetString(), it->name.GetStringLength());
        const auto info = std::find_if(available.begin(), available.end(),
                                       [&name](const PropertyInfo& candidate) { return candidate.name == name; });
        if (info == available.end())
        {
            code    = "unknown_property";
            message = "no such property: " + name;
            return false;
        }
        if (!info->writable)
        {
            code    = "readonly_property";
            message = "property is read-only: " + name;
            return false;
        }

        PropertyValue decoded;
        if (!decodePropertyValue(it->value, info->type, decoded))
        {
            code    = "type_mismatch";
            message = "\"" + name + "\" does not match its declared type";
            return false;
        }
        if (!reflection->setProperty(node, name, decoded))
        {
            code    = "type_mismatch";
            message = "value was rejected for \"" + name + "\"";
            return false;
        }
    }
    return true;
}

/// The path of `node` within the running scene, for reporting back where something ended up.
std::string pathWithinScene(SceneAccess* access, Node* node)
{
    Node* scene = access->getRunningScene();
    return scene ? NodeReflection::getInstance()->pathOf(scene, node) : std::string();
}

bool handleNodeCreate(SceneAccess* access,
                       const rapidjson::Value& params,
                       rapidjson::Document& doc,
                       rapidjson::Value& result,
                       std::string& code,
                       std::string& message)
{
    std::string parentPath, type;
    if (!getRequiredString(params, "parentPath", parentPath) || !getRequiredString(params, "type", type))
    {
        code    = "invalid_params";
        message = "\"parentPath\" and \"type\" are required";
        return false;
    }

    Node* parent = resolveNode(access, parentPath, code, message);
    if (!parent)
        return false;

    PropertyBag creationParams;
    if (auto createIt = params.FindMember("create"); createIt != params.MemberEnd())
    {
        if (!createIt->value.IsObject())
        {
            code    = "invalid_params";
            message = "\"create\" must be an object";
            return false;
        }
        for (auto it = createIt->value.MemberBegin(); it != createIt->value.MemberEnd(); ++it)
        {
            PropertyValue value;
            if (!decodePropertyValueInferred(it->value, value))
            {
                code    = "invalid_params";
                message = "creation parameter \"" + std::string(it->name.GetString()) + "\" has an unsupported value";
                return false;
            }
            creationParams.emplace_back(std::string(it->name.GetString(), it->name.GetStringLength()),
                                        std::move(value));
        }
    }

    std::string createError;
    Node* node = NodeFactory::getInstance()->create(type, creationParams, createError);
    if (!node)
    {
        // unknown_type rather than invalid_params: the caller's request was well-formed, the type
        // simply is not one this build can make. scene.types is how they find out which are.
        code    = NodeFactory::getInstance()->isRegistered(type) ? "invalid_params" : "unknown_type";
        message = createError;
        return false;
    }

    if (auto propsIt = params.FindMember("props"); propsIt != params.MemberEnd())
    {
        // Applied BEFORE attaching: a node rejected here must leave the scene untouched, and an
        // autoreleased node that was never added simply goes away.
        if (!applyProps(node, propsIt->value, code, message))
            return false;
    }

    parent->addChild(node);

    result.SetObject();
    result.AddMember("path", jsonString(pathWithinScene(access, node), doc.GetAllocator()), doc.GetAllocator());
    return true;
}

bool handleNodeDelete(SceneAccess* access,
                       const rapidjson::Value& params,
                       rapidjson::Document& /*doc*/,
                       rapidjson::Value& result,
                       std::string& code,
                       std::string& message)
{
    std::string path;
    if (!getRequiredString(params, "path", path))
    {
        code    = "invalid_params";
        message = "\"path\" is required";
        return false;
    }

    Node* node = resolveNode(access, path, code, message);
    if (!node)
        return false;

    // Deleting the running scene out from under the Director is not an edit, it is a crash.
    if (node == access->getRunningScene())
    {
        code    = "invalid_params";
        message = "refusing to delete the running scene";
        return false;
    }

    // A Scene's default Camera is an ordinary child, so it is addressable - but Scene keeps it in
    // a RAW pointer whose only reference is the children array. Removing it destroys the camera
    // and leaves Scene::_defaultCamera dangling, to be dereferenced on the next projection change.
    if (isEngineManagedNode(node))
    {
        code    = "invalid_params";
        message = "refusing to delete an engine-managed node (the scene owns it)";
        return false;
    }

    node->removeFromParent();
    result.SetObject();
    return true;
}

bool handleNodeReparent(SceneAccess* access,
                         const rapidjson::Value& params,
                         rapidjson::Document& doc,
                         rapidjson::Value& result,
                         std::string& code,
                         std::string& message)
{
    std::string path, toPath;
    if (!getRequiredString(params, "path", path) || !getRequiredString(params, "toPath", toPath))
    {
        code    = "invalid_params";
        message = "\"path\" and \"toPath\" are required";
        return false;
    }

    Node* node = resolveNode(access, path, code, message);
    if (!node)
        return false;

    Node* newParent = resolveNode(access, toPath, code, message);
    if (!newParent)
        return false;

    if (node == access->getRunningScene())
    {
        code    = "invalid_params";
        message = "refusing to reparent the running scene";
        return false;
    }
    // Same ownership problem as node.delete: moving a scene's default camera off it leaves
    // Scene::_defaultCamera pointing at a node the scene no longer owns.
    if (isEngineManagedNode(node))
    {
        code    = "invalid_params";
        message = "refusing to reparent an engine-managed node (the scene owns it)";
        return false;
    }
    if (node == newParent)
    {
        code    = "invalid_params";
        message = "a node cannot be its own parent";
        return false;
    }
    // Reparenting a node under its own descendant would detach that whole branch from the scene
    // and leak it - the cycle keeps both alive with nothing referencing them.
    for (Node* ancestor = newParent->getParent(); ancestor != nullptr; ancestor = ancestor->getParent())
    {
        if (ancestor == node)
        {
            code    = "invalid_params";
            message = "refusing to reparent a node beneath itself";
            return false;
        }
    }

    // retain() across the move: the parent's reference is the only one holding this subtree alive.
    //
    // removeFromParentAndCleanup(FALSE), not removeFromParent(): the latter is
    // removeFromParentAndCleanup(true), which runs stopAllActions() and unscheduleAllCallbacks()
    // on the node AND every descendant. Moving a node is not deleting it - an agent that reparents
    // an animating sprite would get a successful response, a correct path, and a silently frozen
    // sprite. Axmol's own comment in Node::cleanup says to pass false for exactly this case.
    node->retain();
    node->removeFromParentAndCleanup(false);
    newParent->addChild(node);
    node->release();

    result.SetObject();
    result.AddMember("path", jsonString(pathWithinScene(access, node), doc.GetAllocator()), doc.GetAllocator());
    return true;
}

bool handleSceneTypes(SceneAccess* /*access*/,
                       const rapidjson::Value& /*params*/,
                       rapidjson::Document& doc,
                       rapidjson::Value& result,
                       std::string& /*code*/,
                       std::string& /*message*/)
{
    auto& allocator = doc.GetAllocator();
    auto* factory   = NodeFactory::getInstance();

    result.SetArray();
    for (const auto& type : factory->registeredTypes())
    {
        rapidjson::Value entry(rapidjson::kObjectType);
        entry.AddMember("type", jsonString(type, allocator), allocator);
        rapidjson::Value params(rapidjson::kArrayType);
        for (const auto& name : factory->creationParams(type))
            params.PushBack(jsonString(name, allocator), allocator);
        entry.AddMember("createParams", params, allocator);
        result.PushBack(entry, allocator);
    }
    return true;
}

bool handleSceneSave(SceneAccess* access,
                      const rapidjson::Value& params,
                      rapidjson::Document& doc,
                      rapidjson::Value& result,
                      std::string& code,
                      std::string& message)
{
    std::string file;
    if (!getRequiredString(params, "file", file))
    {
        code    = "invalid_params";
        message = "\"file\" is required";
        return false;
    }
    if (pathEscapesWritableSandbox(file))
    {
        code    = "invalid_params";
        message = "\"file\" must not escape the writable path";
        return false;
    }

    std::string path = "/";
    if (auto pathIt = params.FindMember("path"); pathIt != params.MemberEnd())
    {
        if (!pathIt->value.IsString())
        {
            code    = "invalid_params";
            message = "\"path\" must be a string";
            return false;
        }
        path.assign(pathIt->value.GetString(), pathIt->value.GetStringLength());
    }

    Node* node = resolveNode(access, path, code, message);
    if (!node)
        return false;

    std::string json, serializeError;
    std::vector<std::string> warnings;
    if (!SceneSerializer::serialize(node, json, serializeError, &warnings))
    {
        code    = "internal_error";
        message = serializeError;
        return false;
    }

    std::string outPath, writeError;
    if (!access->writeTextFile(file, json, outPath, writeError))
    {
        code    = "internal_error";
        message = writeError;
        return false;
    }

    result.SetObject();
    result.AddMember("path", jsonString(outPath, doc.GetAllocator()), doc.GetAllocator());
    result.AddMember("bytes", static_cast<int>(json.size()), doc.GetAllocator());
    // Reported the same way scene.load reports its own, and always present so a caller does not
    // have to distinguish "no warnings" from "this build does not report them". A save that
    // dropped part of the tree must not be indistinguishable from one that did not.
    result.AddMember("warnings", jsonStringArray(warnings, doc.GetAllocator()), doc.GetAllocator());
    return true;
}

bool handleSceneLoad(SceneAccess* access,
                      const rapidjson::Value& params,
                      rapidjson::Document& doc,
                      rapidjson::Value& result,
                      std::string& code,
                      std::string& message)
{
    std::string file;
    if (!getRequiredString(params, "file", file))
    {
        code    = "invalid_params";
        message = "\"file\" is required";
        return false;
    }
    if (pathEscapesWritableSandbox(file))
    {
        // Reading is resolved through FileUtils rather than the sandbox, but a traversing name is
        // still refused: the bridge should not be a way to read arbitrary files off the machine.
        code    = "invalid_params";
        message = "\"file\" must not escape the resource path";
        return false;
    }

    std::string parentPath = "/";
    if (auto parentIt = params.FindMember("parentPath"); parentIt != params.MemberEnd())
    {
        if (!parentIt->value.IsString())
        {
            code    = "invalid_params";
            message = "\"parentPath\" must be a string";
            return false;
        }
        parentPath.assign(parentIt->value.GetString(), parentIt->value.GetStringLength());
    }

    bool replace = false;
    if (auto replaceIt = params.FindMember("replace"); replaceIt != params.MemberEnd())
    {
        if (!replaceIt->value.IsBool())
        {
            code    = "invalid_params";
            message = "\"replace\" must be a boolean";
            return false;
        }
        replace = replaceIt->value.GetBool();
    }

    // "contents" is what "load this scene" means: keep the running scene object - which is the
    // game's own Scene subclass, application code with its own lifecycle that no factory can or
    // should rebuild - and repopulate it from the file. "child" reconstructs the file's root node
    // itself, which is what you want for a saved subtree.
    bool contentsOnly = false;
    if (auto modeIt = params.FindMember("mode"); modeIt != params.MemberEnd())
    {
        if (!modeIt->value.IsString())
        {
            code    = "invalid_params";
            message = "\"mode\" must be a string";
            return false;
        }
        const std::string_view mode(modeIt->value.GetString(), modeIt->value.GetStringLength());
        if (mode == "contents")
            contentsOnly = true;
        else if (mode != "child")
        {
            code    = "invalid_params";
            message = "\"mode\" must be \"child\" or \"contents\"";
            return false;
        }
    }

    SceneSerializer::LoadOptions options;
    if (auto degradedIt = params.FindMember("allowDegraded"); degradedIt != params.MemberEnd())
    {
        if (!degradedIt->value.IsBool())
        {
            code    = "invalid_params";
            message = "\"allowDegraded\" must be a boolean";
            return false;
        }
        options.allowDegraded = degradedIt->value.GetBool();
    }

    Node* parent = resolveNode(access, parentPath, code, message);
    if (!parent)
        return false;

    std::string json, readError;
    if (!access->readTextFile(file, json, readError))
    {
        code    = "invalid_path";
        message = readError;
        return false;
    }

    std::string loadError;
    std::vector<std::string> warnings;
    std::vector<Node*> loadedChildren;
    Node* loadedRoot = nullptr;
    auto failure     = SceneSerializer::LoadFailure::None;

    const bool loaded = contentsOnly
                            ? SceneSerializer::deserializeChildren(json, options, loadedChildren, loadError,
                                                                   warnings, &failure)
                            : (loadedRoot = SceneSerializer::deserialize(json, options, loadError, warnings,
                                                                          &failure)) != nullptr;
    if (!loaded)
    {
        // "This build cannot make an ax::Menu" gets the SAME code here as it does from
        // node.create. An agent that special-cases unknown_type by asking scene.types what it can
        // make would otherwise fail to recognise the identical problem arriving through a load.
        //
        // A malformed file is invalid_params, not internal_error. It is the caller's file and the
        // caller's choice of file, and the serializer's message names the offending node - so it
        // is both their fault and theirs to fix. Reporting it as internal_error says "the engine
        // is broken", which sends an agent looking in exactly the wrong place; that is not a
        // hypothetical, it is what a bad fontName in a saved scene actually looked like.
        // CreationFailed keeps internal_error: a registered type that will not build IS ours.
        switch (failure)
        {
        case SceneSerializer::LoadFailure::UnknownType:
            code = "unknown_type";
            break;
        case SceneSerializer::LoadFailure::Malformed:
            code = "invalid_params";
            break;
        default:
            code = "internal_error";
            break;
        }
        message = loadError;
        return false;
    }

    // Only now that the whole tree is built is anything in the live scene touched: a failed load
    // must never leave the scene half-replaced.
    if (replace)
        parent->removeAllChildren();

    if (contentsOnly)
    {
        for (Node* child : loadedChildren)
            parent->addChild(child);
    }
    else
    {
        parent->addChild(loadedRoot);
    }

    auto& allocator = doc.GetAllocator();
    result.SetObject();
    result.AddMember("path", jsonString(pathWithinScene(access, contentsOnly ? parent : loadedRoot), allocator),
                     allocator);
    result.AddMember("nodesAdded", static_cast<int>(contentsOnly ? loadedChildren.size() : 1u), allocator);
    result.AddMember("warnings", jsonStringArray(warnings, allocator), allocator);
    return true;
}

bool handleDirectorPause(SceneAccess* access,
                          const rapidjson::Value& /*params*/,
                          rapidjson::Document& /*doc*/,
                          rapidjson::Value& result,
                          std::string& /*code*/,
                          std::string& /*message*/)
{
    access->setPaused(true);
    result.SetObject();
    return true;
}

bool handleDirectorResume(SceneAccess* access,
                           const rapidjson::Value& /*params*/,
                           rapidjson::Document& /*doc*/,
                           rapidjson::Value& result,
                           std::string& /*code*/,
                           std::string& /*message*/)
{
    access->setPaused(false);
    result.SetObject();
    return true;
}

bool dispatch(std::string_view method,
              const rapidjson::Value& params,
              SceneAccess* access,
              rapidjson::Document& doc,
              rapidjson::Value& result,
              std::string& code,
              std::string& message)
{
    if (method == "app.info")
        return handleAppInfo(access, params, doc, result, code, message);
    if (method == "scene.tree")
        return handleSceneTree(access, params, doc, result, code, message);
    if (method == "node.describe")
        return handleNodeDescribe(access, params, doc, result, code, message);
    if (method == "node.properties")
        return handleNodeProperties(access, params, doc, result, code, message);
    if (method == "node.get")
        return handleNodeGet(access, params, doc, result, code, message);
    if (method == "node.set")
        return handleNodeSet(access, params, doc, result, code, message);
    if (method == "node.create")
        return handleNodeCreate(access, params, doc, result, code, message);
    if (method == "node.delete")
        return handleNodeDelete(access, params, doc, result, code, message);
    if (method == "node.reparent")
        return handleNodeReparent(access, params, doc, result, code, message);
    if (method == "scene.types")
        return handleSceneTypes(access, params, doc, result, code, message);
    if (method == "scene.save")
        return handleSceneSave(access, params, doc, result, code, message);
    if (method == "scene.load")
        return handleSceneLoad(access, params, doc, result, code, message);
    if (method == "input.tap")
        return handleInputTap(access, params, doc, result, code, message);
    if (method == "input.swipe")
        return handleInputSwipe(access, params, doc, result, code, message);
    if (method == "screenshot")
        return handleScreenshot(access, params, doc, result, code, message);
    if (method == "director.pause")
        return handleDirectorPause(access, params, doc, result, code, message);
    if (method == "director.resume")
        return handleDirectorResume(access, params, doc, result, code, message);

    code    = "unknown_method";
    message = "unknown method: " + std::string(method);
    return false;
}

// -------------------------------------------------------------------------------------------
// Response envelope building.
// -------------------------------------------------------------------------------------------

std::string documentToString(rapidjson::Document& doc)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return std::string(buffer.GetString(), buffer.GetLength());
}

std::string buildError(rapidjson::Document& doc, const rapidjson::Value& id, std::string_view code, std::string_view message)
{
    auto& allocator = doc.GetAllocator();

    rapidjson::Value idCopy;
    idCopy.CopyFrom(id, allocator);
    doc.AddMember("id", idCopy, allocator);
    doc.AddMember("ok", false, allocator);

    rapidjson::Value errorObj(rapidjson::kObjectType);
    errorObj.AddMember("code", jsonString(code, allocator), allocator);
    errorObj.AddMember("message", jsonString(message, allocator), allocator);
    doc.AddMember("error", errorObj, allocator);

    return documentToString(doc);
}

std::string buildSuccess(rapidjson::Document& doc, const rapidjson::Value& id, rapidjson::Value& result)
{
    auto& allocator = doc.GetAllocator();

    rapidjson::Value idCopy;
    idCopy.CopyFrom(id, allocator);
    doc.AddMember("id", idCopy, allocator);
    doc.AddMember("ok", true, allocator);
    doc.AddMember("result", result, allocator);

    return documentToString(doc);
}

}  // namespace

AgentRequestHandler::AgentRequestHandler(SceneAccess* access) : _access(access) {}

std::string AgentRequestHandler::handle(std::string_view requestJson)
{
    rapidjson::Document responseDoc;
    responseDoc.SetObject();

    rapidjson::Document requestDoc;
    // Parse the (length-bounded, non-null-terminated) buffer directly rather than the in-situ
    // overload, since requestJson is a borrowed view this function has no right to mutate.
    requestDoc.Parse(requestJson.data(), requestJson.size());

    rapidjson::Value idValue;
    idValue.SetNull();

    if (requestDoc.HasParseError())
        return buildError(responseDoc, idValue, "parse_error", "malformed JSON");

    // Syntactically valid JSON that isn't an object (e.g. a bare array or number) parsed fine,
    // so this is a request-shape problem, not a parse problem.
    if (!requestDoc.IsObject())
        return buildError(responseDoc, idValue, "invalid_request", "request must be a JSON object");

    // "id" may be any JSON scalar, or absent (meaning null), and must be echoed back exactly -
    // including on every error path below - so it is captured before anything that can fail.
    if (auto it = requestDoc.FindMember("id"); it != requestDoc.MemberEnd())
        idValue.CopyFrom(it->value, responseDoc.GetAllocator());

    auto methodIt = requestDoc.FindMember("method");
    if (methodIt == requestDoc.MemberEnd() || !methodIt->value.IsString())
        return buildError(responseDoc, idValue, "invalid_request", "\"method\" must be a string");

    const std::string_view method(methodIt->value.GetString(), methodIt->value.GetStringLength());

    rapidjson::Value emptyParams(rapidjson::kObjectType);
    const rapidjson::Value* params = &emptyParams;
    if (auto it = requestDoc.FindMember("params"); it != requestDoc.MemberEnd())
    {
        if (!it->value.IsObject())
            return buildError(responseDoc, idValue, "invalid_params", "\"params\" must be an object");
        params = &it->value;
    }

    if (!_access)
        return buildError(responseDoc, idValue, "internal_error", "no SceneAccess configured");

    std::string errorCode, errorMessage;
    rapidjson::Value result(rapidjson::kObjectType);

    if (!dispatch(method, *params, _access, responseDoc, result, errorCode, errorMessage))
        return buildError(responseDoc, idValue, errorCode, errorMessage);

    return buildSuccess(responseDoc, idValue, result);
}

}  // namespace ax
