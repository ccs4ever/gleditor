"""Reserved colours the tree spends today (throwaway spike data).

Each entry: (family, tier, name, source, rgb as hex int or float triple).
tier "core" = a colour with a fixed meaning shared across the interface
(link types, transclusion, focus, accent, tether, ribbons, authorship);
tier "ext" = per-author / per-lineage / badge palettes and HUD tints, which
are categorical decoration rather than a meaning a dimension could be
mistaken for.
"""

RESERVED_COLOURS = [
    # red
    ("red", "core", "Disagreement link", "link_layout.cpp:481 linkColour", 0xFF7A6B),
    ("red", "ext", "Rose collaborator", "session.cpp:64", 0xF43F5E),
    ("red", "ext", "Command-bar error", "zigzag_visualizer.cpp:2365", 0xEF4444),
    # gold / amber / orange
    ("gold", "core", "Illustration link", "link_layout.cpp linkColour", 0xFFC46B),
    ("gold", "core", "Identity Gold transclusion beam", "beams.cpp:1703,1776", 0xFFD700),
    ("gold", "core", "Transcopyright amber beam", "beams.cpp:1796", 0xF59E0B),
    ("gold", "core", "ZigZag focus_color", "zigzag_visualizer.hpp:104", (0.956, 0.773, 0.259)),
    ("gold", "core", "Pouch Notes aura (Identity Gold)", "pouch_zone.cpp:145", 0xEAB308),
    ("gold", "core", "Transclusion highlight", "session.hpp:194", 0xFFE9A8),
    ("gold", "ext", "Structure/HUD gold", "zigzag_visualizer.cpp:2164", 0xF4C542),
    ("gold", "ext", "Opcode gold", "zigzag_visualizer.cpp:2269", 0xFBBF24),
    ("gold", "ext", "Orange collaborator", "session.cpp:70", 0xF97316),
    ("gold", "ext", "Lineage amber", "hypertime_graph.cpp:41", 0xFFB74D),
    # green
    ("green", "core", "Quotation link", "link_layout.cpp linkColour", 0x7FE0A8),
    ("green", "core", "Pouch Scratch aura (emerald)", "pouch_zone.cpp:154", 0x10B981),
    ("green", "core", "Link-mark colour", "session.hpp:195", 0xB9E8C4),
    ("green", "ext", "Green collaborator", "session.cpp:66", 0x22C55E),
    ("green", "ext", "Emerald badge / gc stdlib", "zigzag_visualizer.cpp:2273", 0x34D399),
    ("green", "ext", "Lineage emerald", "hypertime_graph.cpp:43", 0x4CAF50),
    # cyan / blue
    ("cyan/blue", "core", "Theme accent", "theme.hpp:39", (0.25, 0.7, 0.95)),
    ("cyan/blue", "core", "Sky tether / anchor / satelloid", "beams.cpp:1019,1297", 0x38BDF8),
    ("cyan/blue", "core", "Pouch To-Link-left aura (cyan)", "pouch_zone.cpp:127", 0x06B6D4),
    ("cyan/blue", "core", "Comment link", "link_layout.cpp linkColour", 0x7FB2FF),
    ("cyan/blue", "core", "Quote-cell cyan override", "zigzag_visualizer.cpp:1284", (0.22, 0.74, 0.97)),
    ("cyan/blue", "ext", "Tether satellite", "zigzag_visualizer.cpp:1043", (0.20, 0.80, 0.90)),
    ("cyan/blue", "ext", "HUD dimension list blue", "zigzag_visualizer.cpp:2192", 0x70B0FF),
    ("cyan/blue", "ext", "Lineage blue", "hypertime_graph.cpp:45", 0x42A5F5),
    ("cyan/blue", "ext", "Lineage cyan", "hypertime_graph.cpp:40", 0x00E5FF),
    # purple / violet / magenta
    ("purple/magenta", "core", "Authorship link", "link_layout.cpp linkColour", 0xB98CFF),
    ("purple/magenta", "core", "Dimension link", "link_layout.cpp linkColour", 0xAA00FF),
    ("purple/magenta", "core", "Pouch To-Link-right aura (magenta)", "pouch_zone.cpp:136", 0xEC4899),
    ("purple/magenta", "core", "Xanalink ribbon magenta (named, no literal)", "system_docs.cpp:128; pouch_zone.cpp:131 uses EC4899", 0xEC4899),
    ("purple/magenta", "ext", "Purple collaborator", "session.cpp:65", 0xA855F7),
    ("purple/magenta", "ext", "Lavender badge", "zigzag_visualizer.cpp:2275,2346", 0xC084FC),
    ("purple/magenta", "ext", "Lineage purple", "hypertime_graph.cpp:42", 0xBA68C8),
    ("purple/magenta", "ext", "Lineage pink", "hypertime_graph.cpp:44", 0xFF80AB),
    ("purple/magenta", "ext", "Indigo badge", "zigzag_visualizer.cpp:2337", 0x818CF8),
]
