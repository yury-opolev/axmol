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

// Exercises AgentBridge::extractLines() - the pure, socket-free heart of the bridge's
// newline-delimited-JSON framing (see AgentBridge.h's doc comment on it) - directly, with no TCP
// connection, no running engine, and no AgentBridge instance (it is static). AgentBridge itself
// has no other unit-testable seam: everything else it does (accept/read loop, thread marshalling,
// timeouts) genuinely needs a socket and the Axmol main thread, which is exactly why this
// function was pulled out on its own.

#include <doctest.h>

#include "base/AgentBridge.h"

#include <string>
#include <vector>

using namespace ax;

TEST_SUITE("core/base/AgentBridgeFraming")
{
    TEST_CASE("a_request_split_across_two_appends_is_not_returned_until_complete")
    {
        std::string buffer = R"({"id":1,"method":"a)";
        std::vector<std::string> lines;

        REQUIRE(AgentBridge::extractLines(buffer, 1024, lines));
        CHECK(lines.empty());
        CHECK_EQ(std::string(R"({"id":1,"method":"a)"), buffer);

        buffer += "pp.info\"}\n";
        REQUIRE(AgentBridge::extractLines(buffer, 1024, lines));
        REQUIRE_EQ(1u, lines.size());
        CHECK_EQ(std::string(R"({"id":1,"method":"app.info"})"), lines[0]);
        CHECK(buffer.empty());
    }

    TEST_CASE("several_requests_in_one_append_are_all_returned_in_order")
    {
        std::string buffer = "{\"id\":1}\n{\"id\":2}\n{\"id\":3}\n";
        std::vector<std::string> lines;

        REQUIRE(AgentBridge::extractLines(buffer, 1024, lines));
        REQUIRE_EQ(3u, lines.size());
        CHECK_EQ(std::string("{\"id\":1}"), lines[0]);
        CHECK_EQ(std::string("{\"id\":2}"), lines[1]);
        CHECK_EQ(std::string("{\"id\":3}"), lines[2]);
        CHECK(buffer.empty());
    }

    // A telnet or PowerShell client sends CRLF line endings - the '\r' must be stripped before
    // the JSON is ever parsed, not left dangling on the front of the next call's input.
    TEST_CASE("crlf_line_endings_have_the_trailing_cr_stripped")
    {
        std::string buffer = "{\"id\":1}\r\n{\"id\":2}\r\n";
        std::vector<std::string> lines;

        REQUIRE(AgentBridge::extractLines(buffer, 1024, lines));
        REQUIRE_EQ(2u, lines.size());
        CHECK_EQ(std::string("{\"id\":1}"), lines[0]);
        CHECK_EQ(std::string("{\"id\":2}"), lines[1]);
    }

    TEST_CASE("empty_lines_are_skipped_rather_than_returned")
    {
        std::string buffer = "\r\n\n{\"id\":1}\n\n\r\n";
        std::vector<std::string> lines;

        REQUIRE(AgentBridge::extractLines(buffer, 1024, lines));
        REQUIRE_EQ(1u, lines.size());
        CHECK_EQ(std::string("{\"id\":1}"), lines[0]);
        CHECK(buffer.empty());
    }

    TEST_CASE("size_cap_boundary")
    {
        SUBCASE("exactly at the cap, with no newline yet, is still allowed to keep accumulating")
        {
            std::string buffer(10, 'x');
            std::vector<std::string> lines;

            CHECK(AgentBridge::extractLines(buffer, 10, lines));
            CHECK(lines.empty());
            CHECK_EQ(10u, buffer.size());
        }
        SUBCASE("one byte over the cap, with no newline yet, is rejected")
        {
            std::string buffer(11, 'x');
            std::vector<std::string> lines;

            CHECK_FALSE(AgentBridge::extractLines(buffer, 10, lines));
        }
        SUBCASE("a completed line exactly at the cap is fine - the cap only bounds the "
                "unterminated remainder")
        {
            std::string buffer(9, 'x');
            buffer.push_back('\n');
            std::vector<std::string> lines;

            CHECK(AgentBridge::extractLines(buffer, 10, lines));
            REQUIRE_EQ(1u, lines.size());
            CHECK(buffer.empty());
        }
    }

    // JSON escapes an embedded newline inside a string as the TWO characters '\' and 'n' - never
    // a raw 0x0A byte - so content like this must never be split mid-string by a splitter that
    // only looks for raw newline bytes, which is exactly what extractLines() is.
    TEST_CASE("an_escaped_newline_inside_a_json_string_does_not_split_the_line_early")
    {
        const std::string request =
            R"({"id":1,"method":"node.set","params":{"value":"line1\nline2"}})";
        std::string buffer = request + "\n";
        std::vector<std::string> lines;

        REQUIRE(AgentBridge::extractLines(buffer, 1024, lines));
        REQUIRE_EQ(1u, lines.size());
        CHECK_EQ(request, lines[0]);
        CHECK(buffer.empty());
    }

    TEST_CASE("an_incomplete_trailing_line_remains_in_the_buffer_for_the_next_call")
    {
        std::string buffer = "{\"id\":1}\n{\"id\":2";
        std::vector<std::string> lines;

        REQUIRE(AgentBridge::extractLines(buffer, 1024, lines));
        REQUIRE_EQ(1u, lines.size());
        CHECK_EQ(std::string("{\"id\":1}"), lines[0]);
        CHECK_EQ(std::string("{\"id\":2"), buffer);

        buffer += "}\n";
        lines.clear();
        REQUIRE(AgentBridge::extractLines(buffer, 1024, lines));
        REQUIRE_EQ(1u, lines.size());
        CHECK_EQ(std::string("{\"id\":2}"), lines[0]);
        CHECK(buffer.empty());
    }
}
