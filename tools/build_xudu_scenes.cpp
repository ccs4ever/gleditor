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
  // Scenario 1: Dual Document Transclusion & Quotation Beams
  {
    std::filesystem::remove_all("scratch/store_scene1");
    Session session("scratch/store_scene1");
    auto &st = session.store(0);

    std::string textOriginal =
        "MEMEX AND ASSOCIATIVE TRAILS\nVannevar Bush - 1945\n\n"
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
        st.transclude(vEssay, st.textOf(vEssay).size(), vOriginal, 150, 220);

    std::string essaySuffix =
        "\n\nIn Xanadu, this quotation is not copied text, but a live "
        "windowed transclusion. "
        "Notice how the optical glass beam dynamically bridges the two "
        "passages across the shared 3D cosmos.\n";
    auto vFinal = st.insert(vTrans, st.textOf(vTrans).size(), essaySuffix);

    auto verOrig  = st.rebuild(vOriginal);
    auto verFinal = st.rebuild(vFinal);
    Link l1;
    l1.type  = LinkType::Quotation;
    l1.owner = "ted";
    l1.left  = verOrig.spansFor(150, 220);
    l1.right = verFinal.spansFor(essayPrefix.size(), 220);
    auto vWithLink = st.addLink(vOriginal, l1);

    session.save(0);

    std::ofstream out("scratch/scene1_meta.txt");
    out << vWithLink.str() << " " << vFinal.str() << "\n";
  }

  // Scenario 2: Dense Hypertext Web with 5 Multilateral Links
  {
    std::filesystem::remove_all("scratch/store_scene2");
    Session session("scratch/store_scene2");
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

    std::ofstream out("scratch/scene2_meta.txt");
    out << v5.str() << " " << vCrit.str() << "\n";
  }

  std::cout << "Xudu test scenes constructed successfully.\n";
  return 0;
}
