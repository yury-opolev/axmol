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

#include "base/NodeReflection.h"

#include "platform/PlatformConfig.h"

#if __has_include(<cxxabi.h>)
#    define AX_HAS_CXXABI 1
#    include <cxxabi.h>
#endif

#include "2d/Label.h"
#include "2d/Node.h"
#include "2d/Sprite.h"
#if defined(AX_ENABLE_3D)
// Camera and Light live under core/2d despite being 3D concepts, but MeshRenderer does not - and
// core/3d is only compiled when AX_ENABLE_3D is on (core/CMakeLists.txt:190). This file is built
// unconditionally, so everything that touches those types stays behind this guard.
#    include "2d/Camera.h"
#    include "2d/Light.h"
#    include "3d/MeshRenderer.h"
#    include "3d/Ray.h"
#endif
#include "base/Protocols.h"
#include "platform/FileUtils.h"
#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <random>

namespace ax
{

namespace
{
std::atomic<NodeReflection*> g_instance{nullptr};

// ---------------------------------------------------------------------------------------------
// Built-in providers. These are the data-oriented equivalents of Inspector's three
// InspectPropertyHandler subclasses; they are kept private to this translation unit because
// section 4 of the design only specifies PropertyProvider (the interface) as public API.
// ---------------------------------------------------------------------------------------------

/// Properties common to every Node. Always supports() true, so it must be consulted last -
/// see the ordering comment on NodeReflection::_providers in the header.
class NodePropertyProvider : public PropertyProvider
{
public:
    bool supports(Node*) const override { return true; }

    std::vector<PropertyInfo> list() const override
    {
        return {
            {"position", PropertyType::Vec2, true},      {"contentSize", PropertyType::Vec2, true},
            {"anchorPoint", PropertyType::Vec2, true},    {"scaleX", PropertyType::Float, true},
            {"scaleY", PropertyType::Float, true},        {"rotation", PropertyType::Float, true},
            {"localZOrder", PropertyType::Int, true},     {"globalZOrder", PropertyType::Float, true},
            {"visible", PropertyType::Bool, true},        {"name", PropertyType::String, true},
            {"tag", PropertyType::Int, true},              {"color", PropertyType::Color, true},
            {"opacity", PropertyType::Int, true},
            // The 3D transform lives here, on the provider that supports EVERY node, rather than
            // behind AX_ENABLE_3D: Node carries this state whether or not core/3d is compiled, and
            // laying 2D sprites out at differing depths is an ordinary thing to want. Gating them
            // would make the property surface depend on a build option unrelated to them.
            {"position3D", PropertyType::Vec3, true},     {"rotation3D", PropertyType::Vec3, true},
            {"scaleZ", PropertyType::Float, true},
            // Exposed because a model loaded from a file is a TREE of nodes, not one node: a
            // loader builds a child per named object, so the node a caller holds may draw nothing
            // itself and "color" on it reaches nothing. Cascading is off by default, and without
            // it on the property surface a caller can set a colour, watch it do nothing, and have
            // no way to discover why - the governing switch was simply invisible.
            {"cascadeColor", PropertyType::Bool, true},   {"cascadeOpacity", PropertyType::Bool, true},
        };
    }

    bool get(Node* node, std::string_view name, PropertyValue& out) const override
    {
        if (name == "position")
            out = node->getPosition();
        else if (name == "contentSize")
            out = node->getContentSize();
        else if (name == "anchorPoint")
            out = node->getAnchorPoint();
        // Node::getScale() asserts when scaleX != scaleY, so scaleX/scaleY are always read
        // and written individually, never via getScale()/setScale().
        else if (name == "scaleX")
            out = node->getScaleX();
        else if (name == "scaleY")
            out = node->getScaleY();
        else if (name == "rotation")
            out = node->getRotation();
        else if (name == "position3D")
            out = node->getPosition3D();
        else if (name == "rotation3D")
            out = node->getRotation3D();
        else if (name == "scaleZ")
            out = node->getScaleZ();
        else if (name == "localZOrder")
            out = node->getLocalZOrder();
        else if (name == "globalZOrder")
            out = node->getGlobalZOrder();
        else if (name == "visible")
            out = node->isVisible();
        else if (name == "name")
            out = std::string(node->getName());
        else if (name == "tag")
            out = node->getTag();
        else if (name == "color")
            // Color carries opacity in the alpha channel; set() writes opacity back too so this
            // round-trips.
            out = Color4B(node->getColor(), node->getOpacity());
        else if (name == "opacity")
            out = static_cast<int>(node->getOpacity());
        else if (name == "cascadeColor")
            out = node->isCascadeColorEnabled();
        else if (name == "cascadeOpacity")
            out = node->isCascadeOpacityEnabled();
        else
            return false;
        return true;
    }

