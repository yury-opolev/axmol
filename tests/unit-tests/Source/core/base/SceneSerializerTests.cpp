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

#include "2d/Camera.h"
#include "2d/Node.h"
#include "2d/Scene.h"
#include "base/NodeFactory.h"
#include "base/NodeReflection.h"
#include "base/SceneSerializer.h"

#include <memory>
#include <string>
#include <vector>

using namespace ax;

namespace
{

/// A node type with no registered factory, for exercising the unsupported-type path.
class UnregisteredNode : public Node
{
public:
    static UnregisteredNode* create()
    {
        auto* node = new UnregisteredNode();
        node->init();
        node->autorelease();
        return node;
    }
};

std::string serializeOrFail(Node* root)
{
    std::string json;
    std::string error;
    REQUIRE(SceneSerializer::serialize(root, json, error));
    REQUIRE(error.empty());
    return json;
}

Node* deserializeOrFail(const std::string& json, std::vector<std::string>& warnings)
{
    std::string error;
    Node* node = SceneSerializer::deserialize(json, {}, error, warnings);
    INFO("deserialize error: " << error);
    REQUIRE(node != nullptr);
    return node;
}

}  // namespace

TEST_CASE("serialize_writes_a_versioned_envelope")
{
    const auto json = serializeOrFail(Node::create());
    CHECK(json.find("\"format\":\"axmol-scene\"") != std::string::npos);
    CHECK(json.find("\"version\":1") != std::string::npos);
    CHECK(json.find("\"type\":\"ax::Node\"") != std::string::npos);
}

