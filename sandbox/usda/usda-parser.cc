#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stack>
#include <vector>
#include <map>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include <ryu/ryu.h>
#include <ryu/ryu_parse.h>

#include <nonstd/variant.hpp>

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <stream-reader.hh>

namespace tinyusdz {

struct ErrorDiagnositc {
  std::string err;
  int line_row = -1;
  int line_col = -1;
};

using Value = nonstd::variant<int, float, std::string>;

class Variable {
 public:
  std::string type;
  std::string name;
  Value value;

  Variable() = default;
  Variable(std::string ty, std::string n) : type(ty), name(n) {}
};

inline bool IsChar(char c) { return std::isalpha(int(c)); }

class USDAParser {
 public:
  struct ParseState {
    int64_t loc{-1};  // byte location in StreamReder
  };

  USDAParser(tinyusdz::StreamReader *sr) : _sr(sr) {
    _RegisterBuiltinMeta();
  }

  bool ReadToken(std::string *result) {
    std::stringstream ss;

    while (!_sr->eof()) {
      char c;
      if (!_sr->read1(&c)) {
        return false;
      }

      if (std::isspace(int(c))) {
        _sr->seek_from_current(-1);
        break;
      }

      ss << c;
      _line_col++;
    }

    (*result) = ss.str();

    return true;
  }

  bool ReadInt(int *value) {
    std::stringstream ss;

    std::cout << "ReadInt\n";

    // head character
    bool has_sign = false;
    bool negative = false;
    {
      char sc;
      if (!_sr->read1(&sc)) {
        return false;
      }
      _line_col++;

      // sign or [0-9]
      if (sc == '+') {
        negative = false;
        has_sign = true;
      } else if (sc == '-') {
        negative = true;
        has_sign = true;
      } else if ((sc >= '0') && (sc <= '9')) {
        // ok 
      } else {
        _PushError("Sign or 0-9 expected.\n");
        return false;
      }

      ss << sc;
    }

    while (!_sr->eof()) {
      char c;
      if (!_sr->read1(&c)) {
        return false;
      }

      if ((c >= '0') && (c <= '9')) {
        ss << c;
      } else {
        _sr->seek_from_current(-1);
        break;
      }
    }

    if (has_sign && (ss.str().size() == 1)) {
      // sign only
      _PushError("Integer value expected but got sign character only.\n");
      return false;
    }

    if ((ss.str().size() >= 1) && (ss.str()[0] == '0')) {
      _PushError("Zero padded integer value is not allowed.\n");
      return false;
    } 

    std::cout << "ReadInt token: " << ss.str() << "\n";

    // TODO(syoyo): Use ryu parse.
    try {
      (*value) = std::stoi(ss.str());
    } catch (const std::invalid_argument &e) {
      (void)e;
      _PushError("Not an integer literal.\n");
      return false;
    } catch (const std::out_of_range &e) {
      (void)e;
      _PushError("Integer value out of range.\n");
      return false;
    }

    return true;
  }

  ///
  /// Parses 1 or more occurences of int value, separated by `sep`
  ///
  bool SepBy1Int(const char sep, std::vector<int> *result) {

    result->clear();

    {
      int value;
      if (!ReadInt(&value)) {
        _PushError("Not starting with integer value.\n");
        return false;
      }

      result->push_back(value);
    }

    std::cout << "sep " << result->back() << "\n";

    while (!_sr->eof()) {

      // sep
      if (!SkipWhitespaceAndNewline()) {
        std::cout << "ws failure\n";
        return false;
      }

      char c;
      if (!_sr->read1(&c)) {
        std::cout << "read1 failure\n";
        return false;
      }

      std::cout << "sep c = " << c << "\n";

      if (c != sep) {
        // end
        std::cout << "sepBy1 end\n";
        _sr->seek_from_current(-1); // unwind single char
        break;
      }

      if (!SkipWhitespaceAndNewline()) {
        std::cout << "ws failure\n";
        return false;
      }

      std::cout << "go to read int\n";

      int value;
      if (!ReadInt(&value)) {
        std::cout << "not a int value\n";
        break;
      }

      result->push_back(value);
    }

    std::cout << "result.size " << result->size() << "\n";

    if (result->empty()) {
      _PushError("Empty integer array.\n");
      return false;
    }

    return true;
  }

