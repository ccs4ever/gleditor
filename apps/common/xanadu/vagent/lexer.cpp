/**
 * @file lexer.cpp
 * @brief Implementation of the vagent tokenizer (design §8.1).
 */
#include "common/xanadu/vagent/lexer.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <unordered_map>

namespace xanadu::vagent {

namespace {

constexpr std::int64_t msPerMilli            = 1;
constexpr std::int64_t msPerSecond           = 1000;
constexpr std::int64_t msPerMinute           = 60 * msPerSecond;
constexpr char32_t maxCodePoint              = 0x10FFFF;
constexpr char32_t surrogateFirst            = 0xD800;
constexpr char32_t surrogateLast             = 0xDFFF;
constexpr std::size_t maxUnicodeEscapeDigits = 6;

bool isIdentStart(char c) {
  return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_';
}
bool isIdentCont(char c) {
  return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_';
}
bool isDigit(char c) {
  return std::isdigit(static_cast<unsigned char>(c)) != 0;
}
bool isHexDigit(char c) {
  return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

LexFailure fail(LexErrorCode code, std::string detail, SourceLocation loc,
                char bracket = '\0') {
  return LexFailure{.code    = code,
                    .detail  = std::move(detail),
                    .loc     = loc,
                    .bracket = bracket};
}

const std::unordered_map<std::string_view, TokenKind> &keywordTable() {
  static const std::unordered_map<std::string_view, TokenKind> table = {
#define XANADU_VAGENT_ENTRY(name, text) {text, TokenKind::name},
      XANADU_VAGENT_KEYWORDS(XANADU_VAGENT_ENTRY)
#undef XANADU_VAGENT_ENTRY
          {"true", TokenKind::True},
      {"false", TokenKind::False},
  };
  return table;
}

void appendUtf8(std::string &out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

char closerFor(char opener) {
  switch (opener) {
  case '(':
    return ')';
  case '[':
    return ']';
  default:
    return '}';
  }
}

} // namespace

std::string_view lexErrorName(LexErrorCode code) noexcept {
  switch (code) {
  case LexErrorCode::UnexpectedCharacter:
    return "UnexpectedCharacter";
  case LexErrorCode::UnterminatedString:
    return "UnterminatedString";
  case LexErrorCode::UnterminatedComment:
    return "UnterminatedComment";
  case LexErrorCode::BadEscape:
    return "BadEscape";
  case LexErrorCode::BadNumber:
    return "BadNumber";
  case LexErrorCode::UnclosedBracket:
    return "UnclosedBracket";
  case LexErrorCode::MismatchedBracket:
    return "MismatchedBracket";
  }
  return "Unknown";
}

std::string_view tokenKindName(TokenKind kind) noexcept {
  switch (kind) {
  case TokenKind::EndOfFile:
    return "end of input";
  case TokenKind::Identifier:
    return "identifier";
  case TokenKind::IntegerLiteral:
    return "integer";
  case TokenKind::FloatLiteral:
    return "float";
  case TokenKind::StringLiteral:
    return "string";
  case TokenKind::DurationLiteral:
    return "duration";
  case TokenKind::True:
    return "true";
  case TokenKind::False:
    return "false";
#define XANADU_VAGENT_NAME(name, text)                                         \
  case TokenKind::name:                                                        \
    return text;
    XANADU_VAGENT_KEYWORDS(XANADU_VAGENT_NAME)
#undef XANADU_VAGENT_NAME
  case TokenKind::OpenParen:
    return "(";
  case TokenKind::CloseParen:
    return ")";
  case TokenKind::OpenBracket:
    return "[";
  case TokenKind::CloseBracket:
    return "]";
  case TokenKind::OpenBrace:
    return "{";
  case TokenKind::CloseBrace:
    return "}";
  case TokenKind::Comma:
    return ",";
  case TokenKind::Semicolon:
    return ";";
  case TokenKind::Colon:
    return ":";
  case TokenKind::Dot:
    return ".";
  case TokenKind::Assign:
    return ":=";
  case TokenKind::RevAssign:
    return "<-";
  case TokenKind::Plus:
    return "+";
  case TokenKind::Minus:
    return "-";
  case TokenKind::Star:
    return "*";
  case TokenKind::Slash:
    return "/";
  case TokenKind::Percent:
    return "%";
  case TokenKind::Concat:
    return "++";
  case TokenKind::Less:
    return "<";
  case TokenKind::LessEqual:
    return "<=";
  case TokenKind::Greater:
    return ">";
  case TokenKind::GreaterEqual:
    return ">=";
  case TokenKind::EqualEqual:
    return "==";
  case TokenKind::NotEqual:
    return "!=";
  case TokenKind::Bang:
    return "!";
  case TokenKind::Ampersand:
    return "&";
  case TokenKind::Bar:
    return "|";
  case TokenKind::QuestionMark:
    return "?";
  }
  return "?";
}

bool canEndStatement(TokenKind k) noexcept {
  switch (k) {
  case TokenKind::Identifier:
  case TokenKind::IntegerLiteral:
  case TokenKind::FloatLiteral:
  case TokenKind::StringLiteral:
  case TokenKind::DurationLiteral:
  case TokenKind::True:
  case TokenKind::False:
  case TokenKind::None:
  case TokenKind::Self:
  case TokenKind::CloseParen:
  case TokenKind::CloseBracket:
  case TokenKind::CloseBrace:
  case TokenKind::Yield:
  case TokenKind::Consume:
  case TokenKind::Fail:
  case TokenKind::Terminate:
  case TokenKind::Return:
    return true;
  default:
    return false;
  }
}

bool continuesStatement(TokenKind k) noexcept {
  switch (k) {
  case TokenKind::Ampersand:
  case TokenKind::Bar:
  case TokenKind::Concat:
  case TokenKind::Plus:
  case TokenKind::Slash:
  case TokenKind::Percent:
  case TokenKind::Less:
  case TokenKind::LessEqual:
  case TokenKind::Greater:
  case TokenKind::GreaterEqual:
  case TokenKind::EqualEqual:
  case TokenKind::NotEqual:
  case TokenKind::Dot:
  case TokenKind::QuestionMark:
  case TokenKind::To:
  case TokenKind::By:
  case TokenKind::Then:
  case TokenKind::Else:
  case TokenKind::Do:
    return true;
  default:
    return false;
  }
}

bool newlineSeparates(TokenKind prev, TokenKind next) noexcept {
  return canEndStatement(prev) && !continuesStatement(next);
}

Lexer::Lexer(std::string_view source) : scanner_(source) {}

Token Lexer::make(TokenKind kind, SourceLocation loc, std::size_t start) const {
  Token t;
  t.kind = kind;
  t.loc  = loc;
  t.text = scanner_.source().substr(start, scanner_.position() - start);
  return t;
}

std::expected<bool, LexFailure> Lexer::skipTrivia() {
  bool sawNewline = false;
  for (;;) {
    const char c = scanner_.peekChar();
    if (c == '\n') {
      sawNewline = true;
      scanner_.advanceChar();
    } else if (c == ' ' || c == '\t' || c == '\r') {
      scanner_.advanceChar();
    } else if (c == '/' && scanner_.peekChar(1) == '/') {
      while (!scanner_.isAtEnd() && scanner_.peekChar() != '\n') {
        scanner_.advanceChar();
      }
    } else if (c == '/' && scanner_.peekChar(1) == '*') {
      const SourceLocation start = scanner_.currentLocation();
      scanner_.advanceChar();
      scanner_.advanceChar();
      for (;;) {
        if (scanner_.isAtEnd()) {
          return std::unexpected(fail(LexErrorCode::UnterminatedComment,
                                      "block comment is never closed", start));
        }
        if (scanner_.peekChar() == '*' && scanner_.peekChar(1) == '/') {
          scanner_.advanceChar();
          scanner_.advanceChar();
          break;
        }
        // A comment that spans a line counts as the newline it hides.
        sawNewline = sawNewline || scanner_.peekChar() == '\n';
        scanner_.advanceChar();
      }
    } else {
      return sawNewline;
    }
  }
}

Token Lexer::scanWord() {
  const SourceLocation loc = scanner_.currentLocation();
  const std::size_t start  = scanner_.position();
  // `$` begins reserved field names; the checker, not the lexer, refuses user
  // code that declares or assigns one.
  if (scanner_.peekChar() == '$') {
    scanner_.advanceChar();
  }
  while (isIdentCont(scanner_.peekChar())) {
    scanner_.advanceChar();
  }
  Token t           = make(TokenKind::Identifier, loc, start);
  const auto &table = keywordTable();
  if (const auto it = table.find(t.text); it != table.end()) {
    t.kind = it->second;
  }
  return t;
}

std::expected<Token, LexFailure> Lexer::scanNumber() {
  const SourceLocation loc = scanner_.currentLocation();
  const std::size_t start  = scanner_.position();
  std::string digits;

  const auto badNumber = [&](std::string why) {
    return std::unexpected(fail(LexErrorCode::BadNumber, std::move(why), loc));
  };
  const auto takeDigits = [&](auto isOk) {
    while (isOk(scanner_.peekChar()) ||
           (scanner_.peekChar() == '_' && isOk(scanner_.peekChar(1)) &&
            !digits.empty())) {
      const char c = scanner_.advanceChar();
      if (c != '_') {
        digits += c;
      }
    }
  };

  if (scanner_.peekChar() == '0' &&
      (scanner_.peekChar(1) == 'x' || scanner_.peekChar(1) == 'X') &&
      isHexDigit(scanner_.peekChar(2))) {
    scanner_.advanceChar();
    scanner_.advanceChar();
    takeDigits(isHexDigit);
    std::int64_t v{};
    const auto [ptr, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), v, 16);
    if (ec != std::errc{} || ptr != digits.data() + digits.size() ||
        isIdentCont(scanner_.peekChar())) {
      return badNumber("malformed or out-of-range hexadecimal literal");
    }
    Token t    = make(TokenKind::IntegerLiteral, loc, start);
    t.intValue = v;
    return t;
  }

  takeDigits(isDigit);
  bool isFloat = false;
  if (scanner_.peekChar() == '.' && isDigit(scanner_.peekChar(1))) {
    isFloat = true;
    digits += scanner_.advanceChar();
    takeDigits(isDigit);
  }
  const char e = scanner_.peekChar();
  if ((e == 'e' || e == 'E') &&
      (isDigit(scanner_.peekChar(1)) ||
       ((scanner_.peekChar(1) == '+' || scanner_.peekChar(1) == '-') &&
        isDigit(scanner_.peekChar(2))))) {
    isFloat = true;
    digits += scanner_.advanceChar();
    if (scanner_.peekChar() == '+' || scanner_.peekChar() == '-') {
      digits += scanner_.advanceChar();
    }
    takeDigits(isDigit);
  }

  std::int64_t intValue{};
  double floatValue{};
  if (isFloat) {
    const auto [ptr, ec] = std::from_chars(
        digits.data(), digits.data() + digits.size(), floatValue);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
      return badNumber("malformed or out-of-range float literal");
    }
  } else {
    const auto [ptr, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), intValue);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
      return badNumber("integer literal does not fit in 64 bits");
    }
    floatValue = static_cast<double>(intValue);
  }

  if (isIdentStart(scanner_.peekChar())) {
    const std::size_t suffixStart = scanner_.position();
    while (isIdentCont(scanner_.peekChar())) {
      scanner_.advanceChar();
    }
    const std::string_view suffix = scanner_.source().substr(
        suffixStart, scanner_.position() - suffixStart);
    std::int64_t unit = 0;
    if (suffix == "ms") {
      unit = msPerMilli;
    } else if (suffix == "s") {
      unit = msPerSecond;
    } else if (suffix == "min") {
      unit = msPerMinute;
    } else {
      return badNumber("unknown duration unit '" + std::string(suffix) +
                       "' (expected ms, s or min)");
    }
    std::int64_t ms{};
    if (isFloat) {
      const double scaled = floatValue * static_cast<double>(unit);
      if (!(scaled < 9.2e18)) {
        return badNumber("duration does not fit in 64 bits of milliseconds");
      }
      ms = static_cast<std::int64_t>(std::llround(scaled));
    } else if (__builtin_mul_overflow(intValue, unit, &ms)) {
      return badNumber("duration does not fit in 64 bits of milliseconds");
    }
    Token t      = make(TokenKind::DurationLiteral, loc, start);
    t.durationMs = ms;
    return t;
  }

  Token t = make(isFloat ? TokenKind::FloatLiteral : TokenKind::IntegerLiteral,
                 loc, start);
  t.intValue   = intValue;
  t.floatValue = floatValue;
  return t;
}