TEST_CASE("serialize_rejects_a_null_root")
{
    std::string json;
    std::string error;
    CHECK_FALSE(SceneSerializer::serialize(nullptr, json, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("round_trip_preserves_properties")
{
    auto* root = Node::create();
    root->setName("root-node");
    root->setTag(7);
    root->setPosition(Vec2(12.5f, -30.0f));
    root->setScaleX(2.0f);
    root->setRotation(45.0f);
    root->setVisible(false);
    root->setLocalZOrder(3);

    std::vector<std::string> warnings;
    auto* loaded = deserializeOrFail(serializeOrFail(root), warnings);

    CHECK(loaded->getName() == "root-node");
    CHECK(loaded->getTag() == 7);
    CHECK(loaded->getPosition().x == doctest::Approx(12.5f));
    CHECK(loaded->getPosition().y == doctest::Approx(-30.0f));
    CHECK(loaded->getScaleX() == doctest::Approx(2.0f));
    CHECK(loaded->getRotation() == doctest::Approx(45.0f));
    CHECK(loaded->isVisible() == false);
    CHECK(loaded->getLocalZOrder() == 3);
    CHECK(warnings.empty());
}

TEST_CASE("round_trip_preserves_tree_shape_and_child_order")
{
    auto* root = Node::create();
    for (int i = 0; i < 3; ++i)
    {
        auto* child = Node::create();
        child->setName("child" + std::to_string(i));
        // Deliberately ascending z-order so sorted order matches creation order.
        child->setLocalZOrder(i);
        root->addChild(child);
    }
    auto* grandchild = Node::create();
    grandchild->setName("deep");
    root->getChildren().at(1)->addChild(grandchild);

    std::vector<std::string> warnings;
    auto* loaded = deserializeOrFail(serializeOrFail(root), warnings);

    REQUIRE(loaded->getChildrenCount() == 3);
    loaded->sortAllChildren();
    CHECK(loaded->getChildren().at(0)->getName() == "child0");
    CHECK(loaded->getChildren().at(1)->getName() == "child1");
    CHECK(loaded->getChildren().at(2)->getName() == "child2");
    REQUIRE(loaded->getChildren().at(1)->getChildrenCount() == 1);
    CHECK(loaded->getChildren().at(1)->getChildren().at(0)->getName() == "deep");
}

TEST_CASE("children_are_written_in_render_order_not_insertion_order")
{
    // Every other ordering test builds children with ASCENDING z, which makes sorted order and
    // insertion order identical - so deleting sortAllChildren() from the serializer keeps them all
    // green. Descending z is the only arrangement that can tell the two apart.
    auto* root = Node::create();
    for (int i = 0; i < 3; ++i)
    {
        auto* child = Node::create();
        child->setName("added" + std::to_string(i));
        child->setLocalZOrder(2 - i);  // added0 is drawn LAST
        root->addChild(child);
    }

    std::vector<std::string> warnings;
    auto* loaded = deserializeOrFail(serializeOrFail(root), warnings);

    REQUIRE(loaded->getChildrenCount() == 3);
    loaded->sortAllChildren();
    CHECK(loaded->getChildren().at(0)->getName() == "added2");
    CHECK(loaded->getChildren().at(1)->getName() == "added1");
    CHECK(loaded->getChildren().at(2)->getName() == "added0");
}

TEST_CASE("a_scenes_own_default_camera_is_left_out_of_the_file")
{
    // The rule this guards was itself a defect found only by the end-to-end script: a Scene writes
    // its own default Camera into the file, and loading then adds a second camera to a scene that
    // already has one.
    //
    // A real Scene, not a plain Node with a Camera stuck under it. The predicate is identity
    // against Scene::getDefaultCamera(), because "engine-managed" means "Scene owns this by raw
    // pointer" - see the companion case below for the half that distinction protects.
    auto* scene   = Scene::create();
    REQUIRE(scene != nullptr);
    auto* content = Node::create();
    content->setName("content");
    scene->addChild(content);

    const auto json = serializeOrFail(scene);

    CHECK(json.find("ax::Camera") == std::string::npos);
    CHECK(json.find("content") != std::string::npos);

    // deserializeChildren, not deserialize: the root of a whole-scene file is the game's own Scene
    // type, which no factory can rebuild. That is the case `mode: contents` exists for, and it is
    // what scene.load actually does with a file like this one.
    std::vector<Node*> children;
    std::string loadError;
    std::vector<std::string> warnings;
    REQUIRE(SceneSerializer::deserializeChildren(json, {}, children, loadError, warnings));
    // Exactly one: "content". The camera was never written, so nothing tries to rebuild it and no
    // second camera arrives in a scene that already has its own.
    CHECK(children.size() == 1);
}

TEST_CASE("a_camera_the_game_created_is_content_and_is_written")
{
    // The other half. An earlier isEngineManagedNode said "is a Camera", which also swallowed
    // cameras the GAME made - and a world/UI camera split is an ordinary Axmol pattern. Those
    // vanished from every saved scene with no warning at all: the file looked complete.
    auto* root     = Node::create();
    auto* uiCamera = Camera::create();
    REQUIRE(uiCamera != nullptr);
    uiCamera->setName("uiCamera");
    root->addChild(uiCamera);

    const auto json = serializeOrFail(root);

    CHECK(json.find("uiCamera") != std::string::npos);
    // No factory can rebuild a Camera, so it is recorded as unsupported rather than dropped - the
    // file keeps it, and a reader is told exactly what cannot be reconstructed.
    CHECK(json.find("\"unsupported\":true") != std::string::npos);
}

TEST_CASE("a_path_still_addresses_the_same_node_after_a_round_trip")
{
    // The point of persistence: a path an agent recorded before saving must mean the same node
    // after loading.
    auto* root = Node::create();
    for (int i = 0; i < 3; ++i)
    {
        auto* child = Node::create();
        child->setName("c" + std::to_string(i));
        child->setLocalZOrder(i);
        root->addChild(child);
    }

    std::vector<std::string> warnings;
    auto* loaded = deserializeOrFail(serializeOrFail(root), warnings);

    auto* reflection = NodeReflection::getInstance();
    auto* before     = reflection->resolve(root, "/1");
    auto* after      = reflection->resolve(loaded, "/1");
    REQUIRE(before != nullptr);
    REQUIRE(after != nullptr);
    CHECK(before->getName() == after->getName());
}

TEST_CASE("an_unsupported_type_is_recorded_and_refused_by_default")
{
    auto* root = UnregisteredNode::create();
    const auto json = serializeOrFail(root);

    // Recorded, so the file itself does not lose the information.
    CHECK(json.find("\"unsupported\":true") != std::string::npos);

    std::string error;
    std::vector<std::string> warnings;
    // A scene that loads "successfully" having dropped nodes fails later, in a screenshot, with
    // nothing pointing at the cause - so the default is a loud failure.
    CHECK(SceneSerializer::deserialize(json, {}, error, warnings) == nullptr);
    CHECK(error.find("UnregisteredNode") != std::string::npos);
}

TEST_CASE("degradation_is_opt_in_and_always_reported")
{
    auto* root = UnregisteredNode::create();
    root->setName("mystery");

    std::string error;
    std::vector<std::string> warnings;
    SceneSerializer::LoadOptions options;
    options.allowDegraded = true;

    auto* loaded = SceneSerializer::deserialize(serializeOrFail(root), options, error, warnings);

    REQUIRE(loaded != nullptr);
    CHECK(error.empty());
    CHECK(loaded->getName() == "mystery");
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("UnregisteredNode") != std::string::npos);
}

TEST_CASE("children_of_an_unsupported_node_survive_degradation")
{
    auto* root  = UnregisteredNode::create();
    auto* child = Node::create();
    child->setName("kept");
    root->addChild(child);

    std::string error;
    std::vector<std::string> warnings;
    SceneSerializer::LoadOptions options;
    options.allowDegraded = true;

    auto* loaded = SceneSerializer::deserialize(serializeOrFail(root), options, error, warnings);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->getChildrenCount() == 1);
    CHECK(loaded->getChildren().at(0)->getName() == "kept");
}

TEST_CASE("malformed_documents_are_errors_not_crashes")
{
    std::string error;
    std::vector<std::string> warnings;

    const char* cases[] = {
        "",
        "not json",
        "[]",
        "{}",
        R"({"format":"something-else","version":1,"root":{"type":"ax::Node"}})",
        R"({"format":"axmol-scene","root":{"type":"ax::Node"}})",
        R"({"format":"axmol-scene","version":1})",
        R"({"format":"axmol-scene","version":1,"root":{}})",
        R"({"format":"axmol-scene","version":1,"root":{"type":42}})",
        R"({"format":"axmol-scene","version":1,"root":"nope"})",
        R"({"format":"axmol-scene","version":1,"root":{"type":"ax::Node","props":"nope"}})",
        R"({"format":"axmol-scene","version":1,"root":{"type":"ax::Node","children":"nope"}})",
        R"({"format":"axmol-scene","version":1,"root":{"type":"ax::Node","create":"nope"}})",
    };

    for (const char* json : cases)
    {
        INFO("input: " << json);
        CHECK(SceneSerializer::deserialize(json, {}, error, warnings) == nullptr);
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("a_future_version_is_refused_rather_than_guessed_at")
{
    std::string error;
    std::vector<std::string> warnings;
    const auto json = R"({"format":"axmol-scene","version":99,"root":{"type":"ax::Node"}})";

    CHECK(SceneSerializer::deserialize(json, {}, error, warnings) == nullptr);
    CHECK(error.find("99") != std::string::npos);
}

TEST_CASE("a_property_of_the_wrong_type_is_an_error")
{
    // Never coerced - the same rule the bridge applies to node.set.
    std::string error;
    std::vector<std::string> warnings;
    const auto json = R"({"format":"axmol-scene","version":1,
        "root":{"type":"ax::Node","props":{"visible":"yes"}}})";

    CHECK(SceneSerializer::deserialize(json, {}, error, warnings) == nullptr);
    CHECK(error.find("visible") != std::string::npos);
}

TEST_CASE("an_unknown_property_is_reported_and_skipped")
{
    // A file from a newer build should still load; what it lost must not be invisible.
    std::string error;
    std::vector<std::string> warnings;
    const auto json = R"({"format":"axmol-scene","version":1,
        "root":{"type":"ax::Node","props":{"name":"kept","somethingNew":1}}})";

    auto* loaded = SceneSerializer::deserialize(json, {}, error, warnings);

    REQUIRE(loaded != nullptr);
    CHECK(loaded->getName() == "kept");
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("somethingNew") != std::string::npos);
}

