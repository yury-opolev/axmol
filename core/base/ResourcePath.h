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

#pragma once

#include <string_view>

#include "base/Macros.h"

namespace ax
{

/** True if `path` must not be joined onto a resource or writable directory.

    ONE definition of the rule, in an UNGATED header on purpose. The agent bridge validates
    screenshot and scene-file names with it, and NodeFactory - which is compiled into every build,
    since a shipped game must be able to load a scene it saved - validates asset paths out of
    scene files with it. A scene file is untrusted data: without this, `texturePath` would accept
    an absolute path, and "render any image on this machine" plus a screenshot tool is a file
    disclosure primitive.

    An ALLOWLIST rather than a list of known-bad patterns: ASCII letters, digits, `.`, `_`, `-`,
    `@`, `/` as a separator, and the space. Everything else is refused, which additionally rules
    out

      - `..` traversal in any position, and absolute or drive-lettered paths;
      - backslashes, so there is one separator to reason about;
      - shell metacharacters, which matter because a name written here can later be handed to a
        shell by something else (`adb shell` re-parses its arguments);
      - `:` anywhere, so a Windows alternate data stream (`shot.png:hidden`) cannot be created -
        it stays inside the sandbox but is invisible to a directory listing;
      - control characters, and dots or spaces at either end of a segment, which Windows silently
        strips or renders invisible, so `evil.png.` and ` evil.png` both shadow `evil.png`;
      - reserved DOS device names (`CON`, `NUL`, `COM1`...), with or without an extension, and
        with or without the trailing spaces Windows ignores when resolving them.

    The space is allowed because axmol itself ships `fonts/Marker Felt.ttf`. A rule that cannot
    express the engine's own default font makes every Label unserialisable, and a check that
    blocks legitimate work is a check somebody switches off. A space alone injects nothing; it can
    only split an argument in code that failed to quote, and such code is already broken by
    `C:/Program Files`. Bounding it at the segment edges is what keeps the shadowing tricks out.
    `@` is allowed for `logo@2x.png`, the standard HD-suffix convention.

    KNOWN LIMITATION: ASCII only, so an accented or CJK asset name is refused. This is ungated
    code, so the constraint reaches shipped games and not only the debug tooling. Widening it means
    first deciding what a safe Unicode filename is - normalisation forms, bidi overrides,
    homoglyphs - which is a larger question than this function should settle by itself.

    Reserved DOS device names (CON, NUL, COM1, ...) are refused as well: on Windows those do not
    name files at all.

    An empty `path` never escapes - callers treat empty as "use a default name".

    Deliberately syntactic only, with no FileUtils or real-filesystem dependency, so
    AgentRequestHandler can call it while staying engine-free and unit-testable. */
AX_DLL bool pathEscapesWritableSandbox(std::string_view path);

}  // namespace ax
