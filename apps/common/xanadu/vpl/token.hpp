/**
 * @file token.hpp
 * @brief Token definitions for the Vortex Parallel Language (VPL).
 *
 * Defines the token kinds for VPL, supporting both authentic Unicode APL
 * symbols (⍳, ⍴, ⍉, etc.) and J-style alternative ASCII function names
 * (i., $, |:, etc.), higher-order operators, stranding, and Xanadu hypertime
 * extensions.
 */
#ifndef COMMON_XANADU_VPL_TOKEN_HPP
#define COMMON_XANADU_VPL_TOKEN_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "common/xanadu/scanner_base.hpp"

namespace xanadu::vpl {

enum class TokenKind : std::uint16_t {
  // Literals & Names
  Number,     ///< Numeric scalar (e.g. 42, 3.14, _5, ¯5)
  String,     ///< Quoted string scalar ('...' or "...")
  Dimension,  ///< Dimension reference (e.g. d.1, d.2, d.doc)
  Identifier, ///< Variable or function name (e.g. A, W, myView, H)

  // Structural Verbs / Primitives
  Iota,         ///< ⍳ or i. (index generator / mint rank along axis)
  Rho,          ///< ⍴ or $ (shape / reshape / valence)
  Transpose,    ///< ⍉ or |: (rebind / permute axis list)
  ReverseFirst, ///< ⌽ or |. (reverse first axis / walk negward)
  ReverseLast,  ///< ⊖ or |.. (reverse last axis)
  Take,         ///< ↑ or {. (take first n items)
  Drop,         ///< ↓ or }. (drop first n items)
  Enclose,      ///< ⊂ or < (box view into cell / complex data containment)
  Disclose,     ///< ⊃ or > (unbox / first item / clone master along -d.clone)
  Member,       ///< ∊ or e. (membership test)
  Where,        ///< ⍸ or I. (indices / cells meeting condition)
  Match,     ///< ≡ or -: (valence when monadic; address identity when dyadic)
  Tally,     ///< ≢ or # (sum of span lengths / count items)
  GradeUp,   ///< ⍋ or /: (grade up / sort ascending indices)
  GradeDown, ///< ⍒ or \: (grade down / sort descending indices)

  // Arithmetic & Elementary Math
  Plus,      ///< + (identity / conjugate / add)
  Minus,     ///< - (negate / subtract)
  Times,     ///< × or * (signum / multiply)
  Divide,    ///< ÷ or % (reciprocal / divide)
  Power,     ///< * or ^ (exponential e^x / power x^y)
  Magnitude, ///< | (absolute value / residue modulo)
  Ceiling,   ///< ⌈ or >. (ceiling / maximum)
  Floor,     ///< ⌊ or <. (floor / minimum)

  // Comparisons & Logic
  Equal,        ///< = (equality)
  NotEqual,     ///< ≠ or ~= or ~: (inequality)
  LessThan,     ///< < (less than)
  LessEqual,    ///< ≤ or <= or <:= or <=. (less than or equal)
  GreaterThan,  ///< > (greater than)
  GreaterEqual, ///< ≥ or >= or >:= or >=. (greater than or equal)
  Not,          ///< ∼ or ~ (boolean not)
  And,          ///< ∧ or *. or & (boolean and)
  Or,           ///< ∨ or +. (boolean or)

  // Higher-Order Operators (Adverbs & Conjunctions)
  Reduce,       ///< / (fold along walk)
  Scan,         ///< \ (fold emitting intermediate cells in hypertime)
  Each,         ///< ¨ or each. or &. (apply verb per cell)
  Compress,     ///< ⌿ or copy. or #/ (filter rank by boolean rank)
  OuterProduct, ///< ∘. or o. or table. (Cartesian weave over two dimensions)
  RankOp,       ///< ⍤ or " (rank operator / axis scope)
  Key,          ///< ⌸ or key. or /.. (group rank into clone ranks)

  // Xanadu Hyperstructural Primitives (§3)
  Hypertime,  ///< ⍟ or time. or t. (microversion of view)
  Scrub,      ///< ⍫ or scrub. or s. (replay view as of microversion T)
  Transclude, ///< ⌺ or trans. or x. (discover cells addressing view's spans)

  // Quad System Functions
  Quad,      ///< ⎕ or print. or out. (formatted inspect / print)
  QuadRead,  ///< ⎕READ or read. (read xanadoc into doc rank)
  QuadSplit, ///< ⎕SPLIT or split. (split text span into word rank)

