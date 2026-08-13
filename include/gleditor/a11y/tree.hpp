/**
 * @file tree.hpp
 * @brief What is on screen, said rather than drawn.
 *
 * Everything this program puts in its window is quads: a document, a caret, a
 * notification, a form field, a beam between two passages. A quad is a picture
 * of a thing and not the thing, and a screen reader has no way to get from one
 * to the other. So the second description exists -- a tree of nodes with roles
 * and names and bounds, kept beside the drawing and built from the same state.
 *
 * It is deliberately its own vocabulary rather than AccessKit's. Three reasons,
 * and the third is the one that matters:
 *
 * - The types below are plain data with no dependency beyond the standard
 *   library, so building the tree is testable without a device, a window, a
 *   D-Bus session or an assistive technology running.
 * - A build on a machine with no AccessKit installed has none in it, and the UI
 *   code should not know that -- see a11y/platform.hpp, where the whole of the
 *   difference lives.
 * - AccessKit's node vocabulary is large and general, being the intersection of
 *   several platforms' ideas of what a UI is. What a text editor needs from it
 *   is small, and naming that small part is how the mapping stays readable.
 *
 * The tree is built on the render thread, where the documents and the caret
 * are, and read on the event thread, which is what talks to the platform. See
 * a11y/publisher.hpp for the hand-off.
 */
#ifndef GLEDITOR_A11Y_TREE_H
#define GLEDITOR_A11Y_TREE_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gleditor::a11y {

/**
 * @brief What a node is, in the words the platforms share.
 *
 * A subset of AccessKit's roles: the ones this program has something that is
 * one of. Adding to it is a two-line change here and one line in the bridge.
 */
enum class Role : std::uint8_t {
  /// The window itself. Exactly one node has this, and it is the root.
  Window,
  /// A box holding other nodes and meaning nothing on its own.
  Group,
  /// Something a person reads and cannot edit.
  Label,
  /// A whole document. Its text is in its text runs.
  Document,
  /**
   * @brief A run of text with character offsets.
   *
   * The unit an assistive technology navigates text in: it asks for the
   * character at an offset, the word around one, the extent of a range. A run
   * carries the text and the byte length of each character in it, which is
   * what makes an offset in characters mean something in a UTF-8 string.
   */
  TextRun,
  /// One line typed into.
  TextInput,
  /// Several lines typed into.
  MultilineTextInput,
  /// A field whose content must not be spoken or shown: a passphrase.
  PasswordInput,
  /// A list one of whose entries is chosen.
  ComboBox,
  /// One entry of a ComboBox.
  ListItem,
  /// Something pressed.
  Button,
  /// Something that is on or off.
  Switch,
  /// A window within the window that must be answered before anything else.
  Dialog,
  /// Somewhere messages appear as they happen.
  Log,
  /// A connection to somewhere else, which can be followed.
  Link,
  /// An ordered set of entries.
  List,
};

/// What a node can be asked to do. A request arrives from the assistive
/// technology and is carried out by whatever owns the node.
enum class Action : std::uint8_t {
  /// Put the keyboard in it.
  Focus,
  /// Press it, follow it, open it -- whatever the primary gesture is.
  Click,
  /// Replace its text with the string that comes with the request.
  SetValue,
  /// Bring it into view without doing anything else to it.
  ScrollIntoView,
};

/// Actions as a set, since a node usually supports more than one.
[[nodiscard]] constexpr std::uint32_t bit(const Action action) {
  return 1U << static_cast<std::uint32_t>(action);
}

/// How urgently a change to this node should interrupt whatever is being read.
enum class Live : std::uint8_t {
  /// Not announced.
  Off,
  /// Announced when there is a pause.
  Polite,
  /// Announced now, interrupting.
  Assertive,
};

/**
 * @brief Where a node is, in window pixels from the top left.
 *
 * The platforms' convention rather than the glyph pipeline's, which counts
 * from the bottom. Converting once, here, is what keeps the flip in one place
 * instead of in every element that describes itself.
 */
struct Rect {
  double left{};
  double top{};
  double right{};
  double bottom{};

  bool operator==(const Rect &) const = default;
};

/// One end of a selection: a text run and how many characters into it.
struct TextPoint {
  std::uint64_t node{};
  std::size_t character{};

  bool operator==(const TextPoint &) const = default;
};

/// What is selected, or where the caret is when the two ends are the same.
struct TextSelection {
  /// Where the selection began -- where the drag started.
  TextPoint anchor;
  /// Where it has reached, which is where the caret is drawn.
  TextPoint focus;

  bool operator==(const TextSelection &) const = default;
};

/**
 * @brief One thing on screen.
 *
 * Filled in by whatever draws the thing, through Builder. Everything is
 * optional except the role: a node with only a role is a node an assistive
 * technology can still find, count and move past, which is better than one it
 * cannot see at all.
 */
struct Node {
  std::uint64_t id{};
  Role role{Role::Group};