    bool set(Node* node, std::string_view name, const PropertyValue& value) const override
    {
        if (name == "position")
        {
            if (!std::holds_alternative<Vec2>(value))
                return false;
            node->setPosition(std::get<Vec2>(value));
        }
        else if (name == "contentSize")
        {
            if (!std::holds_alternative<Vec2>(value))
                return false;
            node->setContentSize(std::get<Vec2>(value));
        }
        else if (name == "anchorPoint")
        {
            if (!std::holds_alternative<Vec2>(value))
                return false;
            node->setAnchorPoint(std::get<Vec2>(value));
        }
        else if (name == "scaleX")
        {
            if (!std::holds_alternative<float>(value))
                return false;
            node->setScaleX(std::get<float>(value));
        }
        else if (name == "scaleY")
        {
            if (!std::holds_alternative<float>(value))
                return false;
            node->setScaleY(std::get<float>(value));
        }
        else if (name == "rotation")
        {
            if (!std::holds_alternative<float>(value))
                return false;
            node->setRotation(std::get<float>(value));
        }
        else if (name == "position3D")
        {
            if (!std::holds_alternative<Vec3>(value))
                return false;
            node->setPosition3D(std::get<Vec3>(value));
        }
        else if (name == "rotation3D")
        {
            if (!std::holds_alternative<Vec3>(value))
                return false;
            node->setRotation3D(std::get<Vec3>(value));
        }
        else if (name == "scaleZ")
        {
            if (!std::holds_alternative<float>(value))
                return false;
            node->setScaleZ(std::get<float>(value));
        }
        else if (name == "localZOrder")
        {
            if (!std::holds_alternative<int>(value))
                return false;
            node->setLocalZOrder(std::get<int>(value));
        }
        else if (name == "globalZOrder")
        {
            if (!std::holds_alternative<float>(value))
                return false;
            node->setGlobalZOrder(std::get<float>(value));
        }
        else if (name == "visible")
        {
            if (!std::holds_alternative<bool>(value))
                return false;
            node->setVisible(std::get<bool>(value));
        }
        else if (name == "name")
        {
            if (!std::holds_alternative<std::string>(value))
                return false;
            node->setName(std::get<std::string>(value));
        }
        else if (name == "tag")
        {
            if (!std::holds_alternative<int>(value))
                return false;
            node->setTag(std::get<int>(value));
        }
        else if (name == "color")
        {
            if (!std::holds_alternative<Color4B>(value))
                return false;
            // Color carries opacity in the alpha channel (see get(), which reads
            // Color4B(getColor(), getOpacity())), so writing it must also write opacity for
            // get()/set() to round-trip - otherwise alpha would be silently discarded.
            const auto& c = std::get<Color4B>(value);
            node->setColor(Color3B(c));
            node->setOpacity(c.a);
        }
        else if (name == "opacity")
        {
            if (!std::holds_alternative<int>(value))
                return false;
            // Reject out-of-range values rather than silently truncating them into uint8_t -
            // e.g. 300 must not coerce to 44.
            const int opacity = std::get<int>(value);
            if (opacity < 0 || opacity > 255)
                return false;
            node->setOpacity(static_cast<uint8_t>(opacity));
        }
        else if (name == "cascadeColor")
        {
            if (!std::holds_alternative<bool>(value))
                return false;
            node->setCascadeColorEnabled(std::get<bool>(value));
        }
        else if (name == "cascadeOpacity")
        {
            if (!std::holds_alternative<bool>(value))
                return false;
            node->setCascadeOpacityEnabled(std::get<bool>(value));
        }
        else
        {
            return false;
        }
        return true;
    }
};

/// Sprite-only properties. texturePath is deliberately read-only: a live Sprite's texture is
/// set through Sprite::setTexture()/initWithFile(), not through a bare path string.
/// Turns an absolute, already-resolved asset path back into the resource-relative name that would
/// find it again.
///
/// WHY THIS IS NEEDED. Texture2D::getPath() returns what Image stored, and Image sets it from
/// FileUtils::fullPathForFilename() - i.e. the FULLY RESOLVED path on this machine, not the name
/// the caller passed to Sprite::create(). Reporting that verbatim would be merely ugly for an
/// inspector, but "texturePath" is also what SceneSerializer records so a sprite can be rebuilt:
/// a scene file carrying "C:/Users/someone/.../Content/hero.png" is committed to the repository
/// and then fails to load on every other machine, on Android, and in any packaged build. Stripping
/// the resource roots restores "hero.png", which is portable and is what the file should say.
std::string asResourceRelativePath(std::string_view absolute)
{
    std::string path(absolute);
    if (path.empty())
        return path;

    // Compare on forward slashes: the search paths and the resolved path can disagree about
    // separators on Windows.
    auto normalise = [](std::string text) {
        std::replace(text.begin(), text.end(), '\\', '/');
        return text;
    };
    path = normalise(std::move(path));

    auto* fileUtils = FileUtils::getInstance();

    // Longest prefix wins: search paths are often nested under the resource root, and stripping
    // the shorter one first would leave a stray directory component behind.
    std::string bestPrefix;
    auto consider = [&](std::string_view candidate) {
        if (candidate.empty())
            return;
        std::string prefix = normalise(std::string(candidate));
        if (prefix.back() != '/')
            prefix += '/';
        if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
            prefix.size() > bestPrefix.size())
            bestPrefix = std::move(prefix);
    };

