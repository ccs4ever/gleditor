/**
 * @file published_vocabularies_test.cpp
 * @brief Unit tests for Section 5.11: Published vocabularies: dimension
 *        identity across documents.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/published_vocabulary.hpp"
#include "common/xanadu/quoted_structure.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace {

using namespace xanadu;

struct TestStore {
  std::shared_ptr<UserPermascroll> perma;
  std::unique_ptr<Store> store;
  MicroversionId head;
  std::string scrollKey;

  explicit TestStore(std::string key = "btpk:1111:doc")
      : perma(std::make_shared<UserPermascroll>()),
        store(std::make_unique<Store>(perma)), scrollKey(std::move(key)) {
    head = store->sliceGenesis(MicroversionId{});
    store->setBootstrapPermascroll(scrollKey, nullptr);
  }

  zigzag::DimRef makeDim(std::string_view name) {
    const auto m = store->makeDimension(head, name);
    head         = m.version;
    return m.dim;
  }

  zigzag::CellRef makeCell(std::string_view text) {
    head = store->makeCell(head, text);
    return store->cellRefOf(head);
  }
};

// 1. aVocabularyReleasePinsItsWholeRank
TEST(PublishedVocabulariesTest, aVocabularyReleasePinsItsWholeRank) {
  TestStore alice("btpk:aaaa:alice_vocab");

  // Alice creates terms "supports" and "refutes"
  const auto tSupports = alice.makeCell("supports");
  const auto tRefutes  = alice.makeCell("refutes");

  std::vector<zigzag::CellRef> terms1{tSupports, tRefutes};
  const auto rel1 =
      publishVocabularyRelease(*alice.store, alice.head, zigzag::noCell,
                               "Argumentation Vocab v1", terms1);
  alice.head = rel1.version;

  // Bob adopts Release 1
  TestStore bob("btpk:bbbb:bob");
  const auto adoptedBob = adoptVocabulary(*bob.store, bob.head, rel1.vocabulary,
                                          "Bob's Adopted ArgVocab");
  bob.head              = adoptedBob.version;

  InMemoryForeignSource source;
  source.registerDocument(rel1.vocabulary.state, alice.store.get(), nullptr);

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  zigzag::ArenaManifold bobArena(&bobFold, bob.store.get());
  resolveQuotations(bobArena, bobFold, *bob.store, source, QuotationBudget{});

  // Verify Bob's arena materialises 2 terms along d.dims
  const auto dimVocabOpt = bobArena.dimensionNamed(kDimVocab, *bob.store);
  ASSERT_TRUE(dimVocabOpt.has_value());
  const auto entryCell = bobArena.linked(adoptedBob.quotationCell, *dimVocabOpt,
                                         zigzag::DimVector::POS);
  EXPECT_NE(entryCell, zigzag::noCell);

  const auto bobDimsOpt = bobArena.dimensionNamed(kDimDims, *bob.store);
  ASSERT_TRUE(bobDimsOpt.has_value());

  auto curTerm =
      bobArena.linked(entryCell, *bobDimsOpt, zigzag::DimVector::POS);
  EXPECT_NE(curTerm, zigzag::noCell);
  int bobTermCount = 0;
  while (curTerm != zigzag::noCell && curTerm != bob.store->homeCell()) {
    bobTermCount++;
    curTerm = bobArena.linked(curTerm, *bobDimsOpt, zigzag::DimVector::POS);
  }
  EXPECT_EQ(bobTermCount, 2);

  // Now Alice adds term "qualifies" and publishes Release 2
  const auto tQualifies = alice.makeCell("qualifies");
  std::vector<zigzag::CellRef> terms2{tSupports, tRefutes, tQualifies};
  const auto rel2 =
      publishVocabularyRelease(*alice.store, alice.head, rel1.releaseCell,
                               "Argumentation Vocab v2", terms2);
  alice.head = rel2.version;

  // Charlie adopts Release 2
  TestStore charlie("btpk:cccc:charlie");
  const auto adoptedCharlie = adoptVocabulary(
      *charlie.store, charlie.head, rel2.vocabulary, "Charlie's ArgVocab");
  charlie.head = adoptedCharlie.version;

  source.registerDocument(rel2.vocabulary.state, alice.store.get(), nullptr);

  const auto charlieFold = charlie.store->rebuildManifold(charlie.head);
  zigzag::ArenaManifold charlieArena(&charlieFold, charlie.store.get());
  resolveQuotations(charlieArena, charlieFold, *charlie.store, source,
                    QuotationBudget{});

  const auto charlieVocabOpt =
      charlieArena.dimensionNamed(kDimVocab, *charlie.store);
  ASSERT_TRUE(charlieVocabOpt.has_value());
  const auto charlieEntry = charlieArena.linked(
      adoptedCharlie.quotationCell, *charlieVocabOpt, zigzag::DimVector::POS);
  EXPECT_NE(charlieEntry, zigzag::noCell);

  const auto charlieDimsOpt =
      charlieArena.dimensionNamed(kDimDims, *charlie.store);
  ASSERT_TRUE(charlieDimsOpt.has_value());

  curTerm = charlieArena.linked(charlieEntry, *charlieDimsOpt,
                                zigzag::DimVector::POS);
  EXPECT_NE(curTerm, zigzag::noCell);
  int charlieTermCount = 0;
  while (curTerm != zigzag::noCell && curTerm != charlie.store->homeCell()) {
    charlieTermCount++;
    curTerm =
        charlieArena.linked(curTerm, *charlieDimsOpt, zigzag::DimVector::POS);
  }
  EXPECT_EQ(charlieTermCount, 3);

  // Assert quotations of the two releases materialise different d.dims ranks
  EXPECT_NE(bobTermCount, charlieTermCount);
  EXPECT_EQ(bobTermCount, 2);
  EXPECT_EQ(charlieTermCount, 3);
}

// 2. adoptingAVocabularyDoesNotFileEveryTerm
TEST(PublishedVocabulariesTest, adoptingAVocabularyDoesNotFileEveryTerm) {
  TestStore alice("btpk:aaaa:alice_large_vocab");

  std::vector<zigzag::CellRef> terms;
  terms.reserve(20);
  for (int i = 1; i <= 20; ++i) {
    terms.push_back(alice.makeCell("term_" + std::to_string(i)));
  }

  const auto rel = publishVocabularyRelease(
      *alice.store, alice.head, zigzag::noCell, "Large Vocab 20", terms);
  alice.head = rel.version;

  TestStore bob("btpk:bbbb:bob");
  const auto adopted =
      adoptVocabulary(*bob.store, bob.head, rel.vocabulary, "Adopted Large");
  bob.head = adopted.version;

  const auto bobFoldAfter = bob.store->rebuildManifold(bob.head);

  // Quotation creates quotation support dimensions (d.quotes, d.quotes-state,
  // d.quotes-sel), but does NOT file placeholders for any of the 20 foreign
  // terms
  EXPECT_EQ(bobFoldAfter.refusedOps(), 0U);
  for (const auto &termCell : terms) {
    const auto slot = alice.store->rebuildManifold(alice.head).slot(termCell);
    ASSERT_TRUE(slot.has_value());
    const GlobalOpRef gRef{
        .scroll   = alice.scrollKey,
        .produces = alice.store->segmentedOps().idOf(slot->birthOp),
    };
    EXPECT_FALSE(findAdoptedDimension(*bob.store, gRef).has_value());
  }
}

// 3. usingAPublishedTermMakesItALocalDimension
TEST(PublishedVocabulariesTest, usingAPublishedTermMakesItALocalDimension) {
  TestStore alice("btpk:aaaa:alice_vocab");
  const auto tSupports = alice.makeCell("supports");
  std::vector<zigzag::CellRef> terms{tSupports};
  const auto rel = publishVocabularyRelease(*alice.store, alice.head,
                                            zigzag::noCell, "Rel", terms);
  alice.head     = rel.version;

  const auto aSlot = alice.store->rebuildManifold(alice.head).slot(tSupports);
  ASSERT_TRUE(aSlot.has_value());
  const GlobalOpRef termRef{
      .scroll   = alice.scrollKey,
      .produces = alice.store->segmentedOps().idOf(aSlot->birthOp),
  };

  TestStore bob("btpk:bbbb:bob");
  const auto adopted =
      adoptPublishedDimension(*bob.store, bob.head, termRef, "supports");
  bob.head = adopted.version;

  EXPECT_NE(adopted.local, zigzag::noCell);

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  const auto dims    = bobFold.dimensions();
  EXPECT_TRUE(std::ranges::find(dims, adopted.local) != dims.end());

  // Use the returned DimRef in Store::setLink
  const auto arg1 = bob.makeCell("Claim A");
  const auto arg2 = bob.makeCell("Premise B");
  bob.head        = bob.store->setLink(bob.head, arg1, adopted.local,
                                       zigzag::DimVector::POS, arg2);

  const auto foldAfter = bob.store->rebuildManifold(bob.head);
  EXPECT_EQ(foldAfter.refusedOps(), 0U);
  EXPECT_EQ(foldAfter.linked(arg1, adopted.local, zigzag::DimVector::POS),
            arg2);
  EXPECT_EQ(foldAfter.linked(arg2, adopted.local, zigzag::DimVector::NEG),
            arg1);
}

// 4. filingOneTermTwiceReusesItsPlaceholder
TEST(PublishedVocabulariesTest, filingOneTermTwiceReusesItsPlaceholder) {
  TestStore alice("btpk:aaaa:alice_vocab");
  const auto tSupports = alice.makeCell("supports");
  std::vector<zigzag::CellRef> terms{tSupports};
  const auto rel = publishVocabularyRelease(*alice.store, alice.head,
                                            zigzag::noCell, "Rel", terms);
  alice.head     = rel.version;

  const auto aSlot = alice.store->rebuildManifold(alice.head).slot(tSupports);
  ASSERT_TRUE(aSlot.has_value());
  const GlobalOpRef termRef{
      .scroll   = alice.scrollKey,
      .produces = alice.store->segmentedOps().idOf(aSlot->birthOp),
  };

  TestStore bob("btpk:bbbb:bob");
  const auto adopted1 =
      adoptPublishedDimension(*bob.store, bob.head, termRef, "supports");
  bob.head = adopted1.version;

  const auto dimsCount1 =
      bob.store->rebuildManifold(bob.head).dimensions().size();

  // File the exact same term a second time
  const auto adopted2 =
      adoptPublishedDimension(*bob.store, bob.head, termRef, "supports");
  bob.head = adopted2.version;

  const auto dimsCount2 =
      bob.store->rebuildManifold(bob.head).dimensions().size();

  // One GlobalOpRef, one local dimension cell (interning invariant)
  EXPECT_EQ(adopted1.local, adopted2.local);
  EXPECT_EQ(dimsCount1, dimsCount2);

  const auto found = findAdoptedDimension(*bob.store, termRef);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, adopted1.local);
}

// 5. equalNamesDoNotBindByIdentity
TEST(PublishedVocabulariesTest, equalNamesDoNotBindByIdentity) {
  TestStore alice("btpk:aaaa:alice");
  const auto aliceDim = alice.makeDim("supports");

  TestStore bob("btpk:bbbb:bob");
  const auto bobDim = bob.makeDim("supports");

  const auto mAlice = alice.store->rebuildManifold(alice.head);
  const auto mBob   = bob.store->rebuildManifold(bob.head);

  zigzag::ArenaManifold arena;
  const auto spaceAlice = arena.attach(zigzag::Space{
      .manifold = &mAlice, .store = alice.store.get(), .label = "Alice"});
  const auto spaceBob   = arena.attach(zigzag::Space{
      .manifold = &mBob, .store = bob.store.get(), .label = "Bob"});

  // Bind by identity only
  arena.bindSharedIdentities();

  // Equal names do NOT bind by identity
  EXPECT_EQ(arena.dimIn(spaceBob, aliceDim), zigzag::noCell);
  EXPECT_EQ(arena.dimIn(spaceAlice, bobDim), zigzag::noCell);

  // Now explicitly request NameMatch binding
  arena.bindDimensionsByNameMatch();

  // Now they bind via NameMatch
  EXPECT_EQ(arena.dimIn(spaceBob, aliceDim), bobDim);
  EXPECT_EQ(arena.dimIn(spaceAlice, bobDim), aliceDim);

  const auto arenaDim = arena.dimensionNamed("supports", *alice.store);
  ASSERT_TRUE(arenaDim.has_value());
  const auto bSet = arena.boundDimensionSet(*arenaDim);
  ASSERT_TRUE(bSet.has_value());
  ASSERT_GE(bSet->members.size(), 2U);
  EXPECT_EQ(bSet->members[0].mode, zigzag::DimensionBindingMode::NameMatch);
}

// 6. sharedPublishedIdentityBindsAcrossStores
TEST(PublishedVocabulariesTest, sharedPublishedIdentityBindsAcrossStores) {
  TestStore vocabStore("btpk:vvvv:vocab");
  const auto tSupports = vocabStore.makeCell("supports");
  std::vector<zigzag::CellRef> terms{tSupports};
  const auto rel  = publishVocabularyRelease(*vocabStore.store, vocabStore.head,
                                             zigzag::noCell, "Rel", terms);
  vocabStore.head = rel.version;

  const auto vSlot =
      vocabStore.store->rebuildManifold(vocabStore.head).slot(tSupports);
  ASSERT_TRUE(vSlot.has_value());
  const GlobalOpRef termRef{
      .scroll   = vocabStore.scrollKey,
      .produces = vocabStore.store->segmentedOps().idOf(vSlot->birthOp),
  };

  // Bob files the term
  TestStore bob("btpk:bbbb:bob");
  const auto adoptedBob =
      adoptPublishedDimension(*bob.store, bob.head, termRef, "supports");
  bob.head = adoptedBob.version;

  // Charlie files the same term with a different local alias
  TestStore charlie("btpk:cccc:charlie");
  const auto adoptedCharlie =
      adoptPublishedDimension(*charlie.store, charlie.head, termRef, "sup");
  charlie.head = adoptedCharlie.version;

  const auto mBob     = bob.store->rebuildManifold(bob.head);
  const auto mCharlie = charlie.store->rebuildManifold(charlie.head);

  zigzag::ArenaManifold arena;
  const auto spaceBob     = arena.attach(zigzag::Space{
      .manifold = &mBob, .store = bob.store.get(), .label = "Bob"});
  const auto spaceCharlie = arena.attach(zigzag::Space{
      .manifold = &mCharlie, .store = charlie.store.get(), .label = "Charlie"});

  arena.bindSharedIdentities();

  // Federate across stores: Bob's dimension maps to Charlie's local placeholder
  EXPECT_EQ(arena.dimIn(spaceCharlie, adoptedBob.local), adoptedCharlie.local);
  EXPECT_EQ(arena.dimIn(spaceBob, adoptedCharlie.local), adoptedBob.local);

  const auto arenaDimOpt = arena.dimensionNamed("supports", *bob.store);
  ASSERT_TRUE(arenaDimOpt.has_value());
  const auto bSet = arena.boundDimensionSet(*arenaDimOpt);
  ASSERT_TRUE(bSet.has_value());
  EXPECT_EQ(bSet->members[0].mode,
            zigzag::DimensionBindingMode::SharedIdentity);
}

// 7. renamingATermPreservesItsIdentity
TEST(PublishedVocabulariesTest, renamingATermPreservesItsIdentity) {
  TestStore alice("btpk:aaaa:alice_vocab");
  const auto tSupports = alice.makeCell("supports");
  std::vector<zigzag::CellRef> terms{tSupports};
  const auto rel1 = publishVocabularyRelease(*alice.store, alice.head,
                                             zigzag::noCell, "Rel 1", terms);
  alice.head      = rel1.version;

  const auto aSlot = alice.store->rebuildManifold(alice.head).slot(tSupports);
  ASSERT_TRUE(aSlot.has_value());
  const GlobalOpRef termRef{
      .scroll   = alice.scrollKey,
      .produces = alice.store->segmentedOps().idOf(aSlot->birthOp),
  };

  TestStore bob("btpk:bbbb:bob");
  const auto adoptedBob =
      adoptPublishedDimension(*bob.store, bob.head, termRef, "supports");
  bob.head = adoptedBob.version;

  // Alice renames term cell to "strongly-supports" and publishes Release 2
  alice.head =
      alice.store->setCellText(alice.head, tSupports, "strongly-supports");
  const auto rel2 = publishVocabularyRelease(*alice.store, alice.head,
                                             rel1.releaseCell, "Rel 2", terms);
  alice.head      = rel2.version;

  // Charlie files the renamed term from Release 2
  TestStore charlie("btpk:cccc:charlie");
  const auto adoptedCharlie = adoptPublishedDimension(
      *charlie.store, charlie.head, termRef, "strongly-supports");
  charlie.head = adoptedCharlie.version;

  const auto mBob     = bob.store->rebuildManifold(bob.head);
  const auto mCharlie = charlie.store->rebuildManifold(charlie.head);

  zigzag::ArenaManifold arena;
  const auto spaceBob     = arena.attach(zigzag::Space{
      .manifold = &mBob, .store = bob.store.get(), .label = "Bob"});
  const auto spaceCharlie = arena.attach(zigzag::Space{
      .manifold = &mCharlie, .store = charlie.store.get(), .label = "Charlie"});

  arena.bindSharedIdentities();

  // Identity is preserved despite label edit
  EXPECT_EQ(arena.dimIn(spaceCharlie, adoptedBob.local), adoptedCharlie.local);
  EXPECT_EQ(arena.dimIn(spaceBob, adoptedCharlie.local), adoptedBob.local);
}

// 8. replacingATermDoesNotHijackExistingBindings
TEST(PublishedVocabulariesTest, replacingATermDoesNotHijackExistingBindings) {
  TestStore alice("btpk:aaaa:alice_vocab");
  const auto tSupports1 = alice.makeCell("supports");
  std::vector<zigzag::CellRef> terms1{tSupports1};
  const auto rel1 = publishVocabularyRelease(*alice.store, alice.head,
                                             zigzag::noCell, "Rel 1", terms1);
  alice.head      = rel1.version;

  const auto slot1 = alice.store->rebuildManifold(alice.head).slot(tSupports1);
  ASSERT_TRUE(slot1.has_value());
  const GlobalOpRef termRef1{
      .scroll   = alice.scrollKey,
      .produces = alice.store->segmentedOps().idOf(slot1->birthOp),
  };

  TestStore bob("btpk:bbbb:bob");
  const auto adoptedBob =
      adoptPublishedDimension(*bob.store, bob.head, termRef1, "supports");
  bob.head = adoptedBob.version;

  // Alice mints a replacement term T2 (a brand new cell) with the same spelling
  // "supports"
  const auto tSupports2 = alice.makeCell("supports");
  std::vector<zigzag::CellRef> terms2{tSupports2};
  const auto rel2 = publishVocabularyRelease(*alice.store, alice.head,
                                             rel1.releaseCell, "Rel 2", terms2);
  alice.head      = rel2.version;

  const auto slot2 = alice.store->rebuildManifold(alice.head).slot(tSupports2);
  ASSERT_TRUE(slot2.has_value());
  const GlobalOpRef termRef2{
      .scroll   = alice.scrollKey,
      .produces = alice.store->segmentedOps().idOf(slot2->birthOp),
  };

  EXPECT_NE(termRef1.produces, termRef2.produces);

  TestStore charlie("btpk:cccc:charlie");
  const auto adoptedCharlie = adoptPublishedDimension(
      *charlie.store, charlie.head, termRef2, "supports");
  charlie.head = adoptedCharlie.version;

  const auto mBob     = bob.store->rebuildManifold(bob.head);
  const auto mCharlie = charlie.store->rebuildManifold(charlie.head);

  zigzag::ArenaManifold arena;
  const auto spaceBob     = arena.attach(zigzag::Space{
      .manifold = &mBob, .store = bob.store.get(), .label = "Bob"});
  const auto spaceCharlie = arena.attach(zigzag::Space{
      .manifold = &mCharlie, .store = charlie.store.get(), .label = "Charlie"});

  arena.bindSharedIdentities();

  // Existing documents still bind to original address; replacement does not
  // hijack
  EXPECT_EQ(arena.dimIn(spaceCharlie, adoptedBob.local), zigzag::noCell);
  EXPECT_EQ(arena.dimIn(spaceBob, adoptedCharlie.local), zigzag::noCell);
}

// 9. anUnavailableVocabularyIsAVisibleCitation
TEST(PublishedVocabulariesTest, anUnavailableVocabularyIsAVisibleCitation) {
  TestStore alice("btpk:aaaa:alice_vocab");
  const auto tSupports = alice.makeCell("supports");
  std::vector<zigzag::CellRef> terms{tSupports};
  const auto rel = publishVocabularyRelease(*alice.store, alice.head,
                                            zigzag::noCell, "Rel 1", terms);
  alice.head     = rel.version;

  TestStore bob("btpk:bbbb:bob");
  const auto adopted = adoptVocabulary(*bob.store, bob.head, rel.vocabulary,
                                       "Unavailable Vocab");
  bob.head           = adopted.version;

  // Foreign source has NO record for Alice
  InMemoryForeignSource emptySource;

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, emptySource, QuotationBudget{});

  // Quotation head remains visible in arena
  EXPECT_TRUE(arena.contains(adopted.quotationCell));
  EXPECT_EQ(bobFold.refusedOps(), 0U);

  // No guessed name binding occurs
  const auto dims = arena.dimensions();
  for (const auto d : dims) {
    const auto bSet = arena.boundDimensionSet(d);
    if (bSet.has_value()) {
      for (const auto &m : bSet->members) {
        EXPECT_NE(m.mode, zigzag::DimensionBindingMode::NameMatch);
      }
    }
  }
}

// 10. updatingAVocabularyIsAnAuthoredRepoint
TEST(PublishedVocabulariesTest, updatingAVocabularyIsAnAuthoredRepoint) {
  TestStore alice("btpk:aaaa:alice_vocab");
  const auto tSupports = alice.makeCell("supports");
  const auto tRefutes  = alice.makeCell("refutes");
  std::vector<zigzag::CellRef> terms1{tSupports, tRefutes};
  const auto rel1 = publishVocabularyRelease(*alice.store, alice.head,
                                             zigzag::noCell, "Rel 1", terms1);
  alice.head      = rel1.version;

  TestStore bob("btpk:bbbb:bob");
  const auto q1 =
      adoptVocabulary(*bob.store, bob.head, rel1.vocabulary, "ArgVocab");
  const auto vBobAdoption = q1.version;
  bob.head                = vBobAdoption;

  // Alice publishes Release 2 with a third term "qualifies"
  const auto tQualifies = alice.makeCell("qualifies");
  std::vector<zigzag::CellRef> terms2{tSupports, tRefutes, tQualifies};
  const auto rel2 = publishVocabularyRelease(*alice.store, alice.head,
                                             rel1.releaseCell, "Rel 2", terms2);
  alice.head      = rel2.version;

  // Bob updates his adopted vocabulary to Release 2 via authored repoint
  bob.head = updateAdoptedVocabulary(*bob.store, bob.head, q1.quotationCell,
                                     rel2.vocabulary);
  const auto vBobRepoint = bob.head;

  // 1. Assert the quotation head's R7 chain contains multiple operations
  const auto foldRepointed = bob.store->rebuildManifold(vBobRepoint);
  const auto slotRepointed = foldRepointed.slot(q1.quotationCell);
  ASSERT_TRUE(slotRepointed.has_value());
  EXPECT_NE(slotRepointed->birthOp, slotRepointed->lastOp);

  InMemoryForeignSource source;
  source.registerDocument(rel1.vocabulary.state, alice.store.get(), nullptr);
  source.registerDocument(rel2.vocabulary.state, alice.store.get(), nullptr);

  // 2. Scrubbed / folded at earlier version (vBobAdoption): materialises
  // Release 1 (2 terms)
  const auto foldEarly = bob.store->rebuildManifold(vBobAdoption);
  zigzag::ArenaManifold arenaEarly(&foldEarly, bob.store.get());
  resolveQuotations(arenaEarly, foldEarly, *bob.store, source,
                    QuotationBudget{});

  const auto dimVocabEarly = arenaEarly.dimensionNamed(kDimVocab, *bob.store);
  ASSERT_TRUE(dimVocabEarly.has_value());
  const auto entryEarly = arenaEarly.linked(q1.quotationCell, *dimVocabEarly,
                                            zigzag::DimVector::POS);
  EXPECT_NE(entryEarly, zigzag::noCell);

  const auto dimsEarly = arenaEarly.dimensionNamed(kDimDims, *bob.store);
  ASSERT_TRUE(dimsEarly.has_value());
  auto cur = arenaEarly.linked(entryEarly, *dimsEarly, zigzag::DimVector::POS);
  int earlyCount = 0;
  while (cur != zigzag::noCell && cur != bob.store->homeCell()) {
    earlyCount++;
    cur = arenaEarly.linked(cur, *dimsEarly, zigzag::DimVector::POS);
  }
  EXPECT_EQ(earlyCount, 2);

  // 3. Folded at latest version (vBobRepoint): materialises Release 2 (3 terms)
  zigzag::ArenaManifold arenaLatest(&foldRepointed, bob.store.get());
  resolveQuotations(arenaLatest, foldRepointed, *bob.store, source,
                    QuotationBudget{});

  const auto dimVocabLatest = arenaLatest.dimensionNamed(kDimVocab, *bob.store);
  ASSERT_TRUE(dimVocabLatest.has_value());
  const auto entryLatest = arenaLatest.linked(q1.quotationCell, *dimVocabLatest,
                                              zigzag::DimVector::POS);
  EXPECT_NE(entryLatest, zigzag::noCell);

  const auto dimsLatest = arenaLatest.dimensionNamed(kDimDims, *bob.store);
  ASSERT_TRUE(dimsLatest.has_value());
  cur = arenaLatest.linked(entryLatest, *dimsLatest, zigzag::DimVector::POS);
  int latestCount = 0;
  while (cur != zigzag::noCell && cur != bob.store->homeCell()) {
    latestCount++;
    cur = arenaLatest.linked(cur, *dimsLatest, zigzag::DimVector::POS);
  }
  EXPECT_EQ(latestCount, 3);
}

} // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
