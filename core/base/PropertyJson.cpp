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

#include "base/PropertyJson.h"

#include <cmath>
#include <type_traits>
#include <variant>

namespace ax
{

rapidjson::Value jsonString(std::string_view s, rapidjson::Document::AllocatorType& allocator)
{
    rapidjson::Value v;
    v.SetString(s.data(), static_cast<rapidjson::SizeType>(s.size()), allocator);
    return v;
}

rapidjson::Value jsonStringArray(const std::vector<std::string>& values,
                                 rapidjson::Document::AllocatorType& allocator)
{
    rapidjson::Value array(rapidjson::kArrayType);
    for (const auto& value : values)
        array.PushBack(jsonString(value, allocator), allocator);
    return array;
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
        // property (and, transitively, anything that property drives).
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

bool decodePropertyValueInferred(const rapidjson::Value& json, PropertyValue& out)
{
    if (json.IsBool())
    {
        out = json.GetBool();
        return true;
    }
    if (json.IsInt())
    {
        out = json.GetInt();
        return true;
    }
    if (json.IsNumber())
    {
        const float f = json.GetFloat();
        if (!std::isfinite(f))
            return false;
        out = f;
        return true;
    }
    if (json.IsString())
    {
        out = std::string(json.GetString(), json.GetStringLength());
        return true;
    }
    if (json.IsObject())
    {
        // Order matters: a colour object also has four members, so test the most specific shape
        // first. vec3 before vec2 for the same reason - {x,y,z} would otherwise decode as a vec2
        // and silently drop z.
        Color4B color;
        if (colorFromJson(json, color))
        {
            out = color;
            return true;
        }
        Vec3 v3;
        if (vec3FromJson(json, v3))
        {
            out = v3;
            return true;
        }
        Vec2 v2;
        if (vec2FromJson(json, v2))
        {
            out = v2;
            return true;
        }
    }
    return false;
}

}  // namespace ax
