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