  ///
  /// Parse '[', Sep1By(','), ']'
  ///
  bool ParseIntArray(std::vector<int> *result) {
    if (!Expect('[')) {
      return false;
    }
    std::cout << "got [\n";

    if (!SepBy1Int(',', result)) {
      return false;
    }

    std::cout << "try to parse ]\n";

    if (!Expect(']')) {
      std::cout << "not ]\n";

      return false;
    }
    std::cout << "got ]\n";

    return true;
  }

  ///
  /// Parse '(', Sep1By(','), ')'
  ///
  bool ParseIntTuple(const size_t num_items, std::vector<int> *result) {
    if (!Expect('(')) {
      return false;
    }
    std::cout << "got (\n";

    if (!SepBy1Int(',', result)) {
      return false;
    }

    std::cout << "try to parse )\n";

    if (!Expect(')')) {
      return false;
    }

    if (result->size() != num_items) {
      std::string msg = "The number of tuple elements must be " + std::to_string(num_items) + ", but got " + std::to_string(result->size()) + "\n";
      _PushError(msg);
      return false;
    }

    return true;

  }


  bool ReadStringLiteral(std::string &token) {
    std::stringstream ss;

    char c0;
    if (!_sr->read1(&c0)) {
      return false;
    }

    if (c0 != '"') {
      ErrorDiagnositc diag;
      diag.err = "String literal expected but it does not start with '\"'\n";
      diag.line_col = _line_col;
      diag.line_row = _line_row;

      err_stack.push(diag);
      return false;
    }

    ss << "\"";

    while (!_sr->eof()) {
      char c;
      if (!_sr->read1(&c)) {
        // this should not happen.
        std::cout << "read err\n";
        return false;
      }

      ss << c;

      if (c == '"') {
        break;
      }
    }

    token = ss.str();

    if (token.back() != '"') {
      ErrorDiagnositc diag;
      diag.err = "String literal expected but it does not end with '\"'\n";
      diag.line_col = _line_col;
      diag.line_row = _line_row;

      err_stack.push(diag);
      return false;
    }

    _line_col += token.size();

    return true;
  }

  bool ReadIdentifier(std::string &token) {
    std::stringstream ss;

    std::cout << "readtoken\n";

    while (!_sr->eof()) {
      char c;
      if (!_sr->read1(&c)) {
        // this should not happen.
        return false;
      }

      if (!std::isalpha(int(c))) {
        _sr->seek_from_current(-1);
        break;
      }

      _line_col++;

      std::cout << c << "\n";
      ss << c;
    }

    token = ss.str();
    std::cout << "token = " << token << "\n";
    return true;
  }

  bool SkipUntilNewline() {
    while (!_sr->eof()) {
      char c;
      if (!_sr->read1(&c)) {
        // this should not happen.
        return false;
      }

      if (c == '\n') {
        break;
      } else if (c == '\r') {
        // CRLF?
        if (_sr->tell() < (_sr->size() - 1)) {
          char d;
          if (!_sr->read1(&d)) {
            // this should not happen.
            return false;
          }

          if (d == '\n') {
            break;
          }

          // unwind 1 char
          if (!_sr->seek_from_current(-1)) {
            // this should not happen.
            return false;
          }

          break;
        }

      } else {
        // continue
      }
    }

    _line_row++;
    _line_col = 0;
    return true;
  }

  bool SkipWhitespace() {
    while (!_sr->eof()) {
      char c;
      if (!_sr->read1(&c)) {
        // this should not happen.
        return false;
      }
      _line_col++;

      if ((c == ' ') || (c == '\t') || (c == '\f')) {
        // continue
      } else {
        break;
      }
    }

    // unwind 1 char
    if (!_sr->seek_from_current(-1)) {
      return false;
    }
    _line_col--;

    return true;
  }