    for (const auto& searchPath : fileUtils->getSearchPaths())
        consider(searchPath);
    for (const auto& searchPath : fileUtils->getOriginalSearchPaths())
        consider(searchPath);
    consider(fileUtils->getDefaultResourceRootPath());
    consider(fileUtils->getWritablePath());

    if (!bestPrefix.empty())
        path.erase(0, bestPrefix.size());
    return path;
}

/// A correlation handle for a node, stable within one process run.
///
/// NOT the address. NodeInfo::id used to be the raw pointer, which every scene.tree response then
/// handed to whoever was on the other end of the bridge - a live heap address per node, i.e. an
/// ASLR defeat handed out for free, and exactly the missing half of an exploit for any
/// use-after-free elsewhere. Mixing against a per-run salt keeps the only property the id is
/// documented to have (the same node compares equal within a session) and discards the one it
/// should never have had.
///
/// The mixing is written out by hand rather than left to std::hash. std::hash<T*> is the IDENTITY
/// function in libstdc++ and libc++ - only MSVC's STL actually hashes it - so `std::hash<T*>{}(p) ^
/// salt` degrades to `addr ^ salt` on Android, Linux and macOS. That is not obfuscation: any two
/// ids XOR to the exact distance between their nodes on the heap, and one node whose address is
/// known by other means recovers the salt and with it every address in the process. splitmix64 is
/// a real finalizer, so a single id reveals nothing and pairs of ids reveal nothing.
std::string opaqueNodeId(const Node* node)
{
    // 64-bit regardless of pointer width: a 32-bit target (armeabi-v7a) must not end up with a
    // 32-bit salt, and shifting a 32-bit size_t left by 32 would be undefined anyway.
    static const uint64_t salt = [] {
        std::random_device device;
        return (static_cast<uint64_t>(device()) << 32) ^ static_cast<uint64_t>(device());
    }();

    uint64_t mixed = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(node)) + salt;
    mixed          = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
    mixed          = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
    mixed          = mixed ^ (mixed >> 31);
    return fmt::format("{:016x}", mixed);
}

class SpritePropertyProvider : public PropertyProvider
{
public:
    bool supports(Node* node) const override { return dynamic_cast<Sprite*>(node) != nullptr; }

    std::vector<PropertyInfo> list() const override
    {
        return {
            {"flippedX", PropertyType::Bool, true},
            {"flippedY", PropertyType::Bool, true},
            {"texturePath", PropertyType::String, false},
        };
    }

    bool get(Node* node, std::string_view name, PropertyValue& out) const override
    {
        auto* sprite = dynamic_cast<Sprite*>(node);
        if (!sprite)
            return false;

        if (name == "flippedX")
            out = sprite->isFlippedX();
        else if (name == "flippedY")
            out = sprite->isFlippedY();
        else if (name == "texturePath")
        {
            auto* texture = sprite->getTexture();
            out           = texture ? asResourceRelativePath(texture->getPath()) : std::string();
        }
        else
            return false;
        return true;
    }

    bool set(Node* node, std::string_view name, const PropertyValue& value) const override
    {
        auto* sprite = dynamic_cast<Sprite*>(node);
        if (!sprite)
            return false;

        // texturePath is read-only - fall through to the final `return false` for it as well as
        // for unknown names, rather than special-casing it.
        if (name == "flippedX")
        {
            if (!std::holds_alternative<bool>(value))
                return false;
            sprite->setFlippedX(std::get<bool>(value));
            return true;
        }
        if (name == "flippedY")
        {
            if (!std::holds_alternative<bool>(value))
                return false;
            sprite->setFlippedY(std::get<bool>(value));
            return true;
        }
        return false;
    }
};

/// Properties for anything implementing LabelProtocol (Label, LabelAtlas, LabelBMFont, ...).
class LabelPropertyProvider : public PropertyProvider
{
public:
    bool supports(Node* node) const override { return dynamic_cast<LabelProtocol*>(node) != nullptr; }

    std::vector<PropertyInfo> list() const override
    {
        return {
            {"text", PropertyType::String, true},
            // Read-only, and reported for ax::Label only (see get()). A label's font is fixed at
            // construction - Label::createWithSystemFont / createWithTTF - so these exist to be
            // READ BACK: they are exactly what NodeFactory needs to rebuild the label, and
            // without them a serialized label reloads in the platform default font.
            {"fontName", PropertyType::String, false},
            {"fontSize", PropertyType::Float, false},
        };
    }

    bool get(Node* node, std::string_view name, PropertyValue& out) const override
    {
        auto* label = dynamic_cast<LabelProtocol*>(node);
        if (!label)
            return false;

        if (name == "text")
        {
            out = std::string(label->getString());
            return true;
        }

        // The font accessors live on ax::Label, not on the LabelProtocol this provider supports
        // broadly, so anything else (LabelAtlas, LabelBMFont) simply does not expose them.
        auto* concrete = dynamic_cast<Label*>(node);
        if (!concrete)
            return false;

        const bool isTTF = concrete->getLabelType() == Label::LabelType::TTF;
        if (name == "fontName")
        {
            out = isTTF ? concrete->getTTFConfig().fontFilePath : std::string(concrete->getSystemFontName());
            return true;
        }
        if (name == "fontSize")
        {
            out = isTTF ? concrete->getTTFConfig().fontSize : concrete->getSystemFontSize();
            return true;
        }
        return false;
    }

