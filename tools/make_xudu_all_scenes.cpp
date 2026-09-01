#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <xudu/core/link_package.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>
#include <xudu/session.hpp>

using namespace xudu;

int main() {
  std::filesystem::create_directories("scratch");

  // Scenario 1: Sparse / Focused Transclusion
  {
    std::filesystem::remove_all("scratch/store_s1");
    Session session("scratch/store_s1");
    auto &st = session.store(0);

    std::string textOriginal =
        "AS WE MAY THINK\nVannevar Bush - July 1945\n\n"
        "Consider a future device for individual use, which is a sort of "
        "mechanized private file and library. "
        "It needs a name, and to coin one at random, 'memex' will do. A memex "
        "is a device in which an individual "
        "stores all his books, records, and communications, and which is "
        "mechanized so that it may be consulted with "
        "exceeding speed and flexibility. It is an enlarged intimate supplement "
        "to his memory.\n\n"
        "The process of tying two items together is the important thing. When "
        "the user is building a trail, "
        "he names it, inserts the nickname in his code book, and taps it out on "
        "his keyboard.\n";

    auto vOriginal = st.insert(MicroversionId{}, 0, textOriginal);

    std::string essayPrefix =
        "PROJECT XANADU AND DEEP TRANSCLUSION\nTed Nelson - 1965\n\n"
        "In 1945, Bush envisioned the memex:\n\n> ";
    auto vEssay = st.insert(MicroversionId{}, 0, essayPrefix);

    auto vTrans =
        st.transclude(vEssay, st.textOf(vEssay).size(), vOriginal, 140, 240);

    std::string essaySuffix =
        "\n\nIn Xanadu, this quotation is not copied text, but a live "
        "optical transclusion window. "
        "The glowing glass ribbon bridges the two passages across the shared "
        "3D cosmos.\n";
    auto vFinal = st.insert(vTrans, st.textOf(vTrans).size(), essaySuffix);

    auto verOrig  = st.rebuild(vOriginal);
    auto verFinal = st.rebuild(vFinal);
    Link l1;
    l1.type  = LinkType::Quotation;
    l1.owner = "ted";
    l1.left  = verOrig.spansFor(140, 240);
    l1.right = verFinal.spansFor(essayPrefix.size(), 240);
    auto vWithLink = st.addLink(vOriginal, l1);

    session.save(0);

    std::ofstream out("scratch/s1_meta.txt");
    out << vWithLink.str() << " " << vFinal.str() << "\n";
  }

  // Scenario 2: Dense Hypertext Web with 5 Multilateral Links
  {
    std::filesystem::remove_all("scratch/store_s2");
    Session session("scratch/store_s2");
    auto &st = session.store(0);

    std::string original =
        "HYPERTEXT PRINCIPLES\nTed Nelson - Literary Machines\n\n"
        "[1] Non-sequential writing with branching reading paths.\n\n"
        "[2] Two-way visible links preserving contextual provenance.\n\n"
        "[3] Universal transclusion replacing copy-paste duplication.\n\n"
        "[4] Micro-versioning of all modifications and operations.\n\n"
        "[5] Wall-less 3D space with continuous physics tension.\n";

    auto vOrig = st.insert(MicroversionId{}, 0, original);

    std::string critique =
        "SYSTEM CRITIQUE & ARCHITECTURAL COMMENTARY\n\n"
        "Point 1 Commentary: Branching paths require spatial cognitive maps to "
        "prevent disorientation.\n\n"
        "Point 2 Commentary: Bidirectional links eliminate 404 dead-ends "
        "across distributed docuverses.\n\n"
        "Point 3 Commentary: Transclusion enforces attribution and copyright "
        "royalties automatically.\n\n"
        "Point 4 Commentary: Micro-version trees allow hypertime scrubbing back "
        "to document origins.\n\n"
        "Point 5 Commentary: Spring layouts balance readability against "
        "optical ribbon aesthetic harmony.\n";

    auto vCrit = st.insert(MicroversionId{}, 0, critique);

    auto verOrig = st.rebuild(vOrig);
    auto verCrit = st.rebuild(vCrit);

    Link l1;
    l1.type  = LinkType::Comment;
    l1.owner = "critic";
    l1.left  = verOrig.spansFor(44, 55);
    l1.right = verCrit.spansFor(47, 72);
    auto v1  = st.addLink(vOrig, l1);

    Link l2;
    l2.type  = LinkType::Disagreement;
    l2.owner = "critic";
    l2.left  = verOrig.spansFor(105, 59);
    l2.right = verCrit.spansFor(125, 78);
    auto v2  = st.addLink(v1, l2);

    Link l3;
    l3.type  = LinkType::Quotation;
    l3.owner = "critic";
    l3.left  = verOrig.spansFor(170, 60);
    l3.right = verCrit.spansFor(209, 77);
    auto v3  = st.addLink(v2, l3);

    Link l4;
    l4.type  = LinkType::Illustration;
    l4.owner = "critic";
    l4.left  = verOrig.spansFor(236, 56);
    l4.right = verCrit.spansFor(292, 75);
    auto v4  = st.addLink(v3, l4);

    Link l5;
    l5.type  = LinkType::Comment;
    l5.owner = "critic";
    l5.left  = verOrig.spansFor(298, 55);
    l5.right = verCrit.spansFor(373, 80);
    auto v5  = st.addLink(v4, l5);

    session.save(0);

    std::ofstream out("scratch/s2_meta.txt");
    out << v5.str() << " " << vCrit.str() << "\n";
  }

  // Scenario 4: Overlapping Link Anchors (4 Overlapping Links Filling Margin)
  {
    std::filesystem::remove_all("scratch/store_s4");
    Session session("scratch/store_s4");
    auto &st = session.store(0);

    std::string original =
        "INTERTWINGLED HYPERTEXT STRUCTURES\nTed Nelson - Literary Machines\n\n"
        "[1] Universal transclusion and bidirectional linking across docs.\n\n"
        "[2] Multi-layered commentary with concurrent critical notes.\n\n"
        "[3] Contextual provenance tracking preserved in hypertime.\n";

    auto vOrig = st.insert(MicroversionId{}, 0, original);

    std::string critique =
        "MULTILATERAL COMMENTARY MATRIX\n\n"
        "Comment 1 (Quotation): Transclusion enforces exact textual attribution.\n\n"
        "Comment 2 (Analysis): Bidirectional links prevent 404 dead-ends.\n\n"
        "Comment 3 (Correction): Granular provenance requires cryptographic trees.\n\n"
        "Comment 4 (Illustration): Optical ribbons visually bridge shared passages.\n";

    auto vCrit = st.insert(MicroversionId{}, 0, critique);

    auto verOrig = st.rebuild(vOrig);
    auto verCrit = st.rebuild(vCrit);

    // 4 overlapping links attaching to the same Point [1] span in Doc 1 (bytes 69..133)
    // 1. Quotation (Mint)
    Link l1;
    l1.type  = LinkType::Quotation;
    l1.owner = "ted";
    l1.left  = verOrig.spansFor(69, 64);
    l1.right = verCrit.spansFor(34, 70);
    auto v1  = st.addLink(vOrig, l1);

    // 2. Comment (Azure)
    Link l2;
    l2.type  = LinkType::Comment;
    l2.owner = "critic_a";
    l2.left  = verOrig.spansFor(69, 32);
    l2.right = verCrit.spansFor(108, 64);
    auto v2  = st.addLink(v1, l2);

    // 3. Disagreement / Correction (Coral)
    Link l3;
    l3.type  = LinkType::Disagreement;
    l3.owner = "critic_b";
    l3.left  = verOrig.spansFor(95, 38);
    l3.right = verCrit.spansFor(176, 73);
    auto v3  = st.addLink(v2, l3);

    // 4. Illustration (Gold)
    Link l4;
    l4.type  = LinkType::Illustration;
    l4.owner = "illustrator";
    l4.left  = verOrig.spansFor(69, 64);
    l4.right = verCrit.spansFor(253, 73);
    auto v4  = st.addLink(v3, l4);

    session.save(0);

    std::ofstream out("scratch/s4_meta.txt");
    out << v4.str() << " " << vCrit.str() << "\n";
  }

  // Scenario 5: Multi-Span Disambiguation vs Separate Links of Same Type
  {
    std::filesystem::remove_all("scratch/store_s5");
    Session session("scratch/store_s5");
    auto &st = session.store(0);

    std::string sourceText =
        "DISAMBIGUATION TEST: SEPARATE LINKS VS MULTI-SPAN\n\n"
        "[A1] First separate comment passage with independent topic.\n\n"
        "[A2] Second separate comment passage on another point.\n\n"
        "[B1] Multi-span link primary thesis passage.\n\n"
        "[B2] Multi-span link concluding evidence passage.\n";

    auto vSrc = st.insert(MicroversionId{}, 0, sourceText);

    std::string targetText =
        "COMMENTARY TARGET MATRIX\n\n"
        "Target A1: Analysis for separate Link 1 (Comment type).\n\n"
        "Target A2: Analysis for separate Link 2 (Comment type).\n\n"
        "Target B: Comprehensive synthesis for multi-span Link (covers B1 and B2).\n";

    auto vTgt = st.insert(MicroversionId{}, 0, targetText);

    auto verSrc = st.rebuild(vSrc);
    auto verTgt = st.rebuild(vTgt);

    // Link 1: Separate Comment Link 1 (A1 -> Target A1)
    Link l1;
    l1.type  = LinkType::Comment;
    l1.owner = "curator_1";
    l1.left  = verSrc.spansFor(53, 56);
    l1.right = verTgt.spansFor(26, 54);
    auto v1  = st.addLink(vSrc, l1);

    // Link 2: Separate Comment Link 2 of SAME TYPE (A2 -> Target A2)
    Link l2;
    l2.type  = LinkType::Comment;
    l2.owner = "curator_2";
    l2.left  = verSrc.spansFor(113, 50);
    l2.right = verTgt.spansFor(84, 54);
    auto v2  = st.addLink(v1, l2);

    // Link 3: Single Multi-Span Link (covers B1 and B2 on left -> Target B on right)
    Link l3;
    l3.type  = LinkType::Comment;
    l3.owner = "curator_3";
    auto spanB1 = verSrc.spansFor(167, 43);
    auto spanB2 = verSrc.spansFor(214, 46);
    l3.left = spanB1;
    l3.left.insert(l3.left.end(), spanB2.begin(), spanB2.end());
    l3.right = verTgt.spansFor(142, 73);
    auto v3  = st.addLink(v2, l3);

    session.save(0);

    std::ofstream out("scratch/s5_meta.txt");
    out << v3.str() << " " << vTgt.str() << "\n";
  }

  std::cout << "All Xudu scenes created successfully.\n";
  return 0;
}