  bool SkipWhitespaceAndNewline() {
    while (!_sr->eof()) {
      char c;
      if (!_sr->read1(&c)) {
        // this should not happen.
        return false;
      }

      printf("sws c = %c\n", c);

      if ((c == ' ') || (c == '\t') || (c == '\f')) {
        _line_col++;
        // continue
      } else if (c == '\n') {
        _line_col = 0;
        _line_row++;
        // continue
      } else if (c == '\r') {
        // CRLF?
        if (_sr->tell() < (_sr->size() - 1)) {
          char d;
          if (!_sr->read1(&d)) {
            // this should not happen.
            return false;
          }

          if (d == '\n') {
            // CRLF
          } else {
            // unwind 1 char
            if (!_sr->seek_from_current(-1)) {
              // this should not happen.
              return false;
            }
          }
        }
        _line_col = 0;
        _line_row++;
        // continue
      } else {
        std::cout << "unwind\n";
        // end loop
        if (!_sr->seek_from_current(-1)) {
          return false;
        }
        break;
      }
    }

    return true;
  }

  bool Expect(char expect_c) {
    if (!SkipWhitespace()) {
      return false;
    }

    char c;
    if (!_sr->read1(&c)) {
      // this should not happen.
      return false;
    }

    bool ret = (c == expect_c);

    if (!ret) {
      std::string msg = "Expected `" + std::string(&expect_c, 1) + "` but got `" +
                 std::string(&c, 1) + "`\n";
      _PushError(msg);

      // unwind
      _sr->seek_from_current(-1);
    } else {
      _line_col++;
    }

    return ret;
  }

  // Parse magic
  // #usda FLOAT
  bool ParseMagicHeader() {
    if (!SkipWhitespace()) {
      return false;
    }

    if (_sr->eof()) {
      return false;
    }

    {
      char magic[5];
      if (!_sr->read(5, 5, reinterpret_cast<uint8_t *>(magic))) {
        // eol
        return false;
      }

      if ((magic[0] == '#') && (magic[1] == 'u') && (magic[2] == 's') &&
          (magic[3] == 'd') && (magic[4] == 'a')) {
        // ok
      } else {
        ErrorDiagnositc diag;
        diag.line_row = _line_row;
        diag.line_col = _line_col;
        diag.err = "Magic header must be `#usda` but got `" +
                   std::string(magic, 5) + "`\n";
        err_stack.push(diag);

        return false;
      }
    }

    if (!SkipWhitespace()) {
      // eof
      return false;
    }

    // current we only accept "1.0"
    {
      char ver[3];
      if (!_sr->read(3, 3, reinterpret_cast<uint8_t *>(ver))) {
        return false;
      }

      if ((ver[0] == '1') && (ver[1] == '.') && (ver[2] == '0')) {
        // ok
        _version = 1.0f;
      } else {
        ErrorDiagnositc diag;
        diag.line_row = _line_row;
        diag.line_col = _line_col;
        diag.err =
            "Version must be `1.0` but got `" + std::string(ver, 3) + "`\n";
        err_stack.push(diag);

        return false;
      }
    }

    SkipUntilNewline();

    return true;
  }

  // Parse meta
  // ( metadata_opt )
  bool ParseMeta() {
    if (!Expect('(')) {
      return false;
    }
    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    if (Expect(')')) {
      if (!SkipWhitespaceAndNewline()) {
        return false;
      }
    } else {
      // metadata line
      // var = value
      std::string varname;
      if (!ReadIdentifier(varname)) {
        std::cout << "token " << varname;
        return false;
      }

      if (!IsBuiltinMeta(varname)) {

        ErrorDiagnositc diag;
        diag.line_row = _line_row;
        diag.line_col = _line_col;
        diag.err =
            "'" + varname + "' is not a builtin Metadata variable.\n";
        err_stack.push(diag);
        return false;
      }

      if (!Expect('=')) {
        ErrorDiagnositc diag;
        diag.line_row = _line_row;
        diag.line_col = _line_col;
        diag.err =
            "'=' expected in Metadata line.\n";
        err_stack.push(diag);
        return false;
      }
      SkipWhitespace();

      const Variable &var = _builtin_metas.at(varname); 
      if (var.type == "string") {
        std::string value;
        std::cout << "read string literal\n";
        if (!ReadStringLiteral(value)) {
          std::string msg = "String literal expected for `" + var.name + "`.\n";
          _PushError(msg);
          return false;
        }
      } else if (var.type == "int[]") {
        std::vector<int> values;
        if (!ParseIntArray(&values)) {
          //std::string msg = "Array of int values expected for `" + var.name + "`.\n";
          //_PushError(msg);
          return false;
        }

        for (size_t i = 0; i < values.size(); i++) {
          std::cout << "int[" << i << "] = " << values[i] << "\n";
        }
      } else if (var.type == "int3") {
        std::vector<int> values;
        if (!ParseIntTuple(3, &values)) {
          //std::string msg = "Array of int values expected for `" + var.name + "`.\n";
          //_PushError(msg);
          return false;
        }

        for (size_t i = 0; i < values.size(); i++) {
          std::cout << "int[" << i << "] = " << values[i] << "\n";
        }
      }
    }

    return true;
  }

