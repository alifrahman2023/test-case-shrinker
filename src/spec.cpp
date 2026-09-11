#include <shrink/spec.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <utility>

namespace shrink {
namespace {

struct Token {
  enum class Kind { End, Ident, Int, Semi, LBrack, RBrack, LBrace, RBrace, Comma, Colon, DotDot };

  Kind kind{Kind::End};
  std::string text;
  int64_t value{0};
  std::size_t pos{0};
};

bool ident_start(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}
bool ident_body(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

class Lexer {
 public:
  explicit Lexer(std::string_view src) : src_(src) {}

  Token next() {
    while (i_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[i_])) != 0) ++i_;
    Token t;
    t.pos = i_;
    if (i_ >= src_.size()) return t;

    const char c = src_[i_];
    switch (c) {
      case ';':
        ++i_;
        t.kind = Token::Kind::Semi;
        return t;
      case '[':
        ++i_;
        t.kind = Token::Kind::LBrack;
        return t;
      case ']':
        ++i_;
        t.kind = Token::Kind::RBrack;
        return t;
      case '{':
        ++i_;
        t.kind = Token::Kind::LBrace;
        return t;
      case '}':
        ++i_;
        t.kind = Token::Kind::RBrace;
        return t;
      case ',':
        ++i_;
        t.kind = Token::Kind::Comma;
        return t;
      case ':':
        ++i_;
        t.kind = Token::Kind::Colon;
        return t;
      default:
        break;
    }
    if (c == '.' && i_ + 1 < src_.size() && src_[i_ + 1] == '.') {
      i_ += 2;
      t.kind = Token::Kind::DotDot;
      return t;
    }
    if (ident_start(c)) {
      const std::size_t start = i_;
      while (i_ < src_.size() && ident_body(src_[i_])) ++i_;
      t.kind = Token::Kind::Ident;
      t.text = std::string{src_.substr(start, i_ - start)};
      return t;
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
      const std::size_t start = i_;
      if (c == '-') ++i_;
      while (i_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[i_])) != 0) ++i_;
      const std::string digits{src_.substr(start, i_ - start)};
      if (digits == "-") {
        t.text = digits;
        return t;  // End kind: caller reports "unexpected"
      }
      t.kind = Token::Kind::Int;
      t.text = digits;
      t.value = std::strtoll(digits.c_str(), nullptr, 10);
      return t;
    }
    t.text = std::string(1, c);  // End kind with text set: an unrecognised character
    ++i_;
    return t;
  }

 private:
  std::string_view src_;
  std::size_t i_{0};
};

const char* kind_name(Token::Kind k) {
  switch (k) {
    case Token::Kind::End:
      return "end of spec";
    case Token::Kind::Ident:
      return "a name";
    case Token::Kind::Int:
      return "a number";
    case Token::Kind::Semi:
      return "';'";
    case Token::Kind::LBrack:
      return "'['";
    case Token::Kind::RBrack:
      return "']'";
    case Token::Kind::LBrace:
      return "'{'";
    case Token::Kind::RBrace:
      return "'}'";
    case Token::Kind::Comma:
      return "','";
    case Token::Kind::Colon:
      return "':'";
    case Token::Kind::DotDot:
      return "'..'";
  }
  return "?";
}

std::string describe(const Token& t) {
  if (t.kind == Token::Kind::End && !t.text.empty()) return "'" + t.text + "'";
  if (t.kind == Token::Kind::Ident || t.kind == Token::Kind::Int) return "'" + t.text + "'";
  return kind_name(t.kind);
}

class SpecParser {
 public:
  explicit SpecParser(std::string_view src) : lex_(src) { advance(); }

  Parsed<Spec> parse() {
    scopes_.emplace_back();
    if (!parse_stmts(spec_.stmts)) return Parsed<Spec>::failure(error_);
    if (tok_.kind != Token::Kind::End) {
      return Parsed<Spec>::failure(at(tok_) + "unexpected " + describe(tok_));
    }
    if (spec_.stmts.empty()) return Parsed<Spec>::failure("the format spec is empty");
    scopes_.pop_back();
    finalize(spec_.stmts);
    return Parsed<Spec>::success(std::move(spec_));
  }

 private:
  void advance() { tok_ = lex_.next(); }

  static std::string at(const Token& t) { return "spec position " + std::to_string(t.pos) + ": "; }

