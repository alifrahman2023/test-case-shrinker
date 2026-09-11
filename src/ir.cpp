#include <shrink/ir.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <utility>

namespace shrink {
namespace {

/// Whitespace-separated tokens, with line numbers so a mismatch can say where.
class InputLexer {
 public:
  explicit InputLexer(std::string_view src) : src_(src) {}

  bool next(std::string_view& out, std::size_t& line) {
    skip_space();
    if (i_ >= src_.size()) return false;
    const std::size_t start = i_;
    line = line_;
    while (i_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[i_])) == 0) ++i_;
    out = src_.substr(start, i_ - start);
    return true;
  }

  [[nodiscard]] std::size_t line() const { return line_; }

  [[nodiscard]] std::size_t remaining_tokens() {
    std::size_t n = 0;
    std::string_view tok;
    std::size_t line = 0;
    while (next(tok, line)) ++n;
    return n;
  }

 private:
  void skip_space() {
    while (i_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[i_])) != 0) {
      if (src_[i_] == '\n') ++line_;
      ++i_;
    }
  }

  std::string_view src_;
  std::size_t i_{0};
  std::size_t line_{1};
};

bool to_int(std::string_view text, int64_t& out) {
  if (text.empty()) return false;
  const std::string s{text};
  char* end = nullptr;
  errno = 0;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || end != s.c_str() + s.size()) return false;
  out = v;
  return true;
}

std::string where(std::size_t line) {
  return "line " + std::to_string(line) + ": ";
}

class InputParser {
 public:
  InputParser(const Spec& spec, std::string_view text) : spec_(spec), lex_(text) {}

  Parsed<Node> parse() {
    Node root;
    root.tag = Node::Tag::Seq;
    if (!parse_scope(spec_.stmts, root)) return Parsed<Node>::failure(error_);

    const std::size_t extra = lex_.remaining_tokens();
    if (extra > 0) {
      return Parsed<Node>::failure(
          "input has " + std::to_string(extra) +
          " token(s) left over after the format spec was satisfied. Does the spec match this "
          "file?");
    }
    return Parsed<Node>::success(std::move(root));
  }

