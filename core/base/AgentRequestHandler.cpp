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

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "base/NodeReflection.h"
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

// -------------------------------------------------------------------------------------------
// Small helpers around rapidjson: building strings/points/colors, and the JSON <-> PropertyValue
// codec described in the design doc §4 ("node.set value encoding mirrors PropertyType").
// -------------------------------------------------------------------------------------------

rapidjson::Value jsonString(std::string_view s, rapidjson::Document::AllocatorType& allocator)
{
    rapidjson::Value v;
    v.SetString(s.data(), static_cast<rapidjson::SizeType>(s.size()), allocator);
    return v;
}

rapidjson::Value vec2ToJson(const Vec2& v, rapidjson::Document::AllocatorType& allocator)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("x", v.x, allocator);
    obj.AddMember("y", v.y, allocator);
    return obj;
}

rapidjson::Value vec3ToJson(const Vec3& v, rapidjson::Document::AllocatorType& allocator)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("x", v.x, allocator);
    obj.AddMember("y", v.y, allocator);
    obj.AddMember("z", v.z, allocator);
    return obj;
}

rapidjson::Value colorToJson(const Color4B& c, rapidjson::Document::AllocatorType& allocator)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("r", static_cast<int>(c.r), allocator);
    obj.AddMember("g", static_cast<int>(c.g), allocator);
    obj.AddMember("b", static_cast<int>(c.b), allocator);
    obj.AddMember("a", static_cast<int>(c.a), allocator);
    return obj;
}

/// Reads {"x":..,"y":..} into `out`. Every member must be present and a JSON number - anything
/// else (missing member, string, object, ...) is a type mismatch, never coerced.
bool vec2FromJson(const rapidjson::Value& json, Vec2& out)
{
    if (!json.IsObject())
        return false;
    auto x = json.FindMember("x");
    auto y = json.FindMember("y");
    if (x == json.MemberEnd() || y == json.MemberEnd() || !x->value.IsNumber() || !y->value.IsNumber())
        return false;
    out = Vec2(x->value.GetFloat(), y->value.GetFloat());
    return true;
}

bool vec3FromJson(const rapidjson::Value& json, Vec3& out)
{
    if (!json.IsObject())
        return false;
    auto x = json.FindMember("x");
    auto y = json.FindMember("y");
    auto z = json.FindMember("z");
    if (x == json.MemberEnd() || y == json.MemberEnd() || z == json.MemberEnd() || !x->value.IsNumber() ||
        !y->value.IsNumber() || !z->value.IsNumber())
        return false;
    out = Vec3(x->value.GetFloat(), y->value.GetFloat(), z->value.GetFloat());
    return true;
}

/// Reads {"r":..,"g":..,"b":..,"a":..} into `out`. Every channel must be present and an integer
/// in [0,255] - the same range Color4B's uint8_t channels can hold; anything else is rejected
/// rather than clamped or truncated.
bool colorFromJson(const rapidjson::Value& json, Color4B& out)
{
    if (!json.IsObject())
        return false;
    auto r = json.FindMember("r");
    auto g = json.FindMember("g");
    auto b = json.FindMember("b");
    auto a = json.FindMember("a");
    if (r == json.MemberEnd() || g == json.MemberEnd() || b == json.MemberEnd() || a == json.MemberEnd())
        return false;

    auto channel = [](const rapidjson::Value& v, uint8_t& outChannel) {
        if (!v.IsInt())
            return false;
        const int n = v.GetInt();
        if (n < 0 || n > 255)
            return false;
        outChannel = static_cast<uint8_t>(n);
        return true;
    };

    return channel(r->value, out.r) && channel(g->value, out.g) && channel(b->value, out.b) &&
           channel(a->value, out.a);
}