    bool set(Node* node, std::string_view name, const PropertyValue& value) const override
    {
        auto* label = dynamic_cast<LabelProtocol*>(node);
        if (!label || name != "text" || !std::holds_alternative<std::string>(value))
            return false;

        label->setString(std::get<std::string>(value));
        return true;
    }
};

#if defined(AX_ENABLE_3D)

/// Mesh-specific properties. The model itself is absent on purpose: a MeshRenderer's geometry is
/// fixed at construction, so it is a NodeFactory creation parameter rather than a property, for
/// the same reason a Sprite's texture is. texturePath is exposed read-only so the serializer can
/// capture it without implying it can be reassigned.
class MeshRendererPropertyProvider : public PropertyProvider
{
public:
    bool supports(Node* node) const override { return dynamic_cast<MeshRenderer*>(node) != nullptr; }

    std::vector<PropertyInfo> list() const override
    {
        return {
            // Read-only for the same reason ax::Sprite's texturePath is: geometry is fixed at
            // construction, so assigning here could not remodel a live renderer. It is exposed at
            // all because NodeFactory declares it a creation parameter, and the serializer
            // captures those by READING THEM BACK as properties (SceneSerializer.cpp:130-135) -
            // a declared parameter no provider can read is silently dropped, producing a file that
            // saves without complaint and cannot be reloaded.
            {"modelPath", PropertyType::String, false},
            {"texturePath", PropertyType::String, false},
            {"lightMask", PropertyType::Int, true},
            // How many meshes this renderer draws ITSELF. Read-only, and worth its place: a model
            // with several named objects is loaded as a tree of child MeshRenderers, so the root
            // frequently draws nothing and reports 0 here. That number is the difference between
            // "my colour is being ignored" and "I am talking to the wrong node", and without it
            // the two are indistinguishable from outside the process.
            {"meshCount", PropertyType::Int, false},
            // World-space bounds of this renderer AND ITS CHILDREN, via getAABBRecursively().
            //
            // Recursive on purpose. getAABB() covers only the meshes a node owns directly, so on
            // the tree root of a multi-object model - which owns none - it returns AABB::reset()'s
            // sentinel: min +99999, max -99999. That is not an error value anything checks, so a
            // caller scaling itself from those bounds silently does nothing, which is exactly how
            // an auto-fit here ended up drawing a model one pixel wide.
            //
            // World space, not model space: the transform is already applied, so these answer
            // "where is this on screen and how big" - the question layout, framing and hit-testing
            // actually ask. Model-space extents would need the scale undone by every caller.
            {"boundsMin", PropertyType::Vec3, false},     {"boundsMax", PropertyType::Vec3, false},
        };
    }

    bool get(Node* node, std::string_view name, PropertyValue& out) const override
    {
        auto* mesh = dynamic_cast<MeshRenderer*>(node);
        if (!mesh)
            return false;

        if (name == "modelPath")
        {
            out = std::string(mesh->getModelPath());
            return true;
        }
        if (name == "texturePath")
        {
            // Mirrors SpritePropertyProvider: the path a texture came from is recoverable only
            // from the texture itself. MeshRenderer has no getTexture() - a model can carry
            // several meshes, each with its own material - so this reports the first mesh's
            // diffuse texture, which is what a single-texture model (the case the factory's
            // texturePath parameter covers) has.
            //
            // The count is checked FIRST because getMesh() is _meshes.at(0), which THROWS on an
            // empty renderer - and the root of a multi-object model is exactly that. Serialization
            // reads texturePath for every MeshRenderer it walks, so without this guard saving any
            // scene containing such a model failed outright with an out-of-range error from deep
            // inside the engine.
            auto* firstMesh = mesh->getMeshCount() > 0 ? mesh->getMesh() : nullptr;
            // The USAGE overload, not the no-argument one. Mesh::getTexture() const is
            // `_textures.at(Diffuse)` - a std::map::at that THROWS when the mesh has no diffuse
            // texture, which is every mesh of an untextured model. Reading a property must not
            // throw, and this one is read for every MeshRenderer the serializer walks, so the
            // no-argument form made saving any scene containing an untextured model fail with
            // "invalid map<K, T> key" from deep inside the engine. The usage overload is
            // operator[], which yields null instead.
            auto* texture = firstMesh ? firstMesh->getTexture(NTextureData::Usage::Diffuse) : nullptr;
            out           = texture ? std::string(texture->getPath()) : std::string();
            return true;
        }
        if (name == "lightMask")
        {
            out = static_cast<int>(mesh->getLightMask());
            return true;
        }
        if (name == "meshCount")
        {
            out = static_cast<int>(mesh->getMeshCount());
            return true;
        }
        if (name == "boundsMin" || name == "boundsMax")
        {
            const auto bounds = mesh->getAABBRecursively();
            out               = (name == "boundsMin") ? bounds._min : bounds._max;
            return true;
        }
        return false;
    }

