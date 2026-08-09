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

#include <doctest.h>

#include "base/NodeReflection.h"
#include "2d/Node.h"
#include "base/NodeFactory.h"
#if defined(AX_ENABLE_3D)
#    include "2d/Camera.h"
#    include "3d/MeshRenderer.h"
#endif
#include "fmt/format.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace ax;

namespace
{

/// Builds:  root
///            +-- a        (child 0)  "alpha"  tag 10
///            |     +-- a0 (child 0)  "alpha-0"
///            |     +-- a1 (child 1)  "alpha-1"
///            |           +-- a1x     (child 0) "deep"
///            +-- b        (child 1)  "beta"   tag 20
struct Fixture
{
    Node* root = nullptr;
    Node* a    = nullptr;
    Node* b    = nullptr;
    Node* a0   = nullptr;
    Node* a1   = nullptr;
    Node* a1x  = nullptr;

    Fixture()
    {
        root = Node::create();
        root->setName("root");

        a = Node::create();
        a->setName("alpha");
        a->setTag(10);

        b = Node::create();
        b->setName("beta");
        b->setTag(20);

        a0 = Node::create();
        a0->setName("alpha-0");

        a1 = Node::create();
        a1->setName("alpha-1");

        a1x = Node::create();
        a1x->setName("deep");

        // addChild order determines child index, which is what paths are built from.
        root->addChild(a);
        root->addChild(b);
        a->addChild(a0);
        a->addChild(a1);
        a1->addChild(a1x);

        root->retain();
    }

    ~Fixture() { root->release(); }
};

/// A provider used to prove the registry works without depending on any concrete node type.
class StubProvider : public PropertyProvider
{
public:
    bool supports(Node*) const override { return true; }

    std::vector<PropertyInfo> list() const override
    {
        return {PropertyInfo{"stubValue", PropertyType::Int, true},
                PropertyInfo{"stubReadOnly", PropertyType::Int, false}};
    }

    bool get(Node*, std::string_view name, PropertyValue& out) const override
    {
        if (name == "stubValue")
            out = 4242;
        else if (name == "stubReadOnly")
            out = 7;
        else
            return false;
        return true;
    }

    bool set(Node*, std::string_view name, const PropertyValue& value) const override
    {
        if (name == "stubReadOnly")
            return false;
        return name == "stubValue" && std::holds_alternative<int>(value);
    }
};

/// A provider used to prove that a later-registered provider can override a built-in property
/// name (FIX 5 regression: addProvider must insert at the front so recency wins).
class OverridePositionProvider : public PropertyProvider
{
public:
    bool supports(Node*) const override { return true; }

    std::vector<PropertyInfo> list() const override
    {
        return {PropertyInfo{"position", PropertyType::Vec2, true}};
    }

    bool get(Node*, std::string_view name, PropertyValue& out) const override
    {
        if (name != "position")
            return false;
        out = Vec2(999.0f, 999.0f);
        return true;
    }

    bool set(Node*, std::string_view, const PropertyValue&) const override { return false; }
};

}  // namespace