 private:
  bool fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
    return false;
  }

  bool take(std::string_view what, std::string_view& tok, std::size_t& line) {
    if (!lex_.next(tok, line)) {
      return fail("input ended while reading " + std::string{what} +
                  ". Does the spec match this file?");
    }
    return true;
  }

  bool resolve(int field, int64_t& out) {
    const auto it = env_.find(field);
    if (it == env_.end()) {
      return fail("'" + spec_.field(field).name + "' is needed here but has not been read yet");
    }
    out = it->second;
    return true;
  }

  bool check_range(const Field& f, int64_t value, std::size_t line) {
    int64_t lo = f.lo;
    int64_t hi = f.hi;
    if (f.lo_field >= 0 && !resolve(f.lo_field, lo)) return false;
    if (f.hi_field >= 0 && !resolve(f.hi_field, hi)) return false;
    if (value < lo || value > hi) {
      return fail(where(line) + "'" + f.name + "' is " + std::to_string(value) +
                  ", outside the declared range " + std::to_string(lo) + ".." + std::to_string(hi));
    }
    return true;
  }

  bool read_count(const Size& size, std::string_view what, int64_t& out) {
    if (size.is_fixed()) {
      out = size.fixed;
      return true;
    }
    if (!resolve(size.field, out)) return false;
    if (out < 0) {
      return fail("'" + spec_.field(size.field).name + "' is negative, so " + std::string{what} +
                  " has no valid size");
    }
    return true;
  }

  /// One value of a field: an int (range-checked) or a token kept verbatim.
  bool read_value(int field, Node& out) {
    const Field& f = spec_.field(field);
    std::string_view tok;
    std::size_t line = 0;
    if (!take(f.name, tok, line)) return false;

    if (f.type == Field::Type::Int) {
      int64_t v = 0;
      if (!to_int(tok, v)) {
        return fail(where(line) + "'" + f.name + "' expects an integer, found '" +
                    std::string{tok} + "'");
      }
      if (!check_range(f, v, line)) return false;
      out.tag = Node::Tag::Int;
      out.ival = v;
    } else {
      if (f.type == Field::Type::Char && tok.size() != 1) {
        return fail(where(line) + "'" + f.name + "' expects a single character, found '" +
                    std::string{tok} + "'");
      }
      out.tag = Node::Tag::Str;
      out.sval = std::string{tok};
    }
    out.field = field;
    return true;
  }

  bool parse_scope(const std::vector<Stmt>& stmts, Node& seq) {
    seq.kids.reserve(static_cast<std::size_t>(Spec::stored_count(stmts)));
    for (const Stmt& s : stmts) {
      Node kid;
      if (!parse_stmt(s, kid)) return false;
      if (s.stores()) seq.kids.push_back(std::move(kid));
    }
    return true;
  }

  bool parse_stmt(const Stmt& s, Node& out) {
    switch (s.kind) {
      case Stmt::Kind::Scalar:
        return parse_scalar(s, out);
      case Stmt::Kind::Array:
        return parse_array(s, out);
      case Stmt::Kind::Grid:
        return parse_grid(s, out);
      case Stmt::Kind::Repeat:
        return parse_repeat(s, out);
    }
    return fail("internal: unknown statement kind");
  }

  bool parse_scalar(const Stmt& s, Node& out) {
    const int field = s.fields[0];
    if (!read_value(field, out)) return false;
    // Every scalar's value goes into the environment, whether or not it is stored: sizes
    // and range bounds both read from here.
    if (out.tag == Node::Tag::Int) env_[field] = out.ival;
    return true;
  }

  bool parse_array(const Stmt& s, Node& out) {
    const Field& elem = spec_.field(s.fields[0]);
    int64_t count = 0;
    if (!read_count(s.size, elem.name, count)) return false;

    out.tag = Node::Tag::Array;
    out.field = s.fields[0];

    // A char array arrives as one contiguous token, "abc", not as separate characters.
    if (s.fields.size() == 1 && elem.type == Field::Type::Char) {
      if (count == 0) return true;
      std::string_view tok;
      std::size_t line = 0;
      if (!take(elem.name, tok, line)) return false;
      if (static_cast<int64_t>(tok.size()) != count) {
        return fail(where(line) + "'" + elem.name + "' should be " + std::to_string(count) +
                    " characters, found " + std::to_string(tok.size()));
      }
      for (const char c : tok) {
        Node ch;
        ch.tag = Node::Tag::Str;
        ch.sval = std::string(1, c);
        ch.field = s.fields[0];
        out.kids.push_back(std::move(ch));
      }
      return true;
    }

    for (int64_t i = 0; i < count; ++i) {
      if (s.fields.size() == 1) {
        Node value;
        if (!read_value(s.fields[0], value)) return false;
        out.kids.push_back(std::move(value));
        continue;
      }
      Node record;
      record.tag = Node::Tag::Record;
      for (const int component : s.fields) {
        Node value;
        if (!read_value(component, value)) return false;
        record.kids.push_back(std::move(value));
      }
      out.kids.push_back(std::move(record));
    }
    return true;
  }

  /// A grid row is a single Str node, not an array of characters. That is deliberate: it
  /// makes ragged rows unrepresentable, so only GridLines, which edits every row together,
  /// can change a grid's width.
  bool parse_grid(const Stmt& s, Node& out) {
    const Field& g = spec_.field(s.fields[0]);
    int64_t rows = 0;
    int64_t cols = 0;
    if (!read_count(s.size, g.name, rows)) return false;
    if (!read_count(s.cols, g.name, cols)) return false;

    out.tag = Node::Tag::Array;
    out.field = s.fields[0];
    for (int64_t r = 0; r < rows; ++r) {
      std::string_view tok;
      std::size_t line = 0;
      if (!take("row " + std::to_string(r + 1) + " of '" + g.name + "'", tok, line)) return false;
      if (static_cast<int64_t>(tok.size()) != cols) {
        return fail(where(line) + "row " + std::to_string(r + 1) + " of '" + g.name +
                    "' should be " + std::to_string(cols) + " characters, found " +
                    std::to_string(tok.size()));
      }
      Node row;
      row.tag = Node::Tag::Str;
      row.sval = std::string{tok};
      row.field = s.fields[0];
      out.kids.push_back(std::move(row));
    }
    return true;
  }

  bool parse_repeat(const Stmt& s, Node& out) {
    const Field& counter = spec_.field(s.size.field);
    std::string_view tok;
    std::size_t line = 0;
    if (!take(counter.name, tok, line)) return false;
    int64_t count = 0;
    if (!to_int(tok, count)) {
      return fail(where(line) + "'" + counter.name + "' expects an integer, found '" +
                  std::string{tok} + "'");
    }
    if (count < 0) return fail(where(line) + "'" + counter.name + "' cannot be negative");
    env_[s.size.field] = count;

    out.tag = Node::Tag::Repeat;
    for (int64_t i = 0; i < count; ++i) {
      Node iteration;
      iteration.tag = Node::Tag::Seq;
      if (!parse_scope(s.body, iteration)) return false;
      out.kids.push_back(std::move(iteration));
    }
    return true;
  }

  const Spec& spec_;
  InputLexer lex_;
  std::string error_;
  std::map<int, int64_t> env_;
};

