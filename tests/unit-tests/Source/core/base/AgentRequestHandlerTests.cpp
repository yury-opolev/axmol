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

#include "base/AgentRequestHandler.h"
#include "base/NodeReflection.h"
#include "base/SceneAccess.h"
#include "2d/Node.h"

#include "rapidjson/document.h"

#include <memory>
#include <string>

using namespace ax;

namespace
{

// -------------------------------------------------------------------------------------------
// FakeSceneAccess: a headless double for SceneAccess. Every AgentRequestHandler test in this
// file goes through this rather than DirectorSceneAccess, so the whole suite runs with no
// socket, no window and no GPU - see the class comment on SceneAccess for why the design
// deliberately splits things this way.
// -------------------------------------------------------------------------------------------

class FakeSceneAccess : public SceneAccess
{
public:
    // nullptr simulates "no running scene".
    Node* scene = nullptr;

    bool tapCalled = false;
    float tapX     = 0.0f;
    float tapY     = 0.0f;

    bool swipeCalled = false;
    float swipeX1    = 0.0f;
    float swipeY1    = 0.0f;
    float swipeX2    = 0.0f;
    float swipeY2    = 0.0f;

    bool screenshotShouldFail   = false;
    std::string screenshotPath  = "/writable/agent-screenshot.png";
    std::string screenshotError = "capture failed";
    std::string lastScreenshotFile;

    bool paused = false;

    std::string engineVersion = "1.2.3-test";
    Vec2 designResolution     = Vec2(960.0f, 640.0f);
    Vec2 frameSize            = Vec2(1024.0f, 768.0f);
    float frameRate           = 60.0f;

    Node* getRunningScene() override { return scene; }

    bool captureScreenshot(std::string_view file, std::string& outPath, std::string& outError) override
    {
        lastScreenshotFile = std::string(file);
        if (screenshotShouldFail)
        {
            outError = screenshotError;
            return false;
        }
        outPath = screenshotPath;
        return true;
    }

    void injectTap(float x, float y) override
    {
        tapCalled = true;
        tapX      = x;
        tapY      = y;
    }

    void injectSwipe(float x1, float y1, float x2, float y2) override
    {
        swipeCalled = true;
        swipeX1     = x1;
        swipeY1     = y1;
        swipeX2     = x2;
        swipeY2     = y2;
    }

    void setPaused(bool p) override { paused = p; }
    bool isPaused() const override { return paused; }
    std::string getEngineVersion() const override { return engineVersion; }
    Vec2 getDesignResolution() const override { return designResolution; }
    Vec2 getFrameSize() const override { return frameSize; }
    float getFrameRate() const override { return frameRate; }
};

/// Builds:  root
///            +-- childA      (child 0)
///            +-- childB      (child 1)
///                  +-- grandchild (child 0 of childB)
/// 4 nodes total; kept deliberately small so scene.tree depth/count assertions stay readable.
struct Fixture
{
    Node* root       = nullptr;
    Node* childA     = nullptr;
    Node* childB     = nullptr;
    Node* grandchild = nullptr;

    Fixture()
    {
        root = Node::create();
        root->setName("root");

        childA = Node::create();
        childA->setName("childA");

        childB = Node::create();
        childB->setName("childB");

        grandchild = Node::create();
        grandchild->setName("grandchild");

        root->addChild(childA);
        root->addChild(childB);
        childB->addChild(grandchild);

        root->retain();
    }

    ~Fixture() { root->release(); }
};

/// Registers a single read-only int property, "agentTestReadOnly", on every node. Used to
/// exercise node.set's readonly_property path and node.properties' "writable" field - a plain
/// Node has no read-only property of its own to test against (every built-in NodePropertyProvider
/// property is writable), mirroring the StubProvider pattern in NodeReflectionTests.cpp.
class ReadOnlyStubProvider : public PropertyProvider
{
public:
    bool supports(Node*) const override { return true; }

    std::vector<PropertyInfo> list() const override
    {
        return {PropertyInfo{"agentTestReadOnly", PropertyType::Int, false}};
    }

