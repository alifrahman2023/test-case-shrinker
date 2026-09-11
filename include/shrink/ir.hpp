#pragma once

#include <shrink/parsed.hpp>
#include <shrink/spec.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shrink {

/// A candidate input, as a tree. Value semantics throughout: copying a candidate is a deep
/// copy, which costs nothing next to the process spawn it is about to feed.
///
/// The load-bearing property is what is *absent*. A Derived field such as the `n` in
/// `n; a[n]` has no node here at all; it is emitted at serialize time as the array's size.
/// There is no length invariant, so no pass can break one.
struct Node {
  enum class Tag {
    Seq,     ///< a statement list: one kid per storing statement
    Repeat,  ///< kids are Seq, one per iteration
    Array,   ///< kids are elements: Int, Str, or Record
    Record,  ///< one tuple element; kids are its components
    Int,
    Str,  ///< also holds a single char, and a whole grid row
  };

  Tag tag{Tag::Seq};
  int64_t ival{};
  std::string sval;
  std::vector<Node> kids;
  int field{-1};  ///< index into Spec::fields, where one applies

  friend bool operator==(const Node&, const Node&) = default;
};

/// Position of a node in a tree: successive child indices from the root.
using Path = std::vector<int>;

/// Parse input text against a spec. The whole input must be consumed: leftover or missing
/// tokens are a hard error, because the alternative is confidently shrinking garbage.
[[nodiscard]] Parsed<Node> parse_input(const Spec& spec, std::string_view text);

/// Render a tree back to input text. Derived counts are computed here and nowhere else.
[[nodiscard]] std::string serialize(const Spec& spec, const Node& root);

/// Indented view of the tree, for --dry-run.
[[nodiscard]] std::string dump(const Spec& spec, const Node& root);

/// Structural edits. Each returns a new tree; none can leave a count stale, because no
/// count is stored.
[[nodiscard]] const Node* at(const Node& root, const Path& path);
[[nodiscard]] Node erase_elems(const Node& root, const Path& path, const std::vector<int>& indices);
[[nodiscard]] Node keep_elems(const Node& root, const Path& path, const std::vector<int>& indices);
[[nodiscard]] Node set_int(const Node& root, const Path& path, int64_t value);

/// Paths of every Array and Repeat node, in a deterministic pre-order. The passes walk
/// this rather than rediscovering the shape of the tree.
[[nodiscard]] std::vector<Path> collection_paths(const Node& root);

}  // namespace shrink