TEST_SUITE("core/base/NodeReflection")
{
    TEST_CASE("node_ids_are_opaque_handles_not_addresses")
    {
        // The id used to be the raw pointer, which every scene.tree response then handed to whoever
        // was on the other end of the bridge: a live heap address per node, i.e. an ASLR defeat
        // given away for free, and the missing half of an exploit for any use-after-free elsewhere.
        auto* node       = Node::create();
        auto* other      = Node::create();
        auto* reflection = NodeReflection::getInstance();

        const auto id = reflection->describe(node).id;

        CHECK_FALSE(id.empty());
        // Stable within a run - the one property the id is documented to have.
        CHECK(id == reflection->describe(node).id);
        CHECK(id != reflection->describe(other).id);

        // ...and not the address, in any of the obvious spellings.
        const auto address    = reinterpret_cast<uintptr_t>(node);
        const auto lowercase  = fmt::format("{:x}", address);
        const auto uppercase  = fmt::format("{:X}", address);
        const auto pointerFmt = fmt::format("{}", fmt::ptr(node));
        CHECK(id.find(lowercase) == std::string::npos);
        CHECK(id.find(uppercase) == std::string::npos);
        CHECK(id != pointerFmt);
    }

    TEST_CASE("node_ids_do_not_leak_the_distance_between_two_nodes")
    {
        // The checks above pass for `addr ^ salt`, which is what the implementation ACTUALLY was on
        // every non-MSVC target: std::hash<T*> is the identity function in libstdc++ and libc++, so
        // hashing the pointer hashed nothing. Under that scheme any two ids XOR to the exact
        // distance between their nodes on the heap, and one known address recovers the salt and
        // therefore all of them - so the ids were an ASLR oracle while looking opaque.
        //
        // This is the property that distinguishes a real mixer from an XOR, and it is the reason
        // the mixing is written out by hand instead of delegated to std::hash.
        auto* first      = Node::create();
        auto* second     = Node::create();
        auto* reflection = NodeReflection::getInstance();

        const auto idDelta = std::stoull(reflection->describe(first).id, nullptr, 16) ^
                             std::stoull(reflection->describe(second).id, nullptr, 16);
        const auto addressDelta = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(first)) ^
                                  static_cast<uint64_t>(reinterpret_cast<uintptr_t>(second));

        CHECK(idDelta != addressDelta);
    }

    TEST_CASE("type_name_is_demangled")
    {
        auto node = Node::create();
        // On MSVC typeid().name() yields "class ax::Node"; the leading "class " must be gone.
        CHECK_EQ(std::string("ax::Node"), NodeReflection::getTypeName(node));
    }

    TEST_CASE("describe_reports_basic_state")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        f.a->setPosition(Vec2(12.0f, 34.0f));
        f.a->setVisible(false);

        const auto info = reflection->describe(f.a, "/0");

        CHECK_EQ(std::string("alpha"), info.name);
        CHECK_EQ(10, info.tag);
        CHECK_EQ(2, info.childCount);
        CHECK_EQ(std::string("/0"), info.path);
        CHECK_EQ(std::string("ax::Node"), info.typeName);
        CHECK_EQ(12.0f, info.position.x);
        CHECK_EQ(34.0f, info.position.y);
        CHECK_FALSE(info.visible);
        CHECK_FALSE(info.id.empty());
    }

    TEST_CASE("describe_tree_respects_depth")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        SUBCASE("depth zero yields only the root")
        {
            const auto tree = reflection->describeTree(f.root, 0);
            REQUIRE_EQ(1u, tree.size());
            CHECK_EQ(std::string("root"), tree[0].name);
            CHECK_EQ(std::string("/"), tree[0].path);
        }

        SUBCASE("depth one yields the root and its direct children")
        {
            const auto tree = reflection->describeTree(f.root, 1);
            REQUIRE_EQ(3u, tree.size());
            CHECK_EQ(std::string("root"), tree[0].name);
            CHECK_EQ(std::string("alpha"), tree[1].name);
            CHECK_EQ(std::string("beta"), tree[2].name);
        }

        SUBCASE("unlimited depth yields every node in pre-order")
        {
            const auto tree = reflection->describeTree(f.root, -1);
            REQUIRE_EQ(6u, tree.size());
            CHECK_EQ(std::string("root"), tree[0].name);
            CHECK_EQ(std::string("alpha"), tree[1].name);
            CHECK_EQ(std::string("alpha-0"), tree[2].name);
            CHECK_EQ(std::string("alpha-1"), tree[3].name);
            CHECK_EQ(std::string("deep"), tree[4].name);
            CHECK_EQ(std::string("beta"), tree[5].name);
        }

        SUBCASE("every entry carries its own resolvable path")
        {
            // Fixture node names are unique, so resolving each entry's own path must land back
            // on a node with that entry's name - not merely on some non-null node (which would
            // trivially pass even if every path resolved to the root).
            const auto tree = reflection->describeTree(f.root, -1);
            for (const auto& entry : tree)
            {
                auto* resolved = reflection->resolve(f.root, entry.path);
                REQUIRE(resolved != nullptr);
                CHECK_EQ(entry.name, std::string(resolved->getName()));
            }
        }
    }

    TEST_CASE("path_round_trips_for_a_deep_node")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        const auto path = reflection->pathOf(f.root, f.a1x);
        CHECK_EQ(std::string("/0/1/0"), path);
        CHECK_EQ(f.a1x, reflection->resolve(f.root, path));
    }

    TEST_CASE("path_of_root_is_slash")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        CHECK_EQ(std::string("/"), reflection->pathOf(f.root, f.root));
        CHECK_EQ(f.root, reflection->resolve(f.root, "/"));
    }

    TEST_CASE("path_of_unrelated_node_is_empty")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        auto stranger = Node::create();
        CHECK(reflection->pathOf(f.root, stranger).empty());
    }

    TEST_CASE("resolve_rejects_bad_paths")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        CHECK_EQ(nullptr, reflection->resolve(f.root, ""));
        CHECK_EQ(nullptr, reflection->resolve(f.root, "/0/99"));
        CHECK_EQ(nullptr, reflection->resolve(f.root, "/-1"));
        CHECK_EQ(nullptr, reflection->resolve(f.root, "/abc"));
        CHECK_EQ(nullptr, reflection->resolve(f.root, "0/1"));
        CHECK_EQ(nullptr, reflection->resolve(nullptr, "/0"));
    }

    TEST_CASE("get_property_reads_node_state")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        f.a->setPosition(Vec2(5.0f, 6.0f));
        f.a->setLocalZOrder(7);
        f.a->setVisible(false);
        f.a->setScaleX(2.5f);

        PropertyValue value;

        REQUIRE(reflection->getProperty(f.a, "position", value));
        CHECK_EQ(Vec2(5.0f, 6.0f), std::get<Vec2>(value));

        REQUIRE(reflection->getProperty(f.a, "localZOrder", value));
        CHECK_EQ(7, std::get<int>(value));

        REQUIRE(reflection->getProperty(f.a, "visible", value));
        CHECK_EQ(false, std::get<bool>(value));

        REQUIRE(reflection->getProperty(f.a, "scaleX", value));
        CHECK_EQ(2.5f, std::get<float>(value));

        REQUIRE(reflection->getProperty(f.a, "name", value));
        CHECK_EQ(std::string("alpha"), std::get<std::string>(value));
    }

    TEST_CASE("set_property_writes_through_to_the_node")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        REQUIRE(reflection->setProperty(f.a, "position", PropertyValue{Vec2(11.0f, 22.0f)}));
        CHECK_EQ(11.0f, f.a->getPosition().x);
        CHECK_EQ(22.0f, f.a->getPosition().y);

        REQUIRE(reflection->setProperty(f.a, "visible", PropertyValue{false}));
        CHECK_FALSE(f.a->isVisible());

        REQUIRE(reflection->setProperty(f.a, "localZOrder", PropertyValue{42}));
        CHECK_EQ(42, f.a->getLocalZOrder());

        REQUIRE(reflection->setProperty(f.a, "name", PropertyValue{std::string("renamed")}));
        CHECK_EQ(std::string("renamed"), f.a->getName());
    }

    TEST_CASE("set_property_rejects_wrong_type_without_mutating")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        f.a->setVisible(true);
        // "visible" is a bool; an int must be refused rather than coerced.
        CHECK_FALSE(reflection->setProperty(f.a, "visible", PropertyValue{3}));
        CHECK(f.a->isVisible());

        f.a->setPosition(Vec2(1.0f, 2.0f));
        CHECK_FALSE(reflection->setProperty(f.a, "position", PropertyValue{std::string("nope")}));
        CHECK_EQ(1.0f, f.a->getPosition().x);
        CHECK_EQ(2.0f, f.a->getPosition().y);
    }

    TEST_CASE("set_property_rejects_unknown_name")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        CHECK_FALSE(reflection->setProperty(f.a, "noSuchProperty", PropertyValue{1}));

        PropertyValue value;
        CHECK_FALSE(reflection->getProperty(f.a, "noSuchProperty", value));
    }

    TEST_CASE("list_properties_includes_the_common_node_set")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        const auto properties = reflection->listProperties(f.a);
        REQUIRE_FALSE(properties.empty());

        auto has = [&properties](std::string_view name) {
            for (const auto& property : properties)
            {
                if (property.name == name)
                    return true;
            }
            return false;
        };

        CHECK(has("position"));
        CHECK(has("visible"));
        CHECK(has("localZOrder"));
        CHECK(has("name"));
    }

    TEST_CASE("providers_can_be_registered_and_removed")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        REQUIRE(reflection->addProvider("__stub__", std::make_unique<StubProvider>()));
        // Registering the same id twice must be refused rather than silently replacing.
        CHECK_FALSE(reflection->addProvider("__stub__", std::make_unique<StubProvider>()));

        PropertyValue value;
        REQUIRE(reflection->getProperty(f.a, "stubValue", value));
        CHECK_EQ(4242, std::get<int>(value));

        reflection->removeProvider("__stub__");
        CHECK_FALSE(reflection->getProperty(f.a, "stubValue", value));
    }

    TEST_CASE("null_node_is_handled_without_crashing")
    {
        auto* reflection = NodeReflection::getInstance();

        PropertyValue value;
        CHECK_FALSE(reflection->getProperty(nullptr, "position", value));
        CHECK_FALSE(reflection->setProperty(nullptr, "position", PropertyValue{Vec2::ZERO}));
        CHECK(reflection->listProperties(nullptr).empty());
        CHECK(reflection->describeTree(nullptr, -1).empty());
    }

    TEST_CASE("resolve_rejects_overflowing_index")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        // CRITICAL regression: unbounded accumulation used to wrap "/18446744073709551616" to 0,
        // silently resolving to child 0, and turn "/9223372036854775808" into a huge negative
        // index that slipped past the `index >= children.size()` bounds check into
        // Vector::at() undefined behaviour. Both must now be rejected outright.
        CHECK_EQ(nullptr, reflection->resolve(f.root, "/18446744073709551616"));
        CHECK_EQ(nullptr, reflection->resolve(f.root, "/9223372036854775808"));
    }

    TEST_CASE("resolve_rejects_malformed_slashes")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        CHECK_EQ(nullptr, reflection->resolve(f.root, "/0/"));
        CHECK_EQ(nullptr, reflection->resolve(f.root, "//"));
    }

    TEST_CASE("set_property_opacity_rejects_out_of_range")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        f.a->setOpacity(200);

        CHECK_FALSE(reflection->setProperty(f.a, "opacity", PropertyValue{300}));
        CHECK_EQ(200, static_cast<int>(f.a->getOpacity()));

        CHECK_FALSE(reflection->setProperty(f.a, "opacity", PropertyValue{-1}));
        CHECK_EQ(200, static_cast<int>(f.a->getOpacity()));
    }

    TEST_CASE("set_property_color_round_trips_including_alpha")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        const Color4B written(10, 20, 30, 128);
        REQUIRE(reflection->setProperty(f.a, "color", PropertyValue{written}));
        CHECK_EQ(128, static_cast<int>(f.a->getOpacity()));

        PropertyValue value;
        REQUIRE(reflection->getProperty(f.a, "color", value));
        const auto read = std::get<Color4B>(value);
        CHECK_EQ(10, read.r);
        CHECK_EQ(20, read.g);
        CHECK_EQ(30, read.b);
        CHECK_EQ(128, read.a);
    }

    TEST_CASE("get_property_leaves_out_untouched_on_failure")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        PropertyValue value = std::string("sentinel");
        CHECK_FALSE(reflection->getProperty(f.a, "noSuchProperty", value));
        REQUIRE(std::holds_alternative<std::string>(value));
        CHECK_EQ(std::string("sentinel"), std::get<std::string>(value));
    }

    TEST_CASE("set_property_rejects_read_only_property")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        REQUIRE(reflection->addProvider("__stub_readonly__", std::make_unique<StubProvider>()));

        CHECK_FALSE(reflection->setProperty(f.a, "stubReadOnly", PropertyValue{1}));

        PropertyValue value;
        REQUIRE(reflection->getProperty(f.a, "stubReadOnly", value));
        CHECK_EQ(7, std::get<int>(value));

        reflection->removeProvider("__stub_readonly__");
    }

    TEST_CASE("later_registered_provider_overrides_a_builtin_property")
    {
        Fixture f;
        auto* reflection = NodeReflection::getInstance();

        // Regression for FIX 5: addProvider() must insert at the front of _providers so that a
        // provider registered after the built-ins is consulted first and can legitimately
        // override a colliding built-in property name (e.g. "position").
        REQUIRE(reflection->addProvider("__override_position__", std::make_unique<OverridePositionProvider>()));

        PropertyValue value;
        REQUIRE(reflection->getProperty(f.a, "position", value));
        CHECK_EQ(Vec2(999.0f, 999.0f), std::get<Vec2>(value));

        reflection->removeProvider("__override_position__");

        f.a->setPosition(Vec2(5.0f, 6.0f));
        REQUIRE(reflection->getProperty(f.a, "position", value));
        CHECK_EQ(Vec2(5.0f, 6.0f), std::get<Vec2>(value));
    }
}