  /// What this is called: a field's label, a button's text, a document's name.
  std::string label;
  /// What it holds: the text in a field, the entry a list is on.
  std::string value;
  /// A longer explanation, when the label is not enough on its own.
  std::string description;
  /// What a field would say if it were left empty. Shown greyed on screen.
  std::string placeholder;

  std::optional<Rect> bounds;
  std::vector<std::uint64_t> children;

  /// Actions this node accepts, as a set of bit(Action).
  std::uint32_t actions{};

  /// For a Switch: whether it is on.
  std::optional<bool> toggled;
  Live live{Live::Off};

  /// Whether it can be reached with the keyboard by tabbing to it.
  bool focusable{};
  /// Whether it holds text that cannot be changed.
  bool readOnly{};
  /// Whether everything outside it is unreachable while it is up.
  bool modal{};

  /**
   * @brief TextRun: the byte length of each character in @ref value.
   *
   * An assistive technology counts in characters and the value is UTF-8, so
   * without this an offset means nothing. A character here is a Unicode
   * scalar: the same unit the caret moves by, which is what makes a reported
   * caret position agree with where it is drawn.
   */
  std::vector<std::uint8_t> characterLengths;
  /**
   * @brief TextRun: the character index each word starts at.
   *
   * What "next word" means. Sorted, and counted in characters rather than
   * bytes -- indices into @ref characterLengths. A byte apiece, which is the
   * platforms' own limit and the reason a run is a line rather than a page:
   * see @ref runLimit.
   */
  std::vector<std::uint8_t> wordStarts;

  /// Where the caret and selection are within this node's text runs. Set on
  /// the node that owns the runs, not on the runs themselves.
  std::optional<TextSelection> selection;

  bool operator==(const Node &) const = default;
};

/**
 * @brief The whole of what is on screen, at one moment.
 *
 * A value, copied about and compared: what makes it possible to build one on
 * the render thread, hand it to the event thread, and tell whether anything
 * actually changed without asking every element again.
 */
struct Tree {
  /// Every node. The first is the root, and every other is some node's child.
  std::vector<Node> nodes;
  /// Which node has the keyboard. The root when nothing more specific does.
  std::uint64_t focus{};

  [[nodiscard]] bool empty() const { return nodes.empty(); }
  [[nodiscard]] std::uint64_t root() const {
    return nodes.empty() ? 0 : nodes.front().id;
  }
  /// The node with this id, or nothing. Linear: the trees here have tens of
  /// nodes, not thousands, and a map would cost more to build than it saves.
  [[nodiscard]] const Node *find(std::uint64_t id) const;

  bool operator==(const Tree &) const = default;
};

/**
 * @brief Node ids that do not collide, and stay the same between updates.
 *
 * An assistive technology remembers nodes by id: the same field keeping the
 * same id across an update is what stops a screen reader announcing the whole
 * form again every time a character is typed. So an id is not a counter -- it
 * is made of who the node belongs to and which of theirs it is, both of which
 * are stable.
 *
 * The high sixteen bits are the owner, the low forty-eight are that owner's
 * own numbering. An owner is a Source, which gets its number when it
 * registers; the library reserves the first few for its own.
 */
class Ids {
public:
  /// Owners the library itself uses. A program's sources are numbered from
  /// @ref firstProgramOwner upwards, in the order they register.
  enum Owner : std::uint16_t {
    window            = 0,
    documents         = 1,
    notifications     = 2,
    form              = 3,
    firstProgramOwner = 16,
  };

  [[nodiscard]] static constexpr std::uint64_t of(const std::uint16_t owner,
                                                  const std::uint64_t local) {
    return (static_cast<std::uint64_t>(owner) << 48U) | (local & mask);
  }
  [[nodiscard]] static constexpr std::uint16_t ownerOf(const std::uint64_t id) {
    return static_cast<std::uint16_t>(id >> 48U);
  }
  [[nodiscard]] static constexpr std::uint64_t localOf(const std::uint64_t id) {
    return id & mask;
  }

private:
  static constexpr std::uint64_t mask = (1ULL << 48U) - 1U;
};

/**
 * @brief Where a Source puts what it has to say.
 *
 * Handed to each source in turn while the tree is built. A source adds nodes
 * and says which are its children of the node above it; it never sees the
 * whole tree and cannot reach another source's nodes, which is what keeps two
 * elements from having to agree about anything.
 */
class Builder {
public:
  /**
   * @param aOwner The number this source's ids are made from. Assigned by the
   *        publisher, not chosen by the source.
   */
  Builder(Tree &into, std::uint16_t aOwner) : tree(into), owner_(aOwner) {}

  /// An id of this source's, from a number the source picks. Stable as long as
  /// the source picks the same number for the same thing.
  [[nodiscard]] std::uint64_t id(const std::uint64_t local) const {
    return Ids::of(owner_, local);
  }

  /**
   * @brief Add a node and return a reference to fill in.
   *
   * The reference is valid until the next add(), since the nodes are held in a
   * vector. Fill it in before adding another.
   */
  Node &add(std::uint64_t local, Role role);