    bool set(Node* node, std::string_view name, const PropertyValue& value) const override
    {
        auto* mesh = dynamic_cast<MeshRenderer*>(node);
        if (!mesh || name != "lightMask" || !std::holds_alternative<int>(value))
            return false;

        mesh->setLightMask(static_cast<unsigned int>(std::get<int>(value)));
        return true;
    }
};

/// Camera properties. Field of view is deliberately NOT here: Camera fixes it in
/// createPerspective and offers no setter, and a property that silently fails to apply is worse
/// than an absent one - the caller sees success and a frame that did not change.
class CameraPropertyProvider : public PropertyProvider
{
public:
    bool supports(Node* node) const override { return dynamic_cast<Camera*>(node) != nullptr; }

    std::vector<PropertyInfo> list() const override
    {
        return {
            // Read-only, and present only so the serializer can capture it: Camera fixes the fov
            // in createPerspective and has no setter, but NodeFactory declares it a creation
            // parameter, and an unreadable creation parameter is silently dropped at save time
            // (SceneSerializer.cpp:130-135) - the camera would then reload at the default 60
            // degrees while the file claimed to have round-tripped.
            {"fieldOfView", PropertyType::Float, false},
            {"nearPlane", PropertyType::Float, true},
            {"farPlane", PropertyType::Float, true},
            {"depth", PropertyType::Int, true},
            {"cameraFlag", PropertyType::Int, true},
        };
    }

    bool get(Node* node, std::string_view name, PropertyValue& out) const override
    {
        auto* camera = dynamic_cast<Camera*>(node);
        if (!camera)
            return false;

        if (name == "fieldOfView")
            out = camera->getFOV();
        else if (name == "nearPlane")
            out = camera->getNearPlane();
        else if (name == "farPlane")
            out = camera->getFarPlane();
        else if (name == "depth")
            out = static_cast<int>(camera->getDepth());
        else if (name == "cameraFlag")
            out = static_cast<int>(camera->getCameraFlag());
        else
            return false;
        return true;
    }

    bool set(Node* node, std::string_view name, const PropertyValue& value) const override
    {
        auto* camera = dynamic_cast<Camera*>(node);
        if (!camera)
            return false;

        if (name == "nearPlane")
        {
            if (!std::holds_alternative<float>(value))
                return false;
            camera->setNearPlane(std::get<float>(value));
        }
        else if (name == "farPlane")
        {
            if (!std::holds_alternative<float>(value))
                return false;
            camera->setFarPlane(std::get<float>(value));
        }
        else if (name == "depth")
        {
            if (!std::holds_alternative<int>(value))
                return false;
            camera->setDepth(static_cast<int8_t>(std::get<int>(value)));
        }
        else if (name == "cameraFlag")
        {
            if (!std::holds_alternative<int>(value))
                return false;
            camera->setCameraFlag(static_cast<CameraFlag>(std::get<int>(value)));
        }
        else
        {
            return false;
        }
        return true;
    }
};

/// Light properties. Colour is absent because BaseLight derives from Node, so the base provider's
/// "color" already reads and writes it - adding a second name for one piece of state would give
/// an agent two ways to ask the same question and no way to know they are the same.
///
/// Range and direction are declared for every light even though they apply only to some: a
/// property list is per-TYPE, not per-instance, and get/set return false on a light that has no
/// such concept, which is the same answer an unknown name gets.
class LightPropertyProvider : public PropertyProvider
{
public:
    bool supports(Node* node) const override { return dynamic_cast<BaseLight*>(node) != nullptr; }

    std::vector<PropertyInfo> list() const override
    {
        return {
            {"intensity", PropertyType::Float, true},
            {"range", PropertyType::Float, true},
            {"direction", PropertyType::Vec3, true},
        };
    }

    bool get(Node* node, std::string_view name, PropertyValue& out) const override
    {
        auto* light = dynamic_cast<BaseLight*>(node);
        if (!light)
            return false;

        if (name == "intensity")
        {
            out = light->getIntensity();
            return true;
        }
        if (name == "range")
        {
            if (auto* point = dynamic_cast<PointLight*>(node))
            {
                out = point->getRange();
                return true;
            }
            if (auto* spot = dynamic_cast<SpotLight*>(node))
            {
                out = spot->getRange();
                return true;
            }
            return false;
        }
        if (name == "direction")
        {
            if (auto* directional = dynamic_cast<DirectionLight*>(node))
            {
                out = directional->getDirection();
                return true;
            }
            if (auto* spot = dynamic_cast<SpotLight*>(node))
            {
                out = spot->getDirection();
                return true;
            }
            return false;
        }
        return false;
    }