    bool get(Node*, std::string_view name, PropertyValue& out) const override
    {
        if (name != "agentTestReadOnly")
            return false;
        out = 99;
        return true;
    }

    bool set(Node*, std::string_view, const PropertyValue&) const override { return false; }
};

constexpr const char* kReadOnlyProviderId = "__agent_request_handler_tests_readonly__";

/// Submits `requestJson` and parses the response, so every TEST_CASE can assert on fields
/// instead of comparing whole JSON strings. REQUIREs the response is itself well-formed JSON -
/// handle()'s core contract is that it always returns *some* valid response, so a parse failure
/// here is itself a handler bug worth failing loudly on.
rapidjson::Document sendRequest(AgentRequestHandler& handler, std::string_view requestJson)
{
    const std::string response = handler.handle(requestJson);

    rapidjson::Document doc;
    doc.Parse(response.c_str(), response.size());
    REQUIRE_FALSE(doc.HasParseError());
    REQUIRE(doc.IsObject());
    return doc;
}

}  // namespace

TEST_SUITE("core/base/AgentRequestHandler")
{
    // 1. Malformed JSON -> parse_error, and the connection is not killed.
    TEST_CASE("malformed_json_yields_parse_error_and_handler_stays_usable")
    {
        FakeSceneAccess access;
        AgentRequestHandler handler(&access);

        auto doc = sendRequest(handler, "{not valid json at all");
        REQUIRE_FALSE(doc["ok"].GetBool());
        CHECK_EQ(std::string("parse_error"), std::string(doc["error"]["code"].GetString()));
        CHECK(doc["id"].IsNull());

        // A malformed request must not wedge the handler - the next, well-formed request still
        // gets a normal response.
        auto next = sendRequest(handler, R"({"id":1,"method":"app.info"})");
        CHECK(next["ok"].GetBool());
    }

    // 2. Missing "method" -> invalid_request.
    TEST_CASE("missing_or_non_string_method_yields_invalid_request")
    {
        FakeSceneAccess access;
        AgentRequestHandler handler(&access);

        SUBCASE("method absent entirely")
        {
            auto doc = sendRequest(handler, R"({"id":1})");
            REQUIRE_FALSE(doc["ok"].GetBool());
            CHECK_EQ(std::string("invalid_request"), std::string(doc["error"]["code"].GetString()));
        }
        SUBCASE("method present but not a string")
        {
            auto doc = sendRequest(handler, R"({"id":1,"method":42})");
            REQUIRE_FALSE(doc["ok"].GetBool());
            CHECK_EQ(std::string("invalid_request"), std::string(doc["error"]["code"].GetString()));
        }
        SUBCASE("request is valid JSON but not an object")
        {
            auto doc = sendRequest(handler, R"([1,2,3])");
            REQUIRE_FALSE(doc["ok"].GetBool());
            CHECK_EQ(std::string("invalid_request"), std::string(doc["error"]["code"].GetString()));
        }
    }

    // 3. Unknown method -> unknown_method.
    TEST_CASE("unknown_method_yields_unknown_method")
    {
        FakeSceneAccess access;
        AgentRequestHandler handler(&access);

        auto doc = sendRequest(handler, R"({"id":1,"method":"not.a.real.method"})");
        REQUIRE_FALSE(doc["ok"].GetBool());
        CHECK_EQ(std::string("unknown_method"), std::string(doc["error"]["code"].GetString()));
    }

    // 4. "id" is echoed exactly, including on error, for both string and numeric ids.
    TEST_CASE("id_is_echoed_exactly_on_success_and_on_error")
    {
        FakeSceneAccess access;
        AgentRequestHandler handler(&access);

        SUBCASE("numeric id, successful request")
        {
            auto doc = sendRequest(handler, R"({"id":42,"method":"app.info"})");
            REQUIRE(doc["id"].IsInt());
            CHECK_EQ(42, doc["id"].GetInt());
        }
        SUBCASE("string id, successful request")
        {
            auto doc = sendRequest(handler, R"({"id":"abc-123","method":"app.info"})");
            REQUIRE(doc["id"].IsString());
            CHECK_EQ(std::string("abc-123"), std::string(doc["id"].GetString()));
        }
        SUBCASE("numeric id, erroring request")
        {
            auto doc = sendRequest(handler, R"({"id":7,"method":"nope"})");
            REQUIRE_FALSE(doc["ok"].GetBool());
            REQUIRE(doc["id"].IsInt());
            CHECK_EQ(7, doc["id"].GetInt());
        }
        SUBCASE("string id, erroring request")
        {
            auto doc = sendRequest(handler, R"({"id":"req-1","method":"nope"})");
            REQUIRE_FALSE(doc["ok"].GetBool());
            REQUIRE(doc["id"].IsString());
            CHECK_EQ(std::string("req-1"), std::string(doc["id"].GetString()));
        }
        SUBCASE("absent id becomes JSON null")
        {
            auto doc = sendRequest(handler, R"({"method":"app.info"})");
            CHECK(doc["id"].IsNull());
        }
    }

    // 5. scene.tree returns the right node count and honours depth.
    TEST_CASE("scene_tree_returns_node_count_and_honours_depth")
    {
        Fixture f;
        FakeSceneAccess access;
        access.scene = f.root;
        AgentRequestHandler handler(&access);

        SUBCASE("unlimited depth (the default) returns every node")
        {
            auto doc = sendRequest(handler, R"({"id":1,"method":"scene.tree"})");
            REQUIRE(doc["ok"].GetBool());
            CHECK_EQ(4u, doc["result"].Size());
        }
        SUBCASE("depth 0 returns only the root")
        {
            auto doc = sendRequest(handler, R"({"id":1,"method":"scene.tree","params":{"depth":0}})");
            REQUIRE(doc["ok"].GetBool());
            REQUIRE_EQ(1u, doc["result"].Size());
            CHECK_EQ(std::string("/"), std::string(doc["result"][0]["path"].GetString()));
        }
        SUBCASE("depth 1 returns the root and its direct children")
        {
            auto doc = sendRequest(handler, R"({"id":1,"method":"scene.tree","params":{"depth":1}})");
            REQUIRE(doc["ok"].GetBool());
            CHECK_EQ(3u, doc["result"].Size());
        }
        SUBCASE("\"path\" narrows the subtree and returned paths stay absolute")
        {
            // Regression guard: describeTree() itself would report this subtree's paths rooted
            // at "/" (see NodeReflection::describeTree's doc comment) - the handler must rewrite
            // them back to absolute paths ("/1", "/1/0", ...) so they're directly reusable in a
            // follow-up node.describe/get/set call.
            auto doc = sendRequest(handler, R"({"id":1,"method":"scene.tree","params":{"path":"/1"}})");
            REQUIRE(doc["ok"].GetBool());
            REQUIRE_EQ(2u, doc["result"].Size());
            CHECK_EQ(std::string("/1"), std::string(doc["result"][0]["path"].GetString()));
            CHECK_EQ(std::string("/1/0"), std::string(doc["result"][1]["path"].GetString()));
        }
    }

    // 6. scene.tree with no running scene -> no_scene.
    TEST_CASE("scene_tree_without_running_scene_yields_no_scene")
    {
        FakeSceneAccess access;  // scene left null
        AgentRequestHandler handler(&access);

        auto doc = sendRequest(handler, R"({"id":1,"method":"scene.tree"})");
        REQUIRE_FALSE(doc["ok"].GetBool());
        CHECK_EQ(std::string("no_scene"), std::string(doc["error"]["code"].GetString()));
    }

    // 7. node.describe on a bad path -> invalid_path.
    TEST_CASE("node_describe_bad_path_yields_invalid_path")
    {
        Fixture f;
        FakeSceneAccess access;
        access.scene = f.root;
        AgentRequestHandler handler(&access);

        auto doc = sendRequest(handler, R"({"id":1,"method":"node.describe","params":{"path":"/99"}})");
        REQUIRE_FALSE(doc["ok"].GetBool());
        CHECK_EQ(std::string("invalid_path"), std::string(doc["error"]["code"].GetString()));
    }

    // 8. node.get unknown property -> unknown_property.
    TEST_CASE("node_get_unknown_property_yields_unknown_property")
    {
        Fixture f;
        FakeSceneAccess access;
        access.scene = f.root;
        AgentRequestHandler handler(&access);

        auto doc = sendRequest(handler, R"({"id":1,"method":"node.get","params":{"path":"/0","name":"noSuchProperty"}})");
        REQUIRE_FALSE(doc["ok"].GetBool());
        CHECK_EQ(std::string("unknown_property"), std::string(doc["error"]["code"].GetString()));
    }

    // 9. node.set writes through - verified by reading the node afterwards.
    TEST_CASE("node_set_writes_through_and_is_readable_afterwards")
    {
        Fixture f;
        FakeSceneAccess access;
        access.scene = f.root;
        AgentRequestHandler handler(&access);

        // Deliberately a property that does NOT reorder siblings, so this case tests
        // write-through and read-back only. See node_set_localZOrder_can_change_the_nodes_path
        // below for the reordering interaction, which is a separate concern.
        auto setDoc =
            sendRequest(handler, R"({"id":1,"method":"node.set","params":{"path":"/0","name":"tag","value":42}})");
        REQUIRE(setDoc["ok"].GetBool());
        CHECK_EQ(42, f.childA->getTag());
        // node.set echoes the node's path after the write.
        CHECK_EQ(std::string("/0"), std::string(setDoc["result"]["path"].GetString()));

        auto getDoc = sendRequest(handler, R"({"id":2,"method":"node.get","params":{"path":"/0","name":"tag"}})");
        REQUIRE(getDoc["ok"].GetBool());
        CHECK_EQ(std::string("tag"), std::string(getDoc["result"]["name"].GetString()));
        CHECK_EQ(std::string("int"), std::string(getDoc["result"]["type"].GetString()));
        CHECK_EQ(42, getDoc["result"]["value"].GetInt());
    }

    // Paths index SORTED child order, so writing localZOrder re-sorts the siblings and the node
    // moves to a different path. A client that kept using the old path would silently read a
    // different sibling - which is why node.set returns the post-write path.
    TEST_CASE("node_set_localZOrder_can_change_the_nodes_path")
    {
        Fixture f;
        FakeSceneAccess access;
        access.scene = f.root;
        AgentRequestHandler handler(&access);

        // childA is at "/0" and childB at "/1" while both have z == 0.
        REQUIRE_EQ(0, f.childA->getLocalZOrder());

        auto setDoc = sendRequest(
            handler, R"({"id":1,"method":"node.set","params":{"path":"/0","name":"localZOrder","value":42}})");
        REQUIRE(setDoc["ok"].GetBool());
        CHECK_EQ(42, f.childA->getLocalZOrder());

        // Raising childA's z sorts it after childB, so childA is now "/1", not "/0".
        CHECK_EQ(std::string("/1"), std::string(setDoc["result"]["path"].GetString()));

        // Following the returned path reads the node we actually wrote to ...
        auto followed =
            sendRequest(handler, R"({"id":2,"method":"node.get","params":{"path":"/1","name":"localZOrder"}})");
        REQUIRE(followed["ok"].GetBool());
        CHECK_EQ(42, followed["result"]["value"].GetInt());

        // ... whereas reusing the stale path quietly lands on the sibling.
        auto stale = sendRequest(handler, R"({"id":3,"method":"node.get","params":{"path":"/0","name":"localZOrder"}})");
        REQUIRE(stale["ok"].GetBool());
        CHECK_EQ(0, stale["result"]["value"].GetInt());
    }

    // 10. node.set with a wrong JSON type -> type_mismatch, node unchanged.
    TEST_CASE("node_set_wrong_type_yields_type_mismatch_and_leaves_node_unchanged")
    {
        Fixture f;
        f.childA->setVisible(true);
        FakeSceneAccess access;
        access.scene = f.root;
        AgentRequestHandler handler(&access);

        // "visible" is a Bool property - a number must be refused, never coerced.
        auto doc = sendRequest(handler, R"({"id":1,"method":"node.set","params":{"path":"/0","name":"visible","value":3}})");
        REQUIRE_FALSE(doc["ok"].GetBool());
        CHECK_EQ(std::string("type_mismatch"), std::string(doc["error"]["code"].GetString()));
        CHECK(f.childA->isVisible());
    }

    // 11. node.set on a read-only property -> readonly_property.
    TEST_CASE("node_set_readonly_property_yields_readonly_property")
    {
        Fixture f;
        FakeSceneAccess access;
        access.scene = f.root;
        AgentRequestHandler handler(&access);

        REQUIRE(NodeReflection::getInstance()->addProvider(kReadOnlyProviderId, std::make_unique<ReadOnlyStubProvider>()));

        auto doc = sendRequest(handler,
                                R"({"id":1,"method":"node.set","params":{"path":"/0","name":"agentTestReadOnly","value":1}})");
        REQUIRE_FALSE(doc["ok"].GetBool());
        CHECK_EQ(std::string("readonly_property"), std::string(doc["error"]["code"].GetString()));

        NodeReflection::getInstance()->removeProvider(kReadOnlyProviderId);
    }

    // 12. Vec2 / Color4B encode and decode symmetrically (round-trip through set -> get).
    TEST_CASE("vec2_and_color_round_trip_through_set_then_get")
    {
        Fixture f;
        FakeSceneAccess access;
        access.scene = f.root;
        AgentRequestHandler handler(&access);

        SUBCASE("Vec2 (position)")
        {
            auto setDoc = sendRequest(
                handler, R"({"id":1,"method":"node.set","params":{"path":"/0","name":"position","value":{"x":11.5,"y":-22.25}}})");
            REQUIRE(setDoc["ok"].GetBool());

            auto getDoc = sendRequest(handler, R"({"id":2,"method":"node.get","params":{"path":"/0","name":"position"}})");
            REQUIRE(getDoc["ok"].GetBool());
            CHECK_EQ(std::string("vec2"), std::string(getDoc["result"]["type"].GetString()));
            CHECK_EQ(getDoc["result"]["value"]["x"].GetFloat(), doctest::Approx(11.5f));
            CHECK_EQ(getDoc["result"]["value"]["y"].GetFloat(), doctest::Approx(-22.25f));
        }
        SUBCASE("Color4B (color), including alpha")
        {
            auto setDoc = sendRequest(
                handler,
                R"({"id":1,"method":"node.set","params":{"path":"/0","name":"color","value":{"r":10,"g":20,"b":30,"a":128}}})");
            REQUIRE(setDoc["ok"].GetBool());

            auto getDoc = sendRequest(handler, R"({"id":2,"method":"node.get","params":{"path":"/0","name":"color"}})");
            REQUIRE(getDoc["ok"].GetBool());
            CHECK_EQ(std::string("color"), std::string(getDoc["result"]["type"].GetString()));
            CHECK_EQ(10, getDoc["result"]["value"]["r"].GetInt());
            CHECK_EQ(20, getDoc["result"]["value"]["g"].GetInt());
            CHECK_EQ(30, getDoc["result"]["value"]["b"].GetInt());
            CHECK_EQ(128, getDoc["result"]["value"]["a"].GetInt());
        }
    }

    // 13. input.tap / input.swipe reach SceneAccess with the exact coordinates.
    TEST_CASE("input_tap_and_swipe_reach_scene_access_with_exact_coordinates")
    {
        FakeSceneAccess access;
        AgentRequestHandler handler(&access);

        auto tapDoc = sendRequest(handler, R"({"id":1,"method":"input.tap","params":{"x":12.5,"y":34.5}})");
        REQUIRE(tapDoc["ok"].GetBool());
        CHECK(access.tapCalled);
        CHECK_EQ(access.tapX, doctest::Approx(12.5f));
        CHECK_EQ(access.tapY, doctest::Approx(34.5f));

        auto swipeDoc = sendRequest(handler, R"({"id":2,"method":"input.swipe","params":{"x1":1,"y1":2,"x2":3,"y2":4}})");
        REQUIRE(swipeDoc["ok"].GetBool());
        CHECK(access.swipeCalled);
        CHECK_EQ(access.swipeX1, doctest::Approx(1.0f));
        CHECK_EQ(access.swipeY1, doctest::Approx(2.0f));
        CHECK_EQ(access.swipeX2, doctest::Approx(3.0f));
        CHECK_EQ(access.swipeY2, doctest::Approx(4.0f));
    }

    // 14. screenshot with a traversing (or absolute) path -> invalid_params.
    TEST_CASE("screenshot_with_escaping_path_yields_invalid_params")
    {
        FakeSceneAccess access;
        AgentRequestHandler handler(&access);

        SUBCASE("\"..\" traversal")
        {
            auto doc = sendRequest(handler, R"({"id":1,"method":"screenshot","params":{"file":"../../evil.png"}})");
            REQUIRE_FALSE(doc["ok"].GetBool());
            CHECK_EQ(std::string("invalid_params"), std::string(doc["error"]["code"].GetString()));
        }
        SUBCASE("absolute path")
        {
            auto doc = sendRequest(handler, R"({"id":1,"method":"screenshot","params":{"file":"/etc/passwd"}})");
            REQUIRE_FALSE(doc["ok"].GetBool());
            CHECK_EQ(std::string("invalid_params"), std::string(doc["error"]["code"].GetString()));
        }
        // Neither rejected form should ever have reached SceneAccess.
        CHECK(access.lastScreenshotFile.empty());
    }

    TEST_CASE("screenshot_success_returns_the_captured_path")
    {
        FakeSceneAccess access;
        access.screenshotPath = "/writable/shot.png";
        AgentRequestHandler handler(&access);

        auto doc = sendRequest(handler, R"({"id":1,"method":"screenshot"})");
        REQUIRE(doc["ok"].GetBool());
        CHECK_EQ(std::string("/writable/shot.png"), std::string(doc["result"]["path"].GetString()));
    }

    TEST_CASE("screenshot_capture_failure_yields_internal_error")
    {
        FakeSceneAccess access;
        access.screenshotShouldFail = true;
        AgentRequestHandler handler(&access);

        auto doc = sendRequest(handler, R"({"id":1,"method":"screenshot"})");
        REQUIRE_FALSE(doc["ok"].GetBool());
        CHECK_EQ(std::string("internal_error"), std::string(doc["error"]["code"].GetString()));
    }

    // 15. A request missing a required param -> invalid_params.
    TEST_CASE("missing_required_param_yields_invalid_params")
    {
        Fixture f;
        FakeSceneAccess access;
        access.scene = f.root;
        AgentRequestHandler handler(&access);

        SUBCASE("node.describe without \"path\"")
        {
            auto doc = sendRequest(handler, R"({"id":1,"method":"node.describe"})");
            REQUIRE_FALSE(doc["ok"].GetBool());
            CHECK_EQ(std::string("invalid_params"), std::string(doc["error"]["code"].GetString()));
        }
        SUBCASE("node.get without \"name\"")
        {
            auto doc = sendRequest(handler, R"({"id":1,"method":"node.get","params":{"path":"/0"}})");
            REQUIRE_FALSE(doc["ok"].GetBool());
            CHECK_EQ(std::string("invalid_params"), std::string(doc["error"]["code"].GetString()));
        }
        SUBCASE("node.set without \"value\"")
        {
            auto doc = sendRequest(handler, R"({"id":1,"method":"node.set","params":{"path":"/0","name":"visible"}})");
            REQUIRE_FALSE(doc["ok"].GetBool());
            CHECK_EQ(std::string("invalid_params"), std::string(doc["error"]["code"].GetString()));
        }
        SUBCASE("input.tap without \"y\"")
        {
            auto doc = sendRequest(handler, R"({"id":1,"method":"input.tap","params":{"x":1}})");
            REQUIRE_FALSE(doc["ok"].GetBool());
            CHECK_EQ(std::string("invalid_params"), std::string(doc["error"]["code"].GetString()));
        }
    }

    // --- Additional coverage beyond the numbered list, for the remaining methods/paths. ---

    TEST_CASE("director_pause_and_resume_forward_to_scene_access")
    {
        FakeSceneAccess access;
        AgentRequestHandler handler(&access);

        auto pauseDoc = sendRequest(handler, R"({"id":1,"method":"director.pause"})");
        REQUIRE(pauseDoc["ok"].GetBool());
        CHECK(access.paused);

        auto resumeDoc = sendRequest(handler, R"({"id":2,"method":"director.resume"})");
        REQUIRE(resumeDoc["ok"].GetBool());
        CHECK_FALSE(access.paused);
    }

    TEST_CASE("app_info_reports_scene_access_values")
    {
        FakeSceneAccess access;
        access.engineVersion = "9.9.9-test";
        access.paused        = true;
        AgentRequestHandler handler(&access);

        auto doc = sendRequest(handler, R"({"id":1,"method":"app.info"})");
        REQUIRE(doc["ok"].GetBool());
        CHECK_EQ(std::string("9.9.9-test"), std::string(doc["result"]["engineVersion"].GetString()));
        CHECK(doc["result"]["paused"].GetBool());
        CHECK_EQ(doc["result"]["fps"].GetFloat(), doctest::Approx(access.frameRate));
        CHECK_EQ(doc["result"]["designResolution"]["x"].GetFloat(), doctest::Approx(access.designResolution.x));
        CHECK_EQ(doc["result"]["frameSize"]["y"].GetFloat(), doctest::Approx(access.frameSize.y));
    }

    TEST_CASE("node_properties_reports_name_type_and_writability")
    {
        Fixture f;
        FakeSceneAccess access;
        access.scene = f.root;
        AgentRequestHandler handler(&access);

        REQUIRE(NodeReflection::getInstance()->addProvider(kReadOnlyProviderId, std::make_unique<ReadOnlyStubProvider>()));

        auto doc = sendRequest(handler, R"({"id":1,"method":"node.properties","params":{"path":"/0"}})");
        REQUIRE(doc["ok"].GetBool());

        bool foundWritable = false;
        bool foundReadOnly = false;
        for (auto& entry : doc["result"].GetArray())
        {
            const std::string name(entry["name"].GetString());
            if (name == "visible")
            {
                foundWritable = true;
                CHECK(entry["writable"].GetBool());
                CHECK_EQ(std::string("bool"), std::string(entry["type"].GetString()));
            }
            else if (name == "agentTestReadOnly")
            {
                foundReadOnly = true;
                CHECK_FALSE(entry["writable"].GetBool());
                CHECK_EQ(std::string("int"), std::string(entry["type"].GetString()));
            }
        }
        CHECK(foundWritable);
        CHECK(foundReadOnly);

        NodeReflection::getInstance()->removeProvider(kReadOnlyProviderId);
    }

    TEST_CASE("params_that_is_not_an_object_yields_invalid_params")
    {
        FakeSceneAccess access;
        AgentRequestHandler handler(&access);

        auto doc = sendRequest(handler, R"({"id":1,"method":"app.info","params":[1,2,3]})");
        REQUIRE_FALSE(doc["ok"].GetBool());
        CHECK_EQ(std::string("invalid_params"), std::string(doc["error"]["code"].GetString()));
    }

    TEST_CASE("node_get_and_set_without_running_scene_yield_no_scene")
    {
        FakeSceneAccess access;  // scene left null
        AgentRequestHandler handler(&access);

        auto getDoc = sendRequest(handler, R"({"id":1,"method":"node.get","params":{"path":"/0","name":"visible"}})");
        REQUIRE_FALSE(getDoc["ok"].GetBool());
        CHECK_EQ(std::string("no_scene"), std::string(getDoc["error"]["code"].GetString()));

        auto setDoc =
            sendRequest(handler, R"({"id":2,"method":"node.set","params":{"path":"/0","name":"visible","value":true}})");
        REQUIRE_FALSE(setDoc["ok"].GetBool());
        CHECK_EQ(std::string("no_scene"), std::string(setDoc["error"]["code"].GetString()));
    }
}