TEST_SUITE("core/base/NodeReflection-3d")
{
    TEST_CASE("the 3D transform is reflected on every node, not just 3D ones")
    {
        // position3D/rotation3D/scaleZ live on the base Node provider deliberately: Node carries
        // this state whether or not AX_ENABLE_3D is on, and a 2D Sprite laid out on a board at a
        // depth is an ordinary thing to want. Gating them behind the 3D module would make the
        // property surface depend on a build option that has nothing to do with them.
        auto* node = Node::create();
        node->retain();
        auto* reflection = NodeReflection::getInstance();

        REQUIRE(reflection->setProperty(node, "position3D", Vec3(1.0f, 2.0f, 3.0f)));
        REQUIRE(reflection->setProperty(node, "rotation3D", Vec3(10.0f, 20.0f, 30.0f)));
        REQUIRE(reflection->setProperty(node, "scaleZ", 4.0f));

        PropertyValue value;
        REQUIRE(reflection->getProperty(node, "position3D", value));
        CHECK_EQ(Vec3(1.0f, 2.0f, 3.0f), std::get<Vec3>(value));
        REQUIRE(reflection->getProperty(node, "rotation3D", value));
        CHECK_EQ(Vec3(10.0f, 20.0f, 30.0f), std::get<Vec3>(value));
        REQUIRE(reflection->getProperty(node, "scaleZ", value));
        CHECK(std::get<float>(value) == doctest::Approx(4.0f));

        node->release();
    }

    TEST_CASE("writing position3D agrees with the 2D position on x and y")
    {
        // The two properties are views of one piece of state. If they ever disagree, an agent
        // reading the tree (which reports the 2D position) would be looking at a different node
        // from the one it just moved.
        auto* node = Node::create();
        node->retain();
        auto* reflection = NodeReflection::getInstance();

        REQUIRE(reflection->setProperty(node, "position3D", Vec3(7.0f, 8.0f, 9.0f)));

        PropertyValue value;
        REQUIRE(reflection->getProperty(node, "position", value));
        CHECK_EQ(Vec2(7.0f, 8.0f), std::get<Vec2>(value));

        node->release();
    }

    TEST_CASE("a 3D property rejects the wrong variant alternative without mutating the node")
    {
        // The contract every provider owes setProperty: reject cleanly, leave the node alone.
        // Worth pinning here because a Vec2 passed where a Vec3 belongs is the single most likely
        // mistake a caller writing JSON by hand will make.
        auto* node = Node::create();
        node->retain();
        node->setPosition3D(Vec3(1.0f, 2.0f, 3.0f));
        auto* reflection = NodeReflection::getInstance();

        // GUARD: setProperty also returns false for a name nobody recognises, so without this the
        // two CHECK_FALSEs below would pass just as happily against a build where these properties
        // do not exist at all - proving nothing about the type checking they are meant to pin.
        REQUIRE(reflection->setProperty(node, "position3D", Vec3(1.0f, 2.0f, 3.0f)));

        CHECK_FALSE(reflection->setProperty(node, "position3D", Vec2(50.0f, 60.0f)));
        CHECK_FALSE(reflection->setProperty(node, "scaleZ", 5));

        CHECK_EQ(Vec3(1.0f, 2.0f, 3.0f), node->getPosition3D());
        CHECK(node->getScaleZ() == doctest::Approx(1.0f));

        node->release();
    }

    TEST_CASE("describe reports depth, so a 3D tree does not read as flat")
    {
        // Without this, describeTree returns a Vec2 position for every node and a board whose
        // pieces differ only in Z comes back looking like a pile at one point. Being silently
        // wrong is worse than being incomplete - the agent has no way to notice.
        auto* node = Node::create();
        node->retain();
        node->setPosition3D(Vec3(1.0f, 2.0f, 30.0f));
        node->setRotation3D(Vec3(11.0f, 22.0f, 33.0f));
        node->setScaleZ(2.5f);

        const auto info = NodeReflection::getInstance()->describe(node, "/");

        CHECK(info.positionZ == doctest::Approx(30.0f));
        CHECK(info.scaleZ == doctest::Approx(2.5f));
        CHECK_EQ(Vec3(11.0f, 22.0f, 33.0f), info.rotation3D);

        node->release();
    }

    TEST_CASE("the 3D transform properties are listed, not merely gettable")
    {
        // listProperties is what axmol_node_properties reports, and it is how an agent discovers
        // what it may write. A property that works but is not advertised is one nobody uses.
        auto* node = Node::create();
        node->retain();

        const auto properties = NodeReflection::getInstance()->listProperties(node);
        const auto has        = [&](std::string_view name, PropertyType type) {
            return std::any_of(properties.begin(), properties.end(), [&](const PropertyInfo& p) {
                return p.name == name && p.type == type && p.writable;
            });
        };

        CHECK(has("position3D", PropertyType::Vec3));
        CHECK(has("rotation3D", PropertyType::Vec3));
        CHECK(has("scaleZ", PropertyType::Float));

        node->release();
    }
}