    bool set(Node* node, std::string_view name, const PropertyValue& value) const override
    {
        auto* light = dynamic_cast<BaseLight*>(node);
        if (!light)
            return false;

        if (name == "intensity")
        {
            if (!std::holds_alternative<float>(value))
                return false;
            light->setIntensity(std::get<float>(value));
            return true;
        }
        if (name == "range")
        {
            if (!std::holds_alternative<float>(value))
                return false;
            if (auto* point = dynamic_cast<PointLight*>(node))
            {
                point->setRange(std::get<float>(value));
                return true;
            }
            if (auto* spot = dynamic_cast<SpotLight*>(node))
            {
                spot->setRange(std::get<float>(value));
                return true;
            }
            return false;
        }
        if (name == "direction")
        {
            if (!std::holds_alternative<Vec3>(value))
                return false;
            if (auto* directional = dynamic_cast<DirectionLight*>(node))
            {
                directional->setDirection(std::get<Vec3>(value));
                return true;
            }
            if (auto* spot = dynamic_cast<SpotLight*>(node))
            {
                spot->setDirection(std::get<Vec3>(value));
                return true;
            }
            return false;
        }
        return false;
    }
};

#endif  // AX_ENABLE_3D

/// Appends the child-index path from `current` down to `target` (exclusive of `current`) onto
/// `path`, backtracking on failed branches. Used by NodeReflection::pathOf.
bool appendPathToDescendant(Node* current, Node* target, std::string& path)
{
    // Paths are indices into sorted (render) order, so the tree must be sorted before it is
    // indexed - otherwise a path computed here could point at a different sibling once
    // sortAllChildren() runs on the next visit() (see the class doc comment in the header).
    current->sortAllChildren();
    auto& children = current->getChildren();
    for (ssize_t i = 0; i < children.size(); ++i)
    {
        auto* child = children.at(i);
        if (!child)
            continue;

        const auto restorePoint = path.size();
        path += '/';
        path += std::to_string(i);

        if (child == target || appendPathToDescendant(child, target, path))
            return true;

        path.resize(restorePoint);
    }
    return false;
}

}  // namespace

NodeReflection::NodeReflection()
{
    // addProvider() inserts at the front of _providers, so registration order here is the
    // REVERSE of consultation order - see the ordering comment on _providers in the header.
    // Registering Node, then Label, then Sprite leaves Sprite, Label, Node as the effective
    // order: specific providers are consulted before the general one.
    addProvider("__NODE__", std::make_unique<NodePropertyProvider>());
    addProvider("__LABEL__", std::make_unique<LabelPropertyProvider>());
    addProvider("__SPRITE__", std::make_unique<SpritePropertyProvider>());
#if defined(AX_ENABLE_3D)
    // Registered after the 2D built-ins, so they are consulted BEFORE them. None of their names
    // collide today, but the ordering is what makes a 3D-specific name win if one ever does.
    addProvider("__MESH__", std::make_unique<MeshRendererPropertyProvider>());
    addProvider("__CAMERA__", std::make_unique<CameraPropertyProvider>());
    addProvider("__LIGHT__", std::make_unique<LightPropertyProvider>());
#endif
}

NodeReflection* NodeReflection::getInstance()
{
    // A compare-exchange rather than std::call_once. With call_once, destroyInstance() left the
    // flag set, so the next getInstance() returned the null pointer it had just stored - and every
    // caller dereferences the result unconditionally. Destroy-then-use is exactly what a test that
    // wants a clean registry does, so the pair has to actually work; a once-flag cannot express
    // "again".
    //
    // The class is reached only from the Axmol main thread, so this is belt-and-braces. It is
    // cheap belt-and-braces, and an invariant enforced by a comment is one that a future caller on
    // the bridge thread breaks silently.
    auto* existing = g_instance.load(std::memory_order_acquire);
    if (existing)
        return existing;

    auto* created = new NodeReflection();
    if (g_instance.compare_exchange_strong(existing, created, std::memory_order_acq_rel,
                                           std::memory_order_acquire))
    {
        return created;
    }

    // Someone else won the race; theirs is the one everybody must see.
    delete created;
    return existing;
}

void NodeReflection::destroyInstance()
{
    delete g_instance.exchange(nullptr, std::memory_order_acq_rel);
}

#if defined(_MSC_VER)

std::string NodeReflection::demangle(const char* name)
{
    // MSVC's typeid().name() returns an undecorated name prefixed with "class " or "struct ",
    // e.g. typeid(Node).name() == "class ax::Node" but typeid(SomeStruct).name() == "struct
    // ax::SomeStruct". This guard is on the COMPILER (_MSC_VER), not the platform: MinGW/GCC
    // targeting Win32 produces Itanium-mangled names and must fall through to the cxxabi branch
    // below instead.
    using namespace std::string_view_literals;
    std::string_view view = name;
    if (view.substr(0, 6) == "class "sv)
        view.remove_prefix(6);
    else if (view.substr(0, 7) == "struct "sv)
        view.remove_prefix(7);
    return std::string(view);
}

#elif AX_HAS_CXXABI

std::string NodeReflection::demangle(const char* mangled_name)
{
    int status = -4;
    std::unique_ptr<char, void (*)(void*)> res{abi::__cxa_demangle(mangled_name, nullptr, nullptr, &status),
                                                 std::free};
    return (status == 0) ? res.get() : mangled_name;
}

