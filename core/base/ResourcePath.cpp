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

#include "base/ResourcePath.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace ax
{

namespace
{

/// An allowlist, not a denylist: what is unusual about a hostile path is never the thing you
/// thought to ban.
///
/// A space IS allowed, reluctantly and deliberately. Axmol ships "fonts/Marker Felt.ttf", and a
/// rule that cannot express the engine's own default font is not a rule anyone will keep - it
/// would make labels unserialisable and push callers into disabling the check. A space cannot
/// inject anything on its own; it can only split an argument in code that failed to quote, and
/// such code is already broken by "C:/Program Files". Leading and trailing spaces are still
/// refused per segment below, which is where the actual Windows trickery lives.
///
/// `@` is allowed for the same reason: "logo@2x.png" is the standard HD-suffix convention, and it
/// is not a metacharacter in sh, cmd, or an `adb shell` argument.
///
/// KNOWN LIMITATION: this is ASCII-only, so an accented or CJK filename is refused. That is a real
/// constraint on a shipped game, not just on the debug tooling, because scene loading is ungated -
/// a game whose asset is named "hÃ©ros.png" cannot load its own scene file. Widening it means
/// deciding what a "safe" Unicode filename is (normalisation forms, bidi overrides, homoglyphs),
/// which is a bigger question than this function should answer on its own. Recorded rather than
/// silently endured: if it bites, that is the work.
bool isAllowedCharacter(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-' || c == '/' || c == ' ' || c == '@';
}

/// Names that do not refer to files at all on Windows, in any directory and with any extension.
bool isReservedDeviceName(std::string_view segment)
{
    static constexpr std::array<std::string_view, 22> kReserved{
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

    // The stem is what matters: "NUL.png" is still the NUL device.
    auto dot  = segment.find('.');
    auto stem = dot == std::string_view::npos ? segment : segment.substr(0, dot);

    // ...and trailing spaces in the stem are not part of the name Windows resolves, so "CON .png"
    // is still CON. Before an interior space was allowed this was unreachable; allowing one made
    // it the one way a reserved name could slip through, which is what a loosening costs if you
    // do not go back and re-check what the old rule was quietly enforcing.
    while (!stem.empty() && stem.back() == ' ')
        stem.remove_suffix(1);

    std::string upper(stem);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    return std::find(kReserved.begin(), kReserved.end(), upper) != kReserved.end();
}

}  // namespace

bool pathEscapesWritableSandbox(std::string_view path)
{
    if (path.empty())
        return false;  // "use a default name"

    for (char c : path)
    {
        if (!isAllowedCharacter(c))
            return true;
    }

    if (path.front() == '/')
        return true;  // absolute

    // Every segment is checked rather than just the leaf: "a/../../b" and "a/CON/b" are both
    // problems even though their last component looks harmless.
    size_t start = 0;
    while (start <= path.size())
    {
        const auto slash  = path.find('/', start);
        const auto end    = slash == std::string_view::npos ? path.size() : slash;
        const auto segment = path.substr(start, end - start);

        if (segment == "..")
            return true;
        if (!segment.empty())
        {
            // Windows strips these silently, so "evil.png." and "evil.png" name the same file.
            if (segment.back() == '.' || segment.back() == ' ')
                return true;
            // A leading space is the same trick from the other end: " logo.png" and "logo.png"
            // look identical in any listing, so one can shadow the other.
            if (segment.front() == ' ')
                return true;
            if (isReservedDeviceName(segment))
                return true;
        }

        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }

    return false;
}

}  // namespace ax