const char* propertyTypeName(PropertyType type)
{
    switch (type)
    {
    case PropertyType::Bool:
        return "bool";
    case PropertyType::Int:
        return "int";
    case PropertyType::Float:
        return "float";
    case PropertyType::String:
        return "string";
    case PropertyType::Vec2:
        return "vec2";
    case PropertyType::Vec3:
        return "vec3";
    case PropertyType::Color:
        return "color";
    }
    return "unknown";
}

/// The PropertyType of whichever alternative `value` currently holds. PropertyValue's
/// alternatives mirror PropertyType 1:1 (see the comment on PropertyType in NodeReflection.h),
/// so this is a straight mapping rather than a lookup.
PropertyType propertyTypeOfValue(const PropertyValue& value)
{
    return std::visit(
        [](auto&& v) -> PropertyType {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
                return PropertyType::Bool;
            else if constexpr (std::is_same_v<T, int>)
                return PropertyType::Int;
            else if constexpr (std::is_same_v<T, float>)
                return PropertyType::Float;
            else if constexpr (std::is_same_v<T, std::string>)
                return PropertyType::String;
            else if constexpr (std::is_same_v<T, Vec2>)
                return PropertyType::Vec2;
            else if constexpr (std::is_same_v<T, Vec3>)
                return PropertyType::Vec3;
            else
                return PropertyType::Color;
        },
        value);
}

rapidjson::Value encodePropertyValue(const PropertyValue& value, rapidjson::Document::AllocatorType& allocator)
{
    return std::visit(
        [&](auto&& v) -> rapidjson::Value {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int> || std::is_same_v<T, float>)
                return rapidjson::Value(v);
            else if constexpr (std::is_same_v<T, std::string>)
                return jsonString(v, allocator);
            else if constexpr (std::is_same_v<T, Vec2>)
                return vec2ToJson(v, allocator);
            else if constexpr (std::is_same_v<T, Vec3>)
                return vec3ToJson(v, allocator);
            else
                return colorToJson(v, allocator);
        },
        value);
}

/// Decodes `json` into `out` according to `type`, the property's declared PropertyType. Returns
/// false - a type_mismatch to the caller - if `json`'s shape doesn't match `type` exactly; see
/// the design doc §4: "A number supplied for a Bool, or a string for a Vec2, is a type_mismatch
/// - never coerced." Int specifically requires a JSON integer literal (not e.g. 3.0) for the
/// same reason: silently truncating a double would itself be a coercion.
bool decodePropertyValue(const rapidjson::Value& json, PropertyType type, PropertyValue& out)
{
    switch (type)
    {
    case PropertyType::Bool:
        if (!json.IsBool())
            return false;
        out = json.GetBool();
        return true;
    case PropertyType::Int:
        if (!json.IsInt())
            return false;
        out = json.GetInt();
        return true;
    case PropertyType::Float:
        if (!json.IsNumber())
            return false;
        out = json.GetFloat();
        // A JSON number magnitude beyond FLT_MAX becomes +-Infinity through GetFloat(), and NaN
        // cannot be spelled in JSON at all but rapidjson does not itself forbid a caller from
        // constructing one - reject both here rather than let a non-finite value reach a node
        // property (and, transitively, anything that property drives - see the identical
        // rationale on getRequiredNumber() below, written for the same class of bug found via
        // input.swipe).
        if (!std::isfinite(std::get<float>(out)))
            return false;
        return true;
    case PropertyType::String:
        if (!json.IsString())
            return false;
        out = std::string(json.GetString(), json.GetStringLength());
        return true;
    case PropertyType::Vec2:
    {
        Vec2 v;
        if (!vec2FromJson(json, v))
            return false;
        out = v;
        return true;
    }
    case PropertyType::Vec3:
    {
        Vec3 v;
        if (!vec3FromJson(json, v))
            return false;
        out = v;
        return true;
    }
    case PropertyType::Color:
    {
        Color4B c;
        if (!colorFromJson(json, c))
            return false;
        out = c;
        return true;
    }
    }
    return false;
}

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