#else

std::string NodeReflection::demangle(const char* name)
{
    return {name};
}

#endif

std::string NodeReflection::getTypeName(Node* node)
{
    if (!node)
        return "";
    return demangle(typeid(*node).name());
}

NodeInfo NodeReflection::describe(Node* node, std::string_view path) const
{
    NodeInfo info;
    if (!node)
        return info;

    info.id          = opaqueNodeId(node);
    info.path         = std::string(path);
    info.typeName     = getTypeName(node);
    info.name         = std::string(node->getName());
    info.tag          = node->getTag();
    info.childCount   = static_cast<int>(node->getChildrenCount());
    info.position      = node->getPosition();
    info.contentSize   = node->getContentSize();
    info.anchorPoint   = node->getAnchorPoint();
    // Node::getScale() asserts when scaleX != scaleY - always go through the per-axis getters.
    info.scaleX        = node->getScaleX();
    info.scaleY        = node->getScaleY();
    info.rotation       = node->getRotation();
    // Depth, so a tree of nodes differing only in Z does not come back looking like a pile at one
    // point. See the comment on NodeInfo::positionZ for why this is a scalar and not a Vec3.
    info.positionZ      = node->getPositionZ();
    info.scaleZ         = node->getScaleZ();
    info.rotation3D     = node->getRotation3D();
    info.localZOrder    = node->getLocalZOrder();
    info.globalZOrder   = node->getGlobalZOrder();
    info.visible         = node->isVisible();
    info.color           = Color4B(node->getColor(), node->getOpacity());
    return info;
}

void NodeReflection::describeTree(Node* node, int maxDepth, int depth, const std::string& path,
                                   std::vector<NodeInfo>& out) const
{
    out.push_back(describe(node, path));

    if (maxDepth >= 0 && depth >= maxDepth)
        return;

    // Sort before indexing so the reported paths are indices into render order - see the class
    // doc comment in the header on why paths must be stable against sortAllChildren().
    node->sortAllChildren();
    auto& children = node->getChildren();
    for (ssize_t i = 0; i < children.size(); ++i)
    {
        auto* child = children.at(i);
        if (!child)
            continue;

        const std::string childPath = (path == "/") ? fmt::format("/{}", i) : fmt::format("{}/{}", path, i);
        describeTree(child, maxDepth, depth + 1, childPath, out);
    }
}

std::vector<NodeInfo> NodeReflection::describeTree(Node* root, int maxDepth) const
{
    std::vector<NodeInfo> result;
    if (!root)
        return result;

    describeTree(root, maxDepth, 0, "/", result);
    return result;
}

Node* NodeReflection::resolve(Node* root, std::string_view path) const
{
    if (!root || path.empty() || path.front() != '/')
        return nullptr;

    if (path == "/")
        return root;

    Node* current = root;
    size_t pos    = 1;  // skip the leading '/'
    while (pos <= path.size())
    {
        const size_t next          = path.find('/', pos);
        const std::string_view segment = (next == std::string_view::npos) ? path.substr(pos) : path.substr(pos, next - pos);

        // An empty segment (trailing or doubled slash) and any non-digit character are both
        // malformed; requiring every character to be a digit also rejects a leading '-', which
        // std::from_chars below would otherwise happily accept as a negative int.
        if (segment.empty())
            return nullptr;
        for (char c : segment)
        {
            if (c < '0' || c > '9')
                return nullptr;
        }

        // Parse into a bounded int rather than accumulating by hand: an unbounded accumulation
        // (e.g. into ssize_t) wraps around on a long enough digit string, so an out-of-range
        // path like "/18446744073709551616" would silently wrap to a small, in-range index
        // instead of being rejected. from_chars reports overflow via ec instead of wrapping, and
        // requiring the whole segment to be consumed rejects any trailing garbage.
        int index          = 0;
        const auto [ptr, ec] = std::from_chars(segment.data(), segment.data() + segment.size(), index);
        if (ec != std::errc() || ptr != segment.data() + segment.size() || index < 0)
            return nullptr;

        // Sort before indexing so the index means the same thing visit() would draw - see the
        // class doc comment in the header on why paths must be stable against sortAllChildren().
        current->sortAllChildren();
        auto& children = current->getChildren();
        if (static_cast<size_t>(index) >= children.size())
            return nullptr;

        current = children.at(index);
        if (!current)
            return nullptr;

        if (next == std::string_view::npos)
            break;
        pos = next + 1;
    }

    return current;
}

std::string NodeReflection::pathOf(Node* root, Node* node) const
{
    if (!root || !node)
        return "";

    if (root == node)
        return "/";

    std::string path;
    if (appendPathToDescendant(root, node, path))
        return path;
    return "";
}

std::vector<PropertyInfo> NodeReflection::listProperties(Node* node) const
{
    std::vector<PropertyInfo> result;
    if (!node)
        return result;

    for (const auto& entry : _providers)
    {
        if (!entry.second->supports(node))
            continue;
        auto properties = entry.second->list();
        result.insert(result.end(), std::make_move_iterator(properties.begin()), std::make_move_iterator(properties.end()));
    }
    return result;
}