  bool fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
    return false;
  }

  bool expect(Token::Kind k) {
    if (tok_.kind != k) {
      return fail(at(tok_) + "expected " + kind_name(k) + ", found " + describe(tok_));
    }
    advance();
    return true;
  }

  int declare(const std::string& name) {
    if (name == "in" || name == "int" || name == "str" || name == "char") {
      fail("'" + name + "' is a reserved word and cannot be a field name");
      return -1;
    }
    if (spec_.field_index(name) >= 0) {
      fail("field '" + name + "' is declared twice");
      return -1;
    }
    Field f;
    f.name = name;
    spec_.fields.push_back(std::move(f));
    return static_cast<int>(spec_.fields.size()) - 1;
  }

  /// Turn a previously declared scalar into the count of the statement being parsed. This
  /// is what removes it from the tree: from here on it is computed, never stored.
  bool use_as_size(const std::string& name, Size& out) {
    const int idx = spec_.field_index(name);
    if (idx < 0) return fail("size '" + name + "' is used before it is declared");
    auto& f = spec_.fields[static_cast<std::size_t>(idx)];
    const auto& scope = scopes_.back();
    if (std::find(scope.begin(), scope.end(), idx) == scope.end()) {
      return fail("size '" + name + "' must be declared in the same block as the thing it counts");
    }
    if (f.role == Field::Role::Derived) {
      return fail("'" + name +
                  "' already counts something else. Two collections cannot share one length "
                  "field, because deleting from one would silently corrupt the other");
    }
    if (f.type != Field::Type::Int) return fail("size '" + name + "' must be an int");
    f.role = Field::Role::Derived;
    out.field = idx;
    return true;
  }

  bool parse_size(Size& out) {
    if (tok_.kind == Token::Kind::Int) {
      if (tok_.value < 0) return fail(at(tok_) + "a size cannot be negative");
      out.fixed = tok_.value;
      advance();
      return true;
    }
    if (tok_.kind != Token::Kind::Ident) {
      return fail(at(tok_) + "expected a size, found " + describe(tok_));
    }
    const std::string name = tok_.text;
    advance();
    return use_as_size(name, out);
  }

  bool parse_type(Field::Type& out) {
    if (tok_.kind != Token::Kind::Ident) {
      return fail(at(tok_) + "expected a type after ':', found " + describe(tok_));
    }
    if (tok_.text == "int") {
      out = Field::Type::Int;
    } else if (tok_.text == "str") {
      out = Field::Type::Str;
    } else if (tok_.text == "char") {
      out = Field::Type::Char;
    } else {
      return fail(at(tok_) + "unknown type '" + tok_.text + "', expected int, str or char");
    }
    advance();
    return true;
  }

  /// `in lo..hi`. A bound naming a field marks that field as a label domain, which is what
  /// the Relabel pass needs to know it may compact vertex numbers.
  bool parse_bound(int64_t& literal, int& field) {
    if (tok_.kind == Token::Kind::Int) {
      literal = tok_.value;
      advance();
      return true;
    }
    if (tok_.kind != Token::Kind::Ident) {
      return fail(at(tok_) + "expected a bound, found " + describe(tok_));
    }
    const int idx = spec_.field_index(tok_.text);
    if (idx < 0) return fail("bound '" + tok_.text + "' is used before it is declared");
    auto& f = spec_.fields[static_cast<std::size_t>(idx)];
    // Derived wins: a field that is structurally a count stays one. The link recorded here
    // is what makes it a domain either way.
    if (f.role == Field::Role::Free) f.role = Field::Role::Domain;
    field = idx;
    advance();
    return true;
  }

  bool parse_range(const std::vector<int>& targets) {
    advance();  // 'in'
    int64_t lo = 0;
    int64_t hi = 0;
    int lo_field = -1;
    int hi_field = -1;
    if (!parse_bound(lo, lo_field)) return false;
    if (!expect(Token::Kind::DotDot)) return false;
    if (!parse_bound(hi, hi_field)) return false;
    for (const int t : targets) {
      auto& f = spec_.fields[static_cast<std::size_t>(t)];
      if (lo_field < 0) f.lo = lo;
      if (hi_field < 0) f.hi = hi;
      f.lo_field = lo_field;
      f.hi_field = hi_field;
    }
    return true;
  }

  bool at_range() const { return tok_.kind == Token::Kind::Ident && tok_.text == "in"; }

  bool parse_stmt(Stmt& out) {
    if (tok_.kind == Token::Kind::LBrace) return parse_tuple_array(out);

    if (tok_.kind != Token::Kind::Ident) {
      return fail(at(tok_) + "expected a field name, found " + describe(tok_));
    }
    const std::string name = tok_.text;
    advance();

    if (tok_.kind == Token::Kind::LBrace) return parse_repeat(name, out);
    if (tok_.kind == Token::Kind::LBrack) return parse_indexed(name, out);
    return parse_scalar(name, out);
  }

  bool parse_scalar(const std::string& name, Stmt& out) {
    const int idx = declare(name);
    if (idx < 0) return false;
    scopes_.back().push_back(idx);
    out.kind = Stmt::Kind::Scalar;
    out.fields = {idx};
    if (tok_.kind == Token::Kind::Colon) {
      advance();
      Field::Type type{};
      if (!parse_type(type)) return false;
      spec_.fields[static_cast<std::size_t>(idx)].type = type;
    }
    if (at_range()) return parse_range(out.fields);
    return true;
  }

  bool parse_repeat(const std::string& name, Stmt& out) {
    // `t { ... }` declares t and makes it the iteration count in one step, so t is Derived
    // from birth and never appears in the tree.
    const int idx = declare(name);
    if (idx < 0) return false;
    auto& f = spec_.fields[static_cast<std::size_t>(idx)];
    f.role = Field::Role::Derived;
    out.kind = Stmt::Kind::Repeat;
    out.size.field = idx;

    advance();  // '{'
    scopes_.emplace_back();
    if (!parse_stmts(out.body)) return false;
    scopes_.pop_back();
    if (!expect(Token::Kind::RBrace)) return false;
    if (out.body.empty()) return fail("the block '" + name + " { ... }' is empty");
    return true;
  }

  bool parse_indexed(const std::string& name, Stmt& out) {
    const int idx = declare(name);
    if (idx < 0) return false;
    out.fields = {idx};

    advance();  // '['
    if (!parse_size(out.size)) return false;
    if (!expect(Token::Kind::RBrack)) return false;

    if (tok_.kind == Token::Kind::LBrack) {  // a second dimension makes it a grid
      advance();
      if (!parse_size(out.cols)) return false;
      if (!expect(Token::Kind::RBrack)) return false;
      out.kind = Stmt::Kind::Grid;
      if (tok_.kind != Token::Kind::Colon) {
        return fail("grid '" + name + "' must be declared ':char'");
      }
      advance();
      Field::Type type{};
      if (!parse_type(type)) return false;
      if (type != Field::Type::Char) {
        return fail("grid '" + name + "' must be ':char'; int grids are not supported yet");
      }
      spec_.fields[static_cast<std::size_t>(idx)].type = Field::Type::Char;
      return true;
    }

    out.kind = Stmt::Kind::Array;
    if (tok_.kind == Token::Kind::Colon) {
      advance();
      Field::Type type{};
      if (!parse_type(type)) return false;
      spec_.fields[static_cast<std::size_t>(idx)].type = type;
    }
    if (at_range()) return parse_range(out.fields);
    return true;
  }

  bool parse_tuple_array(Stmt& out) {
    advance();  // '{'
    out.kind = Stmt::Kind::Array;
    for (;;) {
      if (tok_.kind != Token::Kind::Ident) {
        return fail(at(tok_) + "expected a component name, found " + describe(tok_));
      }
      const int idx = declare(tok_.text);
      if (idx < 0) return false;
      out.fields.push_back(idx);
      advance();
      if (tok_.kind == Token::Kind::Comma) {
        advance();
        continue;
      }
      break;
    }
    if (!expect(Token::Kind::RBrace)) return false;
    if (!expect(Token::Kind::LBrack)) return false;
    if (!parse_size(out.size)) return false;
    if (!expect(Token::Kind::RBrack)) return false;
    if (at_range()) return parse_range(out.fields);
    return true;
  }

  bool parse_stmts(std::vector<Stmt>& out) {
    for (;;) {
      if (tok_.kind == Token::Kind::End || tok_.kind == Token::Kind::RBrace) break;
      Stmt s;
      if (!parse_stmt(s)) return false;
      out.push_back(std::move(s));
      if (tok_.kind != Token::Kind::Semi) break;
      advance();
    }
    return true;
  }

  /// Assign each statement its slot among its scope's children, and record which statement
  /// each Derived field counts. Only possible now: a scalar becomes Derived retroactively,
  /// when a later statement uses it as a size.
  void finalize(std::vector<Stmt>& stmts) {
    int next = 0;
    for (std::size_t i = 0; i < stmts.size(); ++i) {
      Stmt& s = stmts[i];
      bool stores = true;
      if (s.kind == Stmt::Kind::Scalar) {
        stores = spec_.fields[static_cast<std::size_t>(s.fields[0])].stored();
      }
      s.kid_index = stores ? next++ : -1;
      const int here = static_cast<int>(i);
      if (s.size.field >= 0) spec_.fields[static_cast<std::size_t>(s.size.field)].len_of = here;
      if (s.cols.field >= 0) spec_.fields[static_cast<std::size_t>(s.cols.field)].len_of = here;
      if (s.kind == Stmt::Kind::Repeat) finalize(s.body);
    }
  }

  Lexer lex_;
  Token tok_;
  Spec spec_;
  std::string error_;
  std::vector<std::vector<int>> scopes_;  ///< field indices declared per block
};

const std::vector<std::pair<std::string, std::string>>& presets() {
  static const std::vector<std::pair<std::string, std::string>> kPresets = {
      {"array", "n; a[n]"},
      {"multi", "t { n; a[n] }"},
      {"graph", "n; m; {u,v}[m] in 1..n"},
      {"grid", "r; c; g[r][c]:char"},
      {"string", "n; s[n]:char"},
      {"scalars", "a; b; c"},
  };
  return kPresets;
}

}  // namespace

int Spec::field_index(std::string_view name) const {
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

int Spec::stored_count(const std::vector<Stmt>& stmts) {
  int n = 0;
  for (const auto& s : stmts) {
    if (s.stores()) ++n;
  }
  return n;
}

Parsed<Spec> parse_spec(std::string_view text) {
  return SpecParser{text}.parse();
}

std::string_view preset_spec(std::string_view name) {
  for (const auto& [key, dsl] : presets()) {
    if (key == name) return dsl;
  }
  return {};
}

const std::vector<std::string>& preset_names() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    for (const auto& [key, dsl] : presets()) out.push_back(key);
    return out;
  }();
  return names;
}

}  // namespace shrink
