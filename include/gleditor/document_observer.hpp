/**
 * @file document_observer.hpp
 * @brief Being told what changed in a document, as it changes.
 *
 * The document model applies an edit and reflows the pages it disturbed. What
 * it does not do is remember that the edit happened: the text afterwards is
 * the whole of what it keeps, which is all a plain editor needs and is not
 * enough for anything that wants to undo, replay, journal, synchronise or
 * account for an edit after the fact.
 *
 * Rather than growing a history the document model would then own the policy
 * for -- how far back, what is a unit, what happens on a branch -- the edits
 * are reported and a program keeps whatever record it wants. The report
 * carries what was removed as well as where, so a recipient can reverse an
 * edit without having kept its own copy of the text.
 */
#ifndef GLEDITOR_DOCUMENT_OBSERVER_H
#define GLEDITOR_DOCUMENT_OBSERVER_H

#include <cstdint>
#include <string>

class Doc;

namespace gleditor {

/**
 * @brief Notified after a document's text changes.
 *
 * Called on the render thread, with the document's text already spliced and
 * before the pages that show it have been laid out again. An observer may read
 * the document; it must not edit one, because an edit made from inside a
 * notification would reflow a document that is midway through reflowing.
 *
 * Observers are held as bare pointers and are not owned. One must outlive the
 * documents it watches, or remove itself from them first.
 */
class DocumentObserver {
public:
  DocumentObserver()          = default;
  virtual ~DocumentObserver() = default;

  DocumentObserver(const DocumentObserver &)            = delete;
  DocumentObserver &operator=(const DocumentObserver &) = delete;
  DocumentObserver(DocumentObserver &&)                 = delete;
  DocumentObserver &operator=(DocumentObserver &&)      = delete;

  /**
   * @brief Text was inserted.
   * @param doc  Document that changed, already carrying the new text.
   * @param at   Byte offset the text was inserted at.
   * @param utf8 What was inserted.
   */
  virtual void textInserted([[maybe_unused]] Doc &doc,
                            [[maybe_unused]] std::uint32_t at,
                            [[maybe_unused]] const std::string &utf8) {}

  /**
   * @brief Text was removed.
   * @param doc     Document that changed, no longer carrying the text.
   * @param at      Byte offset the removed text began at.
   * @param removed What was removed, so that this can be reversed without the
   *                observer having kept a copy of the document.
   */
  virtual void textErased([[maybe_unused]] Doc &doc,
                          [[maybe_unused]] std::uint32_t at,
                          [[maybe_unused]] const std::string &removed) {}
};

} // namespace gleditor

#endif // GLEDITOR_DOCUMENT_OBSERVER_H
