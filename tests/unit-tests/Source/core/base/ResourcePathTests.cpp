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

#include "base/ResourcePath.h"

using namespace ax;

TEST_SUITE("core/base/ResourcePath")
{
    TEST_CASE("ordinary asset names are allowed")
    {
        // Whatever else this rejects, the names the tooling actually produces must pass.
        CHECK_FALSE(pathEscapesWritableSandbox("HelloWorld.png"));
        CHECK_FALSE(pathEscapesWritableSandbox("axmol-mcp-1700000000-1.png"));
        CHECK_FALSE(pathEscapesWritableSandbox("scenes/level1.scene.json"));
        CHECK_FALSE(pathEscapesWritableSandbox("a/b/c/d.png"));
        CHECK_FALSE(pathEscapesWritableSandbox("under_score-and.dots.png"));
    }

    TEST_CASE("an interior space is allowed, because axmol's own assets have one")
    {
        // Found by the scene round-trip smoke test, not by inspection: a Label serialises its
        // fontName, and the default is "fonts/Marker Felt.ttf". A rule strict enough to reject the
        // engine's own default font makes every label unsaveable, which is how a security check
        // ends up being switched off.
        CHECK_FALSE(pathEscapesWritableSandbox("fonts/Marker Felt.ttf"));
        CHECK_FALSE(pathEscapesWritableSandbox("my sprites/hero idle.png"));
        // The HD-suffix convention, for the same reason: it is ubiquitous in game assets and is
        // not a metacharacter anywhere this path can end up.
        CHECK_FALSE(pathEscapesWritableSandbox("ui/logo@2x.png"));
    }

    TEST_CASE("a non-ASCII name is refused, which is a known limitation rather than a rule")
    {
        // Pinned so the behaviour is a decision rather than an accident. Scene loading is ungated,
        // so this constrains shipped games too: widening it means answering what a safe Unicode
        // filename is (normalisation, bidi overrides, homoglyphs), which is deliberately deferred.
        CHECK(pathEscapesWritableSandbox("h\xC3\xA9ros.png"));
    }

    TEST_CASE("a leading or trailing space in any segment is still refused")
    {
        // Allowing the interior space must not reopen the shadowing trick: " a.png" and "a.png"
        // are indistinguishable in a listing, and Windows strips the trailing one outright.
        CHECK(pathEscapesWritableSandbox(" hero.png"));
        CHECK(pathEscapesWritableSandbox("fonts/ Marker Felt.ttf"));
        CHECK(pathEscapesWritableSandbox("fonts /Marker.ttf"));
    }

    TEST_CASE("an empty path is not an escape")
    {
        // Callers treat empty as "use a default name".
        CHECK_FALSE(pathEscapesWritableSandbox(""));
    }

    TEST_CASE("traversal and absolute paths are refused")
    {
        for (const char* path : {"../evil.png", "../../evil.png", "sub/../../x.png", "a/../b", "..",
                                 "/etc/passwd", "/tmp/x.png"})
        {
            INFO("path: " << path);
            CHECK(pathEscapesWritableSandbox(path));
        }
    }

    TEST_CASE("windows-specific absolute forms are refused")
    {
        for (const char* path : {"C:/windows/evil.png", "c:evil.png", "\\\\server\\share\\evil.png",
                                 "sub\\evil.png"})
        {
            INFO("path: " << path);
            CHECK(pathEscapesWritableSandbox(path));
        }
    }

    TEST_CASE("shell metacharacters are refused")
    {
        // A name written here can later be handed to a shell by something else - `adb shell`
        // re-parses its arguments - so the safe set is an allowlist, not a list of known-bad
        // patterns.
        // A bare space is deliberately absent from this list - see the interior-space case above.
        // Every entry here still fails on a character that is not a space.
        for (const char* path : {"a; whoami #.png", "a && rm -rf x", "a|b", "a`id`", "a$(id)", "a'b'",
                                 "a\"b\"", "a\nb", "a*b", "a?b", "a&b", "a>b", "a<b"})
        {
            INFO("path: " << path);
            CHECK(pathEscapesWritableSandbox(path));
        }
    }

    TEST_CASE("an alternate data stream cannot be created")
    {
        // It stays inside the sandbox, but is invisible to a directory listing - stealth storage
        // is not something a debug tool should be able to hand an attacker.
        CHECK(pathEscapesWritableSandbox("shot.png:hidden"));
        CHECK(pathEscapesWritableSandbox("a/shot.png:hidden:$DATA"));
    }

    TEST_CASE("reserved device names are refused, with or without an extension")
    {
        // "CON .png" is in this list because allowing the interior space briefly let it through:
        // the stem became "CON " and no longer compared equal to "CON".
        for (const char* path : {"CON", "nul", "NUL.png", "com1.txt", "LPT9", "a/CON/b.png", "aux.scene.json",
                                 "CON .png", "com1 .txt", "a/NUL /b.png"})
        {
            INFO("path: " << path);
            CHECK(pathEscapesWritableSandbox(path));
        }
        // ...but a name that merely starts with one is a perfectly good file.
        CHECK_FALSE(pathEscapesWritableSandbox("console.png"));
        CHECK_FALSE(pathEscapesWritableSandbox("nullable.json"));
    }

    TEST_CASE("trailing dots and spaces are refused")
    {
        // Windows strips them silently, so "evil.png." and "evil.png" are the same file - which
        // makes a check on the spelled name meaningless unless the spelling is constrained.
        CHECK(pathEscapesWritableSandbox("evil.png."));
        CHECK(pathEscapesWritableSandbox("evil.png "));
        CHECK(pathEscapesWritableSandbox("a./b.png"));
    }

    TEST_CASE("control characters are refused")
    {
        CHECK(pathEscapesWritableSandbox(std::string_view("a\0b.png", 7)));
        CHECK(pathEscapesWritableSandbox("a\tb.png"));
        CHECK(pathEscapesWritableSandbox("a\rb.png"));
    }
}