TEST_CASE("read_only_properties_are_not_written_into_the_file")
{
    // They cannot be applied on load, and writing them would imply a fidelity the format does not
    // have. What matters for reconstruction is captured under "create" instead.
    //
    // A plain Node has NO read-only properties, so asserting against one would pass even if the
    // writability filter were deleted. This registers a provider that actually has one.
    class MixedProvider : public PropertyProvider
    {
    public:
        bool supports(Node*) const override { return true; }
        std::vector<PropertyInfo> list() const override
        {
            return {{"serializerTestWritable", PropertyType::Int, true},
                    {"serializerTestReadOnly", PropertyType::Int, false}};
        }
        bool get(Node*, std::string_view name, PropertyValue& out) const override
        {
            if (name != "serializerTestWritable" && name != "serializerTestReadOnly")
                return false;
            out = 7;
            return true;
        }
        bool set(Node*, std::string_view, const PropertyValue&) const override { return true; }
    };

    constexpr const char* kId = "__scene_serializer_tests_mixed__";
    REQUIRE(NodeReflection::getInstance()->addProvider(kId, std::make_unique<MixedProvider>()));

    const auto json = serializeOrFail(Node::create());
    NodeReflection::getInstance()->removeProvider(kId);

    CHECK(json.find("serializerTestWritable") != std::string::npos);
    CHECK(json.find("serializerTestReadOnly") == std::string::npos);
}

