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

#include "2d/Node.h"
#include "2d/Sprite.h"
#include "base/Protocols.h"
#include "fmt/format.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <mutex>

namespace ax
{

namespace
{
NodeReflection* g_instance = nullptr;

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
        else
        {
            return false;
        }
        return true;
    }
};

/// Sprite-only properties. texturePath is deliberately read-only: a live Sprite's texture is
/// set through Sprite::setTexture()/initWithFile(), not through a bare path string.
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
            out           = texture ? texture->getPath() : std::string();
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

    std::vector<PropertyInfo> list() const override { return {{"text", PropertyType::String, true}}; }

    bool get(Node* node, std::string_view name, PropertyValue& out) const override
    {
        auto* label = dynamic_cast<LabelProtocol*>(node);
        if (!label || name != "text")
            return false;

        out = std::string(label->getString());
        return true;
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
}

NodeReflection* NodeReflection::getInstance()
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] { g_instance = new NodeReflection(); });
    return g_instance;
}

void NodeReflection::destroyInstance()
{
    delete g_instance;
    g_instance = nullptr;
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

    info.id          = fmt::format("{}", fmt::ptr(node));
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
