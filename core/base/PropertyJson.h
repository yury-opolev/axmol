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
#include "base/NodeReflection.h"
#include "math/Color.h"
#include "math/Vec2.h"
#include "math/Vec3.h"

#include "rapidjson/document.h"

namespace ax
{

/** The single JSON encoding of reflected property values.

    WHY THIS IS ITS OWN COMPONENT. Two things now speak this encoding: the agent bridge, which
    sends property values over a socket, and SceneSerializer, which writes them into a scene file.
    Written twice they would drift - one side would start accepting a 3-element array for a Vec3,
    or clamping a colour channel instead of rejecting it - and a scene would then round-trip
    differently depending on which door it came through. Written once, a change to the encoding is
    a change to both by construction.

    The rules, unchanged from the bridge's original implementation: a value's JSON shape must
    match its declared PropertyType EXACTLY. A number supplied for a Bool, a string for a Vec2, or
    a fractional literal for an Int is a mismatch, never a coercion; colour channels must be
    integers in [0,255] rather than clamped; and a non-finite float is rejected outright, since it
    would otherwise propagate into whatever engine state the property drives. */

/** Builds a JSON string value that owns its bytes. */
AX_DLL rapidjson::Value jsonString(std::string_view s, rapidjson::Document::AllocatorType& allocator);

/** Builds a JSON array of strings that owns its bytes. Both scene.save and scene.load report a
    "warnings" array, and they must report it identically - including emitting an empty array
    rather than omitting the member, so a caller never has to tell "nothing to report" apart from
    "this build does not report". */
AX_DLL rapidjson::Value jsonStringArray(const std::vector<std::string>& values,
                                        rapidjson::Document::AllocatorType& allocator);

AX_DLL rapidjson::Value vec2ToJson(const Vec2& v, rapidjson::Document::AllocatorType& allocator);
AX_DLL rapidjson::Value vec3ToJson(const Vec3& v, rapidjson::Document::AllocatorType& allocator);
AX_DLL rapidjson::Value colorToJson(const Color4B& c, rapidjson::Document::AllocatorType& allocator);

/** Reads {"x":..,"y":..} into `out`. Every member must be present and a JSON number - anything
    else (missing member, string, object, ...) is a type mismatch, never coerced. */
AX_DLL bool vec2FromJson(const rapidjson::Value& json, Vec2& out);
AX_DLL bool vec3FromJson(const rapidjson::Value& json, Vec3& out);

/** Reads {"r":..,"g":..,"b":..,"a":..} into `out`. Every channel must be present and an integer
    in [0,255] - the same range Color4B's uint8_t channels can hold; anything else is rejected
    rather than clamped or truncated. */
AX_DLL bool colorFromJson(const rapidjson::Value& json, Color4B& out);

/** The wire/file name of a PropertyType, e.g. "vec2". */
AX_DLL const char* propertyTypeName(PropertyType type);

/** The PropertyType of whichever alternative `value` currently holds. */
AX_DLL PropertyType propertyTypeOfValue(const PropertyValue& value);

AX_DLL rapidjson::Value encodePropertyValue(const PropertyValue& value,
                                            rapidjson::Document::AllocatorType& allocator);

/** Decodes `json` into `out` according to `type`, the property's declared PropertyType. Returns
    false if `json`'s shape does not match `type` exactly. */
AX_DLL bool decodePropertyValue(const rapidjson::Value& json, PropertyType type, PropertyValue& out);

/** Decodes a value whose type is not known in advance, inferring it from the JSON shape: used
    when reading a scene file, where a creation parameter's type is whatever was written. Booleans,
    integers, doubles, strings, {x,y}, {x,y,z} and {r,g,b,a} map to the obvious alternatives.
    Returns false for anything else (arrays, null, mixed objects). */
AX_DLL bool decodePropertyValueInferred(const rapidjson::Value& json, PropertyValue& out);

}  // namespace ax