TEST_CASE("a_deeply_nested_document_is_an_error_not_a_stack_overflow")
{
    // A scene file is DATA and can come from anywhere, and this code is ungated so release builds
    // can load scenes. rapidjson's default parser recurses per nesting level, so an unbounded
    // document is a hard process kill rather than a catchable failure - and it would happen on the
    // Axmol main thread, where nothing upstream can contain it.
    std::string json = R"({"format":"axmol-scene","version":1,"root":)";
    constexpr int kDepth = 10000;
    for (int i = 0; i < kDepth; ++i)
        json += R"({"type":"ax::Node","children":[)";
    json += R"({"type":"ax::Node"})";
    for (int i = 0; i < kDepth; ++i)
        json += "]}";
    json += "}";

    std::string error;
    std::vector<std::string> warnings;
    CHECK(SceneSerializer::deserialize(json, {}, error, warnings) == nullptr);
    CHECK_FALSE(error.empty());

    std::vector<Node*> children;
    CHECK_FALSE(SceneSerializer::deserializeChildren(json, {}, children, error, warnings));
}

TEST_CASE("warnings_are_cleared_between_loads")
{
    std::string error;
    std::vector<std::string> warnings{"stale"};
    const auto json = R"({"format":"axmol-scene","version":1,"root":{"type":"ax::Node"}})";

    REQUIRE(SceneSerializer::deserialize(json, {}, error, warnings) != nullptr);
    CHECK(warnings.empty());
}

TEST_CASE("deserialize_children_loads_a_scene_whose_root_cannot_be_rebuilt")
{
    // The whole-scene case: the root is the game's own Scene subclass, which is application code
    // with its own lifecycle - not something a factory can or should reconstruct.
    auto* root  = UnregisteredNode::create();
    auto* childA = Node::create();
    childA->setName("a");
    auto* childB = Node::create();
    childB->setName("b");
    root->addChild(childA);
    root->addChild(childB);

    std::vector<Node*> children;
    std::string error;
    std::vector<std::string> warnings;

    REQUIRE(SceneSerializer::deserializeChildren(serializeOrFail(root), {}, children, error, warnings));
    CHECK(error.empty());
    REQUIRE(children.size() == 2);
    CHECK(children[0]->getName() == "a");
    CHECK(children[1]->getName() == "b");
    // Nothing was lost: the root was never rebuildable in the first place.
    CHECK(warnings.empty());
}

TEST_CASE("deserialize_children_warns_when_it_drops_a_root_it_could_have_built")
{
    // Silently discarding a node the caller could have had back is the failure mode this guards.
    auto* root  = Node::create();
    auto* child = Node::create();
    child->setName("kept");
    root->addChild(child);

    std::vector<Node*> children;
    std::string error;
    std::vector<std::string> warnings;

    REQUIRE(SceneSerializer::deserializeChildren(serializeOrFail(root), {}, children, error, warnings));
    REQUIRE(children.size() == 1);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("ax::Node") != std::string::npos);
}