bool NodeReflection::getProperty(Node* node, std::string_view name, PropertyValue& out) const
{
    if (!node)
        return false;

    // Read into a local first and only publish it to `out` on success, so a misbehaving
    // third-party provider that writes `out` before returning false (an overall failed lookup)
    // can't clobber the caller's value - this is enforced structurally rather than merely
    // documented, since NodeReflection cannot otherwise guarantee providers behave.
    PropertyValue tmp;
    for (const auto& entry : _providers)
    {
        if (entry.second->supports(node) && entry.second->get(node, name, tmp))
        {
            out = std::move(tmp);
            return true;
        }
    }
    return false;
}

bool NodeReflection::setProperty(Node* node, std::string_view name, const PropertyValue& value)
{
    if (!node)
        return false;

    for (const auto& entry : _providers)
    {
        if (entry.second->supports(node) && entry.second->set(node, name, value))
            return true;
    }
    return false;
}

#if defined(AX_ENABLE_3D)

namespace
{
/// Walks `node` and its descendants, keeping the nearest MeshRenderer the ray passes through.
///
/// Nearest by ray distance rather than by draw order or tree order: with a perspective camera the
/// thing in front is what a person means by "the one I clicked", and tree order says nothing about
/// depth.
void pickNearest(Node* node,
                 const Ray& ray,
                 const std::string& path,
                 const NodeReflection& reflection,
                 PickHit& best,
                 bool& found)
{
    auto* mesh = dynamic_cast<MeshRenderer*>(node);
    // meshCount > 0 filters out the roots of multi-object models, which own no geometry. Testing
    // them would report a hit on a node that draws nothing and hide the piece actually under the
    // cursor behind it.
    if (mesh && mesh->getMeshCount() > 0)
    {
        float distance = 0.0f;
        if (ray.intersects(mesh->getAABB(), &distance) && (!found || distance < best.distance))
        {
            best.path     = path;
            best.typeName = NodeReflection::getTypeName(node);
            best.name     = std::string(node->getName());
            best.distance = distance;
            found         = true;
        }
    }

    // Sorted, so the indices in the path mean what resolve() and describeTree() mean by them.
    node->sortAllChildren();
    const auto& children = node->getChildren();
    for (ssize_t index = 0; index < static_cast<ssize_t>(children.size()); ++index)
    {
        const auto childPath = (path == "/") ? "/" + std::to_string(index) : path + "/" + std::to_string(index);
        pickNearest(children.at(index), ray, childPath, reflection, best, found);
    }
}
}  // namespace

bool NodeReflection::pick(Node* root, Camera* camera, const Vec2& screenPoint, const Vec2& viewSize,
                          PickHit& out) const
{
    if (!root || !camera || viewSize.x <= 0.0f || viewSize.y <= 0.0f)
    {
        return false;
    }

    // unprojectGL wants GL screen space, whose origin is BOTTOM-left, while screenPoint arrives
    // top-left to match input.tap. Flipping here rather than at the call site keeps the bridge's
    // one convention intact; letting the two disagree mirrors every pick vertically, which looks
    // plausible on a symmetric board and is maddening to track down.
    const Vec3 nearPoint(screenPoint.x, viewSize.y - screenPoint.y, -1.0f);
    const Vec3 farPoint(screenPoint.x, viewSize.y - screenPoint.y, 1.0f);

    Vec3 nearWorld;
    Vec3 farWorld;
    camera->unprojectGL(viewSize, &nearPoint, &nearWorld);
    camera->unprojectGL(viewSize, &farPoint, &farWorld);

    Vec3 direction = farWorld - nearWorld;
    if (direction.lengthSquared() <= 0.0f)
    {
        // A degenerate projection - an orthographic camera with a zero depth range, say - would
        // otherwise normalise to NaN and make every intersection test quietly false.
        return false;
    }
    direction.normalize();

    Ray ray;
    ray._origin    = nearWorld;
    ray._direction = direction;

    PickHit best;
    bool found = false;
    pickNearest(root, ray, "/", *this, best, found);
    if (found)
    {
        out = best;
    }
    return found;
}

#endif  // AX_ENABLE_3D

bool NodeReflection::addProvider(std::string_view id, std::unique_ptr<PropertyProvider> provider)
{
    const auto found = std::find_if(_providers.begin(), _providers.end(),
                                     [&](const auto& entry) { return entry.first == id; });
    if (found != _providers.end())
        return false;

    // Insert at the front so the most recently registered provider is consulted first - see the
    // ordering comment on _providers in the header.
    _providers.emplace(_providers.begin(), std::string(id), std::move(provider));
    return true;
}

void NodeReflection::removeProvider(std::string_view id)
{
    _providers.erase(std::remove_if(_providers.begin(), _providers.end(),
                                     [&](const auto& entry) { return entry.first == id; }),
                      _providers.end());
}

}  // namespace ax
