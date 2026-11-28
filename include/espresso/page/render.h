/// @file
/// A page's layout drawn as HTML (`dsl-ui.md` §7.1): the document the page
/// bridge serves when a project has no `ui/index.html` of its own. Each
/// element becomes the `es-*` element its kind names (`metadata::pageKinds()`),
/// with its parameters as that element's attributes, and the document loads
/// `/espresso.js` and `/espresso-ui.js`.

#pragma once

#include <string>
#include <string_view>

#include <espresso/metadata/graph.h>
#include <espresso/metadata/pageSpec.h>

/// The page's layout drawn as HTML (the `page` module).
namespace page {

/// The address on which the server tells open pages to load the page
/// again, when an Update changes it.
inline constexpr std::string_view reloadAddress = metadata::pageReloadAddress;

/// The document that draws @p layout, a tree whose root is a `page`; the
/// same layout gives the same text. Parameters its kind does not have are
/// left out. A @p served page, the one the server serves, reloads when the
/// server sends `reloadAddress`, and its value controls keep the values the
/// player set across the reload. A page written into a project (`--page`,
/// Save Page) does not follow later changes to the layout, so it does
/// neither.
std::string render(const metadata::pageNode& layout, bool served = true);

/// @p text with `&`, `<`, `>`, and `"` written as character references, for
/// HTML text and attribute values.
std::string escaped(std::string_view text);

/// @p v in the fewest digits that read back as the same double.
std::string shortest(double v);

}  // namespace page