  // `#` style comment
  bool ParseSharpComment() {
    char c;
    if (!_sr->read1(&c)) {
      // eol
      return false;
    }

    if (c != '#') {
      return false;
    }

    return true;
  }

  bool Push() {
    // Stack size must be less than the number of input bytes.
    assert(parse_stack.size() < _sr->size());

    uint64_t loc = _sr->tell();

    ParseState state;
    state.loc = int64_t(loc);
    parse_stack.push(state);

    return true;
  }

  bool Pop(ParseState *state) {
    if (parse_stack.empty()) {
      return false;
    }

    (*state) = parse_stack.top();

    parse_stack.pop();

    return true;
  }

  bool Parse() {
    bool ok{false};

    ok = ParseMagicHeader();
    ok &= ParseMeta();

    return ok;
  }

  std::string GetError() {
    if (err_stack.empty()) {
      return std::string();
    }

    ErrorDiagnositc diag = err_stack.top();

    std::stringstream ss;
    ss << "Near line " << diag.line_row << ", col " << diag.line_col << ": ";
    ss << diag.err << "\n";
    return ss.str();
  }

 private:

  void _PushError(const std::string &msg) {
    ErrorDiagnositc diag;
    diag.line_row = _line_row;
    diag.line_col = _line_col;
    diag.err = msg;
    err_stack.push(diag);
  }


  bool IsBuiltinMeta(std::string name) {
    return _builtin_metas.count(name) ? true : false;
  }

  void _RegisterBuiltinMeta() {
    _builtin_metas["doc"] = Variable("string", "doc");
    _builtin_metas["metersPerUnit"] = Variable("float", "metersPerUnit");
    _builtin_metas["upAxis"] = Variable("string", "upAxis");
    _builtin_metas["test"] = Variable("int[]", "test");
    _builtin_metas["testt"] = Variable("int3", "testt");
  }

  const tinyusdz::StreamReader *_sr = nullptr;

  std::map<std::string, Variable> _builtin_metas;

  std::stack<ErrorDiagnositc> err_stack;
  std::stack<ParseState> parse_stack;

  int _line_row{0};
  int _line_col{0};

  float _version{1.0f};
};

}  // namespace tinyusdz

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cout << "Need input.usda\n";
    exit(-1);
  }

  std::string filename = argv[1];

  std::vector<uint8_t> data;
  {
    // TODO(syoyo): Support UTF-8 filename
    std::ifstream ifs(filename.c_str(), std::ifstream::binary);
    if (!ifs) {
      std::cerr << "Failed to open file: " << filename << "\n";
      return -1;
    }

    // TODO(syoyo): Use mmap
    ifs.seekg(0, ifs.end);
    size_t sz = static_cast<size_t>(ifs.tellg());
    if (int64_t(sz) < 0) {
      // Looks reading directory, not a file.
      std::cerr << "Looks like filename is a directory : \"" << filename
                << "\"\n";
      return -1;
    }

    data.resize(sz);

    ifs.seekg(0, ifs.beg);
    ifs.read(reinterpret_cast<char *>(&data.at(0)),
             static_cast<std::streamsize>(sz));
  }

  tinyusdz::StreamReader sr(data.data(), data.size(), /* swap endian */ false);
  tinyusdz::USDAParser parser(&sr);

  {
    bool ret = parser.Parse();

    if (!ret) {
      std::cerr << "Failed to parse .usda: \n";
      std::cerr << parser.GetError() << "\n";
    } else {
      std::cout << "ok\n";
    }
  }

  return 0;
}
