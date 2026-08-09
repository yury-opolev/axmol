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

#include "2d/Node.h"
#include "base/NodeFactory.h"

#include <string>

using namespace ax;

// Creating a Sprite or a Label needs a live texture cache and font renderer, which a headless
// unit test does not have - so these tests cover the registry itself and the failure paths that
// are reached BEFORE any engine object is built. Successful construction of those types is
// covered end to end against the running game instead.

TEST_CASE("factory_creates_a_plain_node")
{
    std::string error;
    auto* node = NodeFactory::getInstance()->create("ax::Node", {}, error);
    CHECK(node != nullptr);
    CHECK(error.empty());
}

TEST_CASE("factory_reports_unknown_types_by_name")
{
    std::string error;
    auto* node = NodeFactory::getInstance()->create("ax::NoSuchType", {}, error);
    CHECK(node == nullptr);
    // The name has to appear: "could not create node" leaves a caller with nowhere to go.
    CHECK(error.find("ax::NoSuchType") != std::string::npos);
}

TEST_CASE("built_in_types_are_registered")
{
    auto* factory = NodeFactory::getInstance();
    CHECK(factory->isRegistered("ax::Node"));
    CHECK(factory->isRegistered("ax::Sprite"));
    CHECK(factory->isRegistered("ax::Label"));
    CHECK(factory->isRegistered("ax::LayerColor"));
    CHECK_FALSE(factory->isRegistered("ax::Node "));  // no trimming: exact names only
    CHECK_FALSE(factory->isRegistered(""));
}

TEST_CASE("registered_types_are_discoverable")
{
    // Without this an agent finds the registry's limits one failed create at a time.
    const auto types = NodeFactory::getInstance()->registeredTypes();
    CHECK(types.size() >= 4);
    CHECK(std::find(types.begin(), types.end(), "ax::Sprite") != types.end());
}

TEST_CASE("creation_params_are_declared_per_type")
{
    auto* factory = NodeFactory::getInstance();
    CHECK(factory->creationParams("ax::Node").empty());

    const auto sprite = factory->creationParams("ax::Sprite");
    REQUIRE(sprite.size() == 1);
    CHECK(sprite[0] == "texturePath");

    const auto label = factory->creationParams("ax::Label");
    CHECK(label.size() == 2);

    // An unknown type has no params rather than being an error here - create() is where the
    // unknown type is reported.
    CHECK(factory->creationParams("nope").empty());
}

TEST_CASE("a_missing_required_param_names_the_param")
{
    std::string error;
    auto* node = NodeFactory::getInstance()->create("ax::Sprite", {}, error);
    CHECK(node == nullptr);
    CHECK(error.find("texturePath") != std::string::npos);
}

TEST_CASE("a_required_param_of_the_wrong_type_is_rejected")
{
    std::string error;
    PropertyBag params{{"texturePath", 42}};
    auto* node = NodeFactory::getInstance()->create("ax::Sprite", params, error);
    CHECK(node == nullptr);
    CHECK(error.find("texturePath") != std::string::npos);
    CHECK(error.find("string") != std::string::npos);
}

TEST_CASE("an_empty_required_param_is_rejected")
{
    // Sprite::create("") would fail anyway, but with a much less useful message.
    std::string error;
    PropertyBag params{{"texturePath", std::string()}};
    CHECK(NodeFactory::getInstance()->create("ax::Sprite", params, error) == nullptr);
    CHECK(error.find("empty") != std::string::npos);
}

TEST_CASE("registration_is_not_silently_overwritten")
{
    auto* factory = NodeFactory::getInstance();
    const auto creator = [](const PropertyBag&, std::string&) -> Node* { return Node::create(); };

    CHECK(factory->registerType("test::Unique1", {}, creator));
    // A second registration must not replace the first: two components each registering "their"
    // version of a type would otherwise silently depend on load order.
    CHECK_FALSE(factory->registerType("test::Unique1", {}, creator));
    CHECK(factory->isRegistered("test::Unique1"));
}

TEST_CASE("registration_rejects_an_empty_name_or_creator")
{
    auto* factory = NodeFactory::getInstance();
    CHECK_FALSE(factory->registerType("", {}, [](const PropertyBag&, std::string&) -> Node* { return nullptr; }));
    CHECK_FALSE(factory->registerType("test::NoCreator", {}, NodeFactory::Creator{}));
    CHECK_FALSE(factory->isRegistered("test::NoCreator"));
}

TEST_CASE("a_creator_that_fails_without_a_message_still_produces_one")
{
    // Otherwise the caller gets nullptr and an empty string, which reads as "no error".
    auto* factory = NodeFactory::getInstance();
    CHECK(factory->registerType("test::AlwaysFails", {},
                                [](const PropertyBag&, std::string&) -> Node* { return nullptr; }));

    std::string error;
    CHECK(factory->create("test::AlwaysFails", {}, error) == nullptr);
    CHECK_FALSE(error.empty());
}

TEST_CASE("find_param_locates_by_name")
{
    PropertyBag params{{"a", 1}, {"b", std::string("two")}};
    REQUIRE(findParam(params, "b") != nullptr);
    CHECK(std::get<std::string>(*findParam(params, "b")) == "two");
    CHECK(findParam(params, "missing") == nullptr);
}

TEST_CASE("a texture path that escapes the resource root is refused")
{
    // Ungated code: FileUtils::fullPathForFilename passes an absolute path through unchanged, so an
    // unchecked texturePath means "render any image on this machine" - and with a screenshot tool
    // on the other end of the bridge that is a file-disclosure primitive, not just untidiness.
    auto* factory = NodeFactory::getInstance();

    for (const char* texturePath : {"C:/windows/secret.png", "/etc/passwd", "../../secret.png",
                                    "a; whoami #.png", "shot.png:hidden"})
    {
        INFO("texturePath: " << texturePath);
        std::string error;
        CHECK(factory->create("ax::Sprite", {{"texturePath", std::string(texturePath)}}, error) == nullptr);
        CHECK(error.find("resource-relative") != std::string::npos);
    }
}

TEST_CASE("the font axmol actually ships is not rejected by the path rule")
{
    // The regression the space allowance in ResourcePath exists for, asserted at the layer that
    // consumes it: the engine's own default font is "fonts/Marker Felt.ttf", so a rule that
    // refuses a space makes every Label unbuildable from a scene file. Caught originally by the
    // live round-trip smoke test.
    //
    // Deliberately NOT asserting the label is built. Whether this binary can resolve that TTF
    // depends on its resource search path, which is not what this test is about - and pinning it
    // would make the test fail for a reason that has nothing to do with the rule. What must never
    // happen is a rejection by the PATH check.
    auto* factory = NodeFactory::getInstance();
    std::string error;
    factory->create("ax::Label",
                    {{"text", std::string("hello")},
                     {"fontName", std::string("fonts/Marker Felt.ttf")},
                     {"fontSize", 24.0f}},
                    error);
    CHECK(error.find("resource-relative") == std::string::npos);
}

TEST_CASE("a font path that escapes the resource root is refused")
{
    auto* factory = NodeFactory::getInstance();
    std::string error;
    CHECK(factory->create("ax::Label",
                          {{"text", std::string("hi")}, {"fontName", std::string("C:/windows/fonts/arial.ttf")},
                           {"fontSize", 24.0f}},
                          error) == nullptr);
    CHECK(error.find("resource-relative") != std::string::npos);
}