std::expected<Token, LexFailure> Lexer::scanString() {
  const SourceLocation loc = scanner_.currentLocation();
  const std::size_t start  = scanner_.position();
  scanner_.advanceChar(); // opening quote
  std::string out;
  for (;;) {
    if (scanner_.isAtEnd() || scanner_.peekChar() == '\n') {
      return std::unexpected(fail(LexErrorCode::UnterminatedString,
                                  "string literal is never closed", loc));
    }
    const SourceLocation here = scanner_.currentLocation();
    const char c              = scanner_.advanceChar();
    if (c == '"') {
      break;
    }
    if (c != '\\') {
      out += c;
      continue;
    }
    const char esc = scanner_.advanceChar();
    switch (esc) {
    case 'n':
      out += '\n';
      break;
    case 't':
      out += '\t';
      break;
    case '"':
      out += '"';
      break;
    case '\\':
      out += '\\';
      break;
    case 'u': {
      if (!scanner_.match('{')) {
        return std::unexpected(
            fail(LexErrorCode::BadEscape, "expected '{' after \\u", here));
      }
      char32_t cp       = 0;
      std::size_t count = 0;
      while (isHexDigit(scanner_.peekChar())) {
        const char h = scanner_.advanceChar();
        cp           = (cp << 4) |
             static_cast<char32_t>(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
        if (++count > maxUnicodeEscapeDigits) {
          break;
        }
      }
      if (count == 0 || count > maxUnicodeEscapeDigits ||
          !scanner_.match('}') || cp > maxCodePoint ||
          (cp >= surrogateFirst && cp <= surrogateLast)) {
        return std::unexpected(fail(LexErrorCode::BadEscape,
                                    "malformed or invalid \\u{...} escape",
                                    here));
      }
      appendUtf8(out, cp);
      break;
    }
    default:
      return std::unexpected(fail(LexErrorCode::BadEscape,
                                  std::string("unknown escape '\\") + esc + "'",
                                  here));
    }
  }
  Token t       = make(TokenKind::StringLiteral, loc, start);
  t.stringValue = std::move(out);
  return t;
}

std::expected<Token, LexFailure> Lexer::scanToken() {
  const SourceLocation loc = scanner_.currentLocation();
  const std::size_t start  = scanner_.position();
  const char c             = scanner_.peekChar();

  if (isIdentStart(c) || (c == '$' && isIdentStart(scanner_.peekChar(1)))) {
    return scanWord();
  }
  if (isDigit(c)) {
    return scanNumber();
  }
  if (c == '"') {
    return scanString();
  }

  scanner_.advanceChar();
  const auto one = [&](TokenKind k) { return make(k, loc, start); };
  const auto two = [&](char next, TokenKind both, TokenKind alone) {
    return make(scanner_.match(next) ? both : alone, loc, start);
  };
  switch (c) {
  case '(':
    return one(TokenKind::OpenParen);
  case ')':
    return one(TokenKind::CloseParen);
  case '[':
    return one(TokenKind::OpenBracket);
  case ']':
    return one(TokenKind::CloseBracket);
  case '{':
    return one(TokenKind::OpenBrace);
  case '}':
    return one(TokenKind::CloseBrace);
  case ',':
    return one(TokenKind::Comma);
  case ';':
    return one(TokenKind::Semicolon);
  case '.':
    return one(TokenKind::Dot);
  case ':':
    return two('=', TokenKind::Assign, TokenKind::Colon);
  case '+':
    return two('+', TokenKind::Concat, TokenKind::Plus);
  case '-':
    return one(TokenKind::Minus);
  case '*':
    return one(TokenKind::Star);
  case '/':
    return one(TokenKind::Slash);
  case '%':
    return one(TokenKind::Percent);
  case '&':
    return one(TokenKind::Ampersand);
  case '|':
    return one(TokenKind::Bar);
  case '?':
    return one(TokenKind::QuestionMark);
  case '!':
    return two('=', TokenKind::NotEqual, TokenKind::Bang);
  case '>':
    return two('=', TokenKind::GreaterEqual, TokenKind::Greater);
  case '<':
    if (scanner_.match('-')) {
      return one(TokenKind::RevAssign);
    }
    return two('=', TokenKind::LessEqual, TokenKind::Less);
  case '=':
    if (scanner_.match('=')) {
      return one(TokenKind::EqualEqual);
    }
    return std::unexpected(fail(LexErrorCode::UnexpectedCharacter,
                                "'=' alone is not an operator; comparison is "
                                "'==' and assignment is ':=' or '<-'",
                                loc));
  default:
    return std::unexpected(fail(LexErrorCode::UnexpectedCharacter,
                                std::string("unexpected character '") + c + "'",
                                loc));
  }
}

std::expected<void, LexFailure> Lexer::trackBracket(const Token &t) {
  switch (t.kind) {
  case TokenKind::OpenParen:
  case TokenKind::OpenBracket:
  case TokenKind::OpenBrace:
    open_.push_back({t.text.front(), t.loc});
    return {};
  case TokenKind::CloseParen:
  case TokenKind::CloseBracket:
  case TokenKind::CloseBrace: {
    const char closer = t.text.front();
    if (open_.empty() || closerFor(open_.back().bracket) != closer) {
      return std::unexpected(fail(
          LexErrorCode::MismatchedBracket,
          open_.empty() ? std::string("'") + closer + "' has no opener"
                        : std::string("'") + closer + "' does not match the '" +
                              open_.back().bracket + "' opened at line " +
                              std::to_string(open_.back().loc.line),
          t.loc, closer));
    }
    open_.pop_back();
    return {};
  }
  default:
    return {};
  }
}

std::expected<std::vector<Token>, LexFailure> Lexer::tokenize() {
  std::vector<Token> out;
  for (;;) {
    auto newline = skipTrivia();
    if (!newline) {
      return std::unexpected(std::move(newline.error()));
    }
    if (scanner_.isAtEnd()) {
      if (!open_.empty()) {
        const Open &o = open_.back();
        return std::unexpected(
            fail(LexErrorCode::UnclosedBracket,
                 std::string("'") + o.bracket + "' is never closed", o.loc,
                 o.bracket));
      }
      Token eof;
      eof.kind          = TokenKind::EndOfFile;
      eof.loc           = scanner_.currentLocation();
      eof.newlineBefore = *newline;
      out.push_back(std::move(eof));
      return out;
    }
    auto tok = scanToken();
    if (!tok) {
      return std::unexpected(std::move(tok.error()));
    }
    tok->newlineBefore = *newline;
    if (auto ok = trackBracket(*tok); !ok) {
      return std::unexpected(std::move(ok.error()));
    }
    out.push_back(std::move(*tok));
  }
}

} // namespace xanadu::vagent