TEST_CASE("deserialize_children_of_an_empty_root_is_success_not_failure")
{
    std::vector<Node*> children;
    std::string error;
    std::vector<std::string> warnings;

    CHECK(SceneSerializer::deserializeChildren(serializeOrFail(UnregisteredNode::create()), {}, children, error,
                                               warnings));
    CHECK(children.empty());
    CHECK(error.empty());
}

TEST_CASE("deserialize_children_rejects_the_same_malformed_documents")
{
    std::vector<Node*> children;
    std::string error;
    std::vector<std::string> warnings;

    for (const char* json : {"", "{}", R"({"format":"nope","version":1,"root":{}})",
                             R"({"format":"axmol-scene","version":99,"root":{}})",
                             R"({"format":"axmol-scene","version":1,"root":{"type":"ax::Node","children":"nope"}})"})
    {
        INFO("input: " << json);
        CHECK_FALSE(SceneSerializer::deserializeChildren(json, {}, children, error, warnings));
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("deserialize_children_returns_nothing_when_one_child_fails")
{
    // A partially loaded scene is worse than a refused one.
    std::vector<Node*> children;
    std::string error;
    std::vector<std::string> warnings;
    const auto json = R"({"format":"axmol-scene","version":1,"root":{"type":"ax::Node","children":[
        {"type":"ax::Node"},
        {"type":"ax::NoSuchType"}
    ]}})";

    CHECK_FALSE(SceneSerializer::deserializeChildren(json, {}, children, error, warnings));
    CHECK(children.empty());
}

TEST_CASE("a_failed_child_fails_the_whole_load")
{
    // Half a tree is worse than none: the caller would attach something that silently lacks nodes.
    std::string error;
    std::vector<std::string> warnings;
    const auto json = R"({"format":"axmol-scene","version":1,"root":{"type":"ax::Node","children":[
        {"type":"ax::Node"},
        {"type":"ax::NoSuchType"}
    ]}})";

    CHECK(SceneSerializer::deserialize(json, {}, error, warnings) == nullptr);
    CHECK(error.find("ax::NoSuchType") != std::string::npos);
    // The error names where it failed, not just what failed.
    CHECK(error.find("/1") != std::string::npos);
}

TEST_CASE("a tree deeper than the cap is truncated into a file that still LOADS")
{
    // The point of truncating rather than failing is that the caller gets a usable file - their
    // scene is not something they can be asked to fix. The first version emitted the marker one
    // level BELOW the cap and kept the original type, so the save reported success and the
    // resulting file was rejected by the loader every time: a silent, unrecoverable save.
    Node* root    = Node::create();
    Node* deepest = root;
    for (int i = 0; i < 200; ++i)
    {
        Node* child = Node::create();
        deepest->addChild(child);
        deepest = child;
    }

    std::string json, error;
    std::vector<std::string> saveWarnings;
    REQUIRE(SceneSerializer::serialize(root, json, error, &saveWarnings));

    // Truncation is REPORTED. A save that quietly dropped 70 levels is indistinguishable from one
    // that did not, which is the whole failure mode.
    CHECK_FALSE(saveWarnings.empty());
    CHECK(saveWarnings.front().find("truncated") != std::string::npos);

    std::string loadError;
    std::vector<std::string> loadWarnings;
    Node* reloaded = SceneSerializer::deserialize(json, {}, loadError, loadWarnings);
    CHECK(reloaded != nullptr);
    CHECK(loadError.empty());
}

TEST_CASE("a shallow tree is not truncated and reports nothing")
{
    Node* root = Node::create();
    root->addChild(Node::create());

    std::string json, error;
    std::vector<std::string> warnings;
    REQUIRE(SceneSerializer::serialize(root, json, error, &warnings));
    CHECK(warnings.empty());
    CHECK(json.find("truncated") == std::string::npos);
}