int64_t grid_cols(const Node& grid) {
  return grid.kids.empty() ? 0 : static_cast<int64_t>(grid.kids.front().sval.size());
}

class Serializer {
 public:
  explicit Serializer(const Spec& spec) : spec_(spec) {}

  void scope(const std::vector<Stmt>& stmts, const Node& seq) {
    // Derived counts are produced here, from the tree's actual shape, and nowhere else.
    // This is the whole reason no pass can desynchronise a length header.
    std::map<int, int64_t> counts;
    for (const Stmt& s : stmts) {
      if (!s.stores()) continue;
      const Node& kid = seq.kids[static_cast<std::size_t>(s.kid_index)];
      if (s.size.field >= 0) counts[s.size.field] = static_cast<int64_t>(kid.kids.size());
      if (s.cols.field >= 0) counts[s.cols.field] = grid_cols(kid);
    }

    for (const Stmt& s : stmts) {
      if (!s.stores()) {
        const auto it = counts.find(s.fields[0]);
        line(std::to_string(it == counts.end() ? 0 : it->second));
        continue;
      }
      emit(s, seq.kids[static_cast<std::size_t>(s.kid_index)]);
    }
  }

  std::string take() { return std::move(out_); }

 private:
  void line(const std::string& text) {
    out_ += text;
    out_ += '\n';
  }

  static std::string value(const Node& n) {
    return n.tag == Node::Tag::Int ? std::to_string(n.ival) : n.sval;
  }

  void emit(const Stmt& s, const Node& node) {
    switch (s.kind) {
      case Stmt::Kind::Scalar:
        line(value(node));
        return;

      case Stmt::Kind::Grid:
        for (const Node& row : node.kids) line(row.sval);
        return;

      case Stmt::Kind::Repeat:
        // A repeat declares its counter inline, so no scalar statement will emit it. The
        // count still comes from the tree's shape and nowhere else.
        line(std::to_string(node.kids.size()));
        for (const Node& iteration : node.kids) scope(s.body, iteration);
        return;

      case Stmt::Kind::Array: {
        const Field& elem = spec_.field(s.fields[0]);
        if (s.fields.size() == 1 && elem.type == Field::Type::Char) {
          std::string joined;
          for (const Node& c : node.kids) joined += c.sval;
          line(joined);
          return;
        }
        if (s.fields.size() > 1) {
          // One tuple per line. The plan's "arrays on one line" would put a whole edge list
          // on a single line, and the output being readable is half the point of the tool.
          for (const Node& record : node.kids) {
            std::string row;
            for (const Node& component : record.kids) {
              if (!row.empty()) row += ' ';
              row += value(component);
            }
            line(row);
          }
          return;
        }
        std::string row;
        for (const Node& element : node.kids) {
          if (!row.empty()) row += ' ';
          row += value(element);
        }
        line(row);
        return;
      }
    }
  }