  /// What this source contributed at the top level: the nodes the element
  /// above it should list as its children.
  [[nodiscard]] const std::vector<std::uint64_t> &roots() const {
    return roots_;
  }
  /// Say that a node is one of this source's top-level nodes. Called by the
  /// source for the ones the view should hold directly; nodes reached only as
  /// another node's child are not.
  void contribute(std::uint64_t nodeId) { roots_.push_back(nodeId); }

  /// Ask for the keyboard focus to be reported as being on this node. The last
  /// source to ask wins, which is right: a modal is described after the
  /// document behind it.
  void takeFocus(const std::uint64_t nodeId) { tree.focus = nodeId; }

  [[nodiscard]] std::uint16_t owner() const { return owner_; }

private:
  Tree &tree;
  std::uint16_t owner_;
  std::vector<std::uint64_t> roots_;
};

/**
 * @brief Something on screen that can say what it is.
 *
 * The accessibility counterpart of FrameContributor: one draws the thing, this
 * describes it. An element that does both implements both, which is how the
 * two descriptions stay built from the same state instead of drifting apart.
 *
 * describe() is called on the render thread, in a frame, so an implementation
 * that also holds event-thread state locks the same way it does to draw.
 */
class Source {
public:
  Source()          = default;
  virtual ~Source() = default;

  Source(const Source &)            = delete;
  Source &operator=(const Source &) = delete;
  Source(Source &&)                 = delete;
  Source &operator=(Source &&)      = delete;

  /// Say what is there now.
  virtual void describe(Builder &into) = 0;

  /**
   * @brief A number that changes when what this would say changes.
   *
   * Asked every frame; describe() is only called when the total across every
   * source has moved. It does not have to be exact in the direction of saying
   * something changed when it has not -- that costs a rebuild. It must never
   * be wrong the other way: a change that does not move this is a change no
   * assistive technology hears about.
   */
  [[nodiscard]] virtual std::uint64_t accessibilityRevision() const = 0;

  /**
   * @brief Do what the assistive technology asked.
   *
   * Called on the event thread, with a node id this source made. @p value is
   * the new text for Action::SetValue and empty otherwise.
   *
   * @return Whether it was done. False for an action this node does not take,
   *         which is not an error: the platforms ask.
   */
  virtual bool performAction([[maybe_unused]] std::uint64_t nodeId,
                             [[maybe_unused]] Action action,
                             [[maybe_unused]] std::string_view value) {
    return false;
  }
};

/**
 * @brief The byte length of each character in @p text.
 *
 * For Node::characterLengths. A character is a Unicode scalar; a byte that
 * begins no valid sequence counts as one character of one byte, since text
 * that is not quite UTF-8 still has to be describable.
 */
[[nodiscard]] std::vector<std::uint8_t> characterLengths(std::string_view text);

/**
 * @brief The character index each word of @p text begins at.
 *
 * For Node::wordStarts. A word is a run of non-space characters together with
 * the space that follows it, which is the convention the platforms expect:
 * moving to the next word lands on a word rather than on the gap in front of
 * one. Leading whitespace on a line is its own word, which is what makes
 * moving by words in indented text land where a person expects.
 */
[[nodiscard]] std::vector<std::uint8_t> wordStarts(std::string_view text);

/**
 * @brief Characters a text run may hold before it has to be split.
 *
 * Not a performance number. Word starts are character indices and the
 * platforms carry them a byte apiece, so a word beginning past the two hundred
 * and fifty-fifth character of a run cannot be described at all. A run is
 * meant to be a line, and lines are shorter than this; the limit is what
 * happens to text that is not.
 */
inline constexpr std::size_t runLimit = 200;

/**
 * @brief Where to cut @p text into runs.
 *
 * A run per line, since that is what a run is for and what the platforms
 * assume when they decide what "up" and "down" mean. The line break stays on
 * the end of the run it ends, which is what AccessKit asks for and what puts
 * the caret at the end of a line rather than at the start of the next one.
 *
 * A line longer than @ref runLimit is cut again -- at a space where there is
 * one within reach, and at a character boundary where there is not. That is a
 * line break an assistive technology will believe in, which is wrong; it is
 * the least wrong thing available, and it only happens to text with no line
 * breaks of its own for two hundred characters.
 *
 * @return Byte offsets into @p text: the start of each run, and finally its
 *         size. Empty text gives one empty run, because a document with
 *         nothing in it still has a place the caret can be.
 */
[[nodiscard]] std::vector<std::size_t> runBreaks(std::string_view text);

/**
 * @brief How many characters into @p text a byte offset falls.
 *
 * The conversion between the caret, which is a byte offset, and a text
 * position, which is a character index. Offsets past the end give the length,
 * and one inside a character gives the index of the character it is inside.
 */
[[nodiscard]] std::size_t characterIndexOf(std::string_view text,
                                           std::size_t byteOffset);

} // namespace gleditor::a11y

#endif // GLEDITOR_A11Y_TREE_H
// vi: set sw=2 sts=2 ts=2 et:
