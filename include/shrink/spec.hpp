#pragma once

#include <shrink/parsed.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace shrink {

/// One named quantity in the input format.
struct Field {
  enum class Role {
    Free,     ///< an ordinary value, stored in the tree
    Derived,  ///< a count. NOT stored: it is recomputed from the thing it measures
    Domain,   ///< stored, and it bounds a label range such as `in 1..n`
  };
  enum class Type { Int, Str, Char };

  std::string name;
  Role role{Role::Free};
  Type type{Type::Int};

  /// Derived only: index, within its own scope's statement list, of the array, grid or
  /// repeat whose size this field reports.
  int len_of{-1};

  /// Declared bounds from `in lo..hi`. A bound naming a field is resolved at parse time
  /// against that field's value, and the field index is kept here for the Relabel pass.
  int64_t lo{std::numeric_limits<int64_t>::min()};
  int64_t hi{std::numeric_limits<int64_t>::max()};
  int lo_field{-1};
  int hi_field{-1};

  [[nodiscard]] bool stored() const noexcept { return role != Role::Derived; }
  [[nodiscard]] bool bounded_below() const noexcept {
    return lo_field >= 0 || lo != std::numeric_limits<int64_t>::min();
  }
};

/// How many elements a repeat, array or grid dimension holds: either a literal written in
/// the spec, or a Derived field that reports the tree's actual size.
struct Size {
  int field{-1};      ///< Derived field reporting this count
  int64_t fixed{-1};  ///< literal count from the spec

  [[nodiscard]] bool is_fixed() const noexcept { return field < 0; }
};

/// One statement of the format. Statements are the structural template: parsing walks them
/// against the input's tokens, serializing walks them against the tree's children.
struct Stmt {
  enum class Kind { Scalar, Array, Grid, Repeat };

  Kind kind{Kind::Scalar};

  /// Scalar and Grid name one field. Array names one field, or several for a tuple such as
  /// `{u,v}[m]`, in which case each element is a Record.
  std::vector<int> fields;

  Size size;  ///< Array element count, Repeat iteration count, Grid row count
  Size cols;  ///< Grid column count

  std::vector<Stmt> body;  ///< Repeat only

  /// Index of this statement's node among its scope's children, or -1 when it stores
  /// nothing (a Derived scalar). Assigned once the whole spec is known, because a scalar
  /// only becomes Derived when a later statement uses it as a size.
  int kid_index{-1};

  [[nodiscard]] bool stores() const noexcept { return kid_index >= 0; }
};

struct Spec {
  std::vector<Field> fields;
  std::vector<Stmt> stmts;

  [[nodiscard]] int field_index(std::string_view name) const;
  [[nodiscard]] const Field& field(int index) const {
    return fields[static_cast<std::size_t>(index)];
  }
  /// Number of children a node holds for this statement list.
  [[nodiscard]] static int stored_count(const std::vector<Stmt>& stmts);
};

/// Parse the format DSL, e.g. "n; a[n]" or "n; m; {u,v}[m] in 1..n".
[[nodiscard]] Parsed<Spec> parse_spec(std::string_view text);

/// DSL text for a built-in preset name, or an empty view if the name is unknown.
[[nodiscard]] std::string_view preset_spec(std::string_view name);

/// Every preset name, in the order the help text lists them.
[[nodiscard]] const std::vector<std::string>& preset_names();

}  // namespace shrink