  const Spec& spec_;
  std::string out_;
};

void dump_node(const Spec& spec, const Node& n, int depth, std::string& out) {
  out.append(static_cast<std::size_t>(depth) * 2, ' ');
  const char* tag = "?";
  switch (n.tag) {
    case Node::Tag::Seq:
      tag = "seq";
      break;
    case Node::Tag::Repeat:
      tag = "repeat";
      break;
    case Node::Tag::Array:
      tag = "array";
      break;
    case Node::Tag::Record:
      tag = "record";
      break;
    case Node::Tag::Int:
      tag = "int";
      break;
    case Node::Tag::Str:
      tag = "str";
      break;
  }
  out += tag;
  if (n.field >= 0) {
    out += " ";
    out += spec.field(n.field).name;
  }
  if (n.tag == Node::Tag::Int) out += " = " + std::to_string(n.ival);
  if (n.tag == Node::Tag::Str) out += " = \"" + n.sval + "\"";
  if (!n.kids.empty()) out += " (" + std::to_string(n.kids.size()) + ")";
  out += '\n';
  for (const Node& kid : n.kids) dump_node(spec, kid, depth + 1, out);
}

Node* mutable_at(Node& root, const Path& path) {
  Node* cur = &root;
  for (const int step : path) {
    if (step < 0 || static_cast<std::size_t>(step) >= cur->kids.size()) return nullptr;
    cur = &cur->kids[static_cast<std::size_t>(step)];
  }
  return cur;
}

void collect(const Node& n, Path& path, std::vector<Path>& out) {
  if (n.tag == Node::Tag::Array || n.tag == Node::Tag::Repeat) out.push_back(path);
  for (std::size_t i = 0; i < n.kids.size(); ++i) {
    path.push_back(static_cast<int>(i));
    collect(n.kids[i], path, out);
    path.pop_back();
  }
}

}  // namespace

Parsed<Node> parse_input(const Spec& spec, std::string_view text) {
  return InputParser{spec, text}.parse();
}

std::string serialize(const Spec& spec, const Node& root) {
  Serializer s{spec};
  s.scope(spec.stmts, root);
  return s.take();
}

std::string dump(const Spec& spec, const Node& root) {
  std::string out;
  dump_node(spec, root, 0, out);
  return out;
}

const Node* at(const Node& root, const Path& path) {
  return mutable_at(const_cast<Node&>(root), path);
}

Node erase_elems(const Node& root, const Path& path, const std::vector<int>& indices) {
  Node copy = root;
  Node* target = mutable_at(copy, path);
  if (target == nullptr) return copy;
  std::vector<int> sorted = indices;
  std::sort(sorted.begin(), sorted.end(), std::greater<>());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  for (const int i : sorted) {
    if (i >= 0 && static_cast<std::size_t>(i) < target->kids.size()) {
      target->kids.erase(target->kids.begin() + i);
    }
  }
  return copy;
}

Node keep_elems(const Node& root, const Path& path, const std::vector<int>& indices) {
  Node copy = root;
  Node* target = mutable_at(copy, path);
  if (target == nullptr) return copy;
  std::vector<char> keep(target->kids.size(), 0);
  for (const int i : indices) {
    if (i >= 0 && static_cast<std::size_t>(i) < keep.size()) keep[static_cast<std::size_t>(i)] = 1;
  }
  std::vector<Node> kept;
  for (std::size_t i = 0; i < target->kids.size(); ++i) {
    if (keep[i] != 0) kept.push_back(std::move(target->kids[i]));
  }
  target->kids = std::move(kept);
  return copy;
}

Node set_int(const Node& root, const Path& path, int64_t value) {
  Node copy = root;
  Node* target = mutable_at(copy, path);
  if (target != nullptr && target->tag == Node::Tag::Int) target->ival = value;
  return copy;
}

std::vector<Path> collection_paths(const Node& root) {
  std::vector<Path> out;
  Path path;
  collect(root, path, out);
  return out;
}

}  // namespace shrink