#if defined(AX_ENABLE_3D)
TEST_SUITE("core/base/NodeReflection-3d-roundtrip")
{
    TEST_CASE("every creation parameter a 3D type declares is readable as a property")
    {
        // THE CONTRACT, spelled out on NodeFactory::registerType: creation params are captured at
        // save time by reading them back through NodeReflection (SceneSerializer.cpp:130-135), and
        // a name no provider exposes is SILENTLY SKIPPED. So a type that declares a param nobody
        // can read produces a file that reports success and cannot be reloaded - the exact defect
        // the depth-cap fix dealt with in the last review, arriving by a different route.
        //
        // MeshRenderer::create() with no arguments builds an empty renderer without touching the
        // filesystem, which is what makes this testable headlessly at all.
        auto* factory    = NodeFactory::getInstance();
        auto* reflection = NodeReflection::getInstance();

        auto* mesh = MeshRenderer::create();
        REQUIRE(mesh != nullptr);
        mesh->retain();

        for (const auto& param : factory->creationParams("ax::MeshRenderer"))
        {
            CAPTURE(param);
            PropertyValue value;
            CHECK(reflection->getProperty(mesh, param, value));
        }
        mesh->release();

        auto* camera = Camera::create();
        REQUIRE(camera != nullptr);
        camera->retain();

        for (const auto& param : factory->creationParams("ax::Camera"))
        {
            CAPTURE(param);
            PropertyValue value;
            CHECK(reflection->getProperty(camera, param, value));
        }
        camera->release();
    }

    TEST_CASE("a light round-trips its creation parameters")
    {
        // Lights are the one 3D family fully constructible headlessly, so this closes the loop the
        // test above only checks halfway: declared, readable, AND the value that comes back is the
        // one that was asked for.
        auto* factory    = NodeFactory::getInstance();
        auto* reflection = NodeReflection::getInstance();

        std::string error;
        auto* light = factory->create("ax::PointLight", {{"range", 250.0f}}, error);
        REQUIRE(light != nullptr);
        light->retain();

        PropertyValue value;
        REQUIRE(reflection->getProperty(light, "range", value));
        CHECK(std::get<float>(value) == doctest::Approx(250.0f));

        auto* directional = factory->create("ax::DirectionLight", {{"direction", Vec3(0.0f, -1.0f, 0.0f)}}, error);
        REQUIRE(directional != nullptr);
        directional->retain();

        // Compared with tolerance, not exactly, and that is a fact about the engine rather than a
        // slack assertion: DirectionLight stores a direction as a ROTATION (setRotationFromDirection)
        // and recovers it from the transform matrix, so what comes back is the normalised direction
        // reconstructed through trigonometry. A caller that writes (0, -2, 0) reads back (0, -1, 0);
        // the bearing round-trips, the magnitude does not.
        REQUIRE(reflection->getProperty(directional, "direction", value));
        const auto direction = std::get<Vec3>(value);
        CHECK(direction.x == doctest::Approx(0.0f).epsilon(0.001));
        CHECK(direction.y == doctest::Approx(-1.0f).epsilon(0.001));
        CHECK(direction.z == doctest::Approx(0.0f).epsilon(0.001));

        light->release();
        directional->release();
    }
}
#endif  // AX_ENABLE_3D