  // Delimiters & Punctuation
  Assign,    ///< ← or =. or := or <- (name binding / weave write)
  LParen,    ///< (
  RParen,    ///< )
  LBracket,  ///< [
  RBracket,  ///< ]
  Semicolon, ///< ;
  Newline,   ///< \n
  EndOfFile, ///< EOF
  Error      ///< Lexical error
};

struct Token {
  TokenKind kind{TokenKind::EndOfFile};
  std::string_view text{};
  SourceLocation location{};
  double floatValue{0.0};
  std::int64_t intValue{0};
  bool isFloat{false};
  std::string stringValue{};
};

[[nodiscard]] constexpr std::string_view
tokenKindToString(TokenKind kind) noexcept {
  switch (kind) {
  case TokenKind::Number:
    return "Number";
  case TokenKind::String:
    return "String";
  case TokenKind::Dimension:
    return "Dimension";
  case TokenKind::Identifier:
    return "Identifier";
  case TokenKind::Iota:
    return "Iota (⍳ / i.)";
  case TokenKind::Rho:
    return "Rho (⍴ / $)";
  case TokenKind::Transpose:
    return "Transpose (⍉ / |:)";
  case TokenKind::ReverseFirst:
    return "ReverseFirst (⌽ / |.)";
  case TokenKind::ReverseLast:
    return "ReverseLast (⊖ / |..)";
  case TokenKind::Take:
    return "Take (↑ / {.)";
  case TokenKind::Drop:
    return "Drop (↓ / }.)";
  case TokenKind::Enclose:
    return "Enclose (⊂ / <)";
  case TokenKind::Disclose:
    return "Disclose (⊃ / >)";
  case TokenKind::Member:
    return "Member (∊ / e.)";
  case TokenKind::Where:
    return "Where (⍸ / I.)";
  case TokenKind::Match:
    return "Match (≡ / -:)";
  case TokenKind::Tally:
    return "Tally (≢ / #)";
  case TokenKind::GradeUp:
    return "GradeUp (⍋ / /:)";
  case TokenKind::GradeDown:
    return "GradeDown (⍒ / \\:)";
  case TokenKind::Plus:
    return "Plus (+)";
  case TokenKind::Minus:
    return "Minus (-)";
  case TokenKind::Times:
    return "Times (× / *)";
  case TokenKind::Divide:
    return "Divide (÷ / %)";
  case TokenKind::Power:
    return "Power (* / ^)";
  case TokenKind::Magnitude:
    return "Magnitude (|)";
  case TokenKind::Ceiling:
    return "Ceiling (⌈ / >.)";
  case TokenKind::Floor:
    return "Floor (⌊ / <.)";
  case TokenKind::Equal:
    return "Equal (=)";
  case TokenKind::NotEqual:
    return "NotEqual (≠ / ~=)";
  case TokenKind::LessThan:
    return "LessThan (<)";
  case TokenKind::LessEqual:
    return "LessEqual (≤ / <=)";
  case TokenKind::GreaterThan:
    return "GreaterThan (>)";
  case TokenKind::GreaterEqual:
    return "GreaterEqual (≥ / >=)";
  case TokenKind::Not:
    return "Not (∼ / ~)";
  case TokenKind::And:
    return "And (∧ / &)";
  case TokenKind::Or:
    return "Or (∨ / |)";
  case TokenKind::Reduce:
    return "Reduce (/)";
  case TokenKind::Scan:
    return "Scan (\\)";
  case TokenKind::Each:
    return "Each (¨ / each.)";
  case TokenKind::Compress:
    return "Compress (⌿ / copy.)";
  case TokenKind::OuterProduct:
    return "OuterProduct (∘. / o.)";
  case TokenKind::RankOp:
    return "RankOp (⍤ / \")";
  case TokenKind::Key:
    return "Key (⌸ / key.)";
  case TokenKind::Hypertime:
    return "Hypertime (⍟ / time.)";
  case TokenKind::Scrub:
    return "Scrub (⍫ / scrub.)";
  case TokenKind::Transclude:
    return "Transclude (⌺ / trans.)";
  case TokenKind::Quad:
    return "Quad (⎕ / print.)";
  case TokenKind::QuadRead:
    return "QuadRead (⎕READ / read.)";
  case TokenKind::QuadSplit:
    return "QuadSplit (⎕SPLIT / split.)";
  case TokenKind::Assign:
    return "Assign (← / =.)";
  case TokenKind::LParen:
    return "LParen ('(')";
  case TokenKind::RParen:
    return "RParen (')')";
  case TokenKind::LBracket:
    return "LBracket ('[')";
  case TokenKind::RBracket:
    return "RBracket (']')";
  case TokenKind::Semicolon:
    return "Semicolon (';')";
  case TokenKind::Newline:
    return "Newline";
  case TokenKind::EndOfFile:
    return "EndOfFile";
  case TokenKind::Error:
    return "Error";
  }
  return "Unknown";
}

[[nodiscard]] constexpr bool isVerb(TokenKind kind) noexcept {
  switch (kind) {
  case TokenKind::Iota:
  case TokenKind::Rho:
  case TokenKind::Transpose:
  case TokenKind::ReverseFirst:
  case TokenKind::ReverseLast:
  case TokenKind::Take:
  case TokenKind::Drop:
  case TokenKind::Enclose:
  case TokenKind::Disclose:
  case TokenKind::Member:
  case TokenKind::Where:
  case TokenKind::Match:
  case TokenKind::Tally:
  case TokenKind::GradeUp:
  case TokenKind::GradeDown:
  case TokenKind::Plus:
  case TokenKind::Minus:
  case TokenKind::Times:
  case TokenKind::Divide:
  case TokenKind::Power:
  case TokenKind::Magnitude:
  case TokenKind::Ceiling:
  case TokenKind::Floor:
  case TokenKind::Equal:
  case TokenKind::NotEqual:
  case TokenKind::LessThan:
  case TokenKind::LessEqual:
  case TokenKind::GreaterThan:
  case TokenKind::GreaterEqual:
  case TokenKind::Not:
  case TokenKind::And:
  case TokenKind::Or:
  case TokenKind::Hypertime:
  case TokenKind::Scrub:
  case TokenKind::Transclude:
  case TokenKind::Quad:
  case TokenKind::QuadRead:
  case TokenKind::QuadSplit:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool isAdverb(TokenKind kind) noexcept {
  switch (kind) {
  case TokenKind::Reduce:
  case TokenKind::Scan:
  case TokenKind::Each:
  case TokenKind::Compress:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool isConjunction(TokenKind kind) noexcept {
  switch (kind) {
  case TokenKind::OuterProduct:
  case TokenKind::RankOp:
  case TokenKind::Key:
    return true;
  default:
    return false;
  }
}

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_TOKEN_HPP
