/**
 * @file cli.cpp
 * @brief Unified CLI option parser and configuration for Xuzz.
 */
#include "cli.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "config.h" // for GLEDITOR_VERSION, TOSTRING
#include <gleditor/app.hpp>

namespace fs = std::filesystem;

namespace xuzz {

bool CliParser::wantsEveryOption(const int argc,
                                 const char *const *const argv) {
  for (int i = 1; i < argc; i++) {
    if (nullptr != argv[i] && std::string_view{"--help-all"} == argv[i]) {
      return true;
    }
  }
  return false;
}

void CliParser::buildParser(argparse::ArgumentParser &parser,
                            const bool detailed) {
  gleditor::addCommonArguments(parser, detailed);

  parser.add_argument("store")
      .help("directory the primary spools live in or slice to load; created if "
            "it is not there")
      .default_value(std::string{"xanadoc"});

  parser.add_argument("--view")
      .help("initial presentation mode: unified (default), xanadoc, or zigzag")
      .default_value(std::string{"unified"});

  parser.add_argument("--version-id")
      .help("microversion to open, for example 2a4; the default is the most "
            "recent state in the store")
      .default_value(std::string{});

  parser.add_argument("--alongside")
      .help("a second microversion to show beside the opening one")
      .default_value(std::string{});

  parser.add_argument("--background")
      .help("open this microversion as a background document (depthZ < 0); "
            "repeatable")
      .append();

  parser.add_argument("--no-beams")
      .help("do not draw the connections between documents and cells")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--no-sworph")
      .help("do not let a link coming into view bring its far document over")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--map")
      .help(
          "show the hypertime map on startup; ctrl-h toggles it while running")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--compare")
      .help("compare microversions in hypertime map diff panel, e.g. v1,v2,v3; "
            "repeatable")
      .append();

  parser.add_argument("--alias")
      .help("assign alias to microversion as VERSION:ALIAS; repeatable")
      .append();

  parser.add_argument("--onion-skin")
      .help("visualize open documents stacked in 3D depth with opacity decay")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--pouch")
      .help("open the screen-edge pouch drawer on startup; ctrl-\\ or F2 "
            "toggles it")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--telescope")
      .help("open decentralized swarm telescope overlay on startup; "
            "ctrl-shift-T or F3 toggles it")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--physics")
      .help("enable 3-way tension spring layout simulation for document "
            "positioning")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--tension-layout")
      .help("alias for --physics")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--audio")
      .help(
          "open an audio stream or file as an embedded AudioWidget; repeatable")
      .append();

  parser.add_argument("--video")
      .help(
          "open a video stream or file as an embedded MediaWidget; repeatable")
      .append();

  parser.add_argument("files")
      .help("source files or slices to import or open")
      .remaining();

  const auto hiddenUnlessDetailed =
      [detailed](argparse::Argument &arg) -> argparse::Argument & {
    if (!detailed) {
      arg.hidden();
    }
    return arg;
  };

  hiddenUnlessDetailed(parser.add_argument("--xudu"))
      .help("load a Xudu store path or document")
      .default_value(std::string{});

  hiddenUnlessDetailed(parser.add_argument("--slice"))
      .help("load a ZigZag slice or store path")
      .default_value(std::string{});

  hiddenUnlessDetailed(parser.add_argument("--raster"))
      .help("print 1D/2D raster reading text of the slice to stdout and exit")
      .default_value(false)
      .implicit_value(true);

  if (detailed) {
    parser.add_group("Networking and Swarm options");
  }

  hiddenUnlessDetailed(parser.add_argument("--torrent"))
      .help("a .torrent file, a magnet link, or a name; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--torrent-data"))
      .help("directory where files described by --torrent are")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--swarm"))
      .help("fetch quoted content from the BitTorrent network")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--private-dht"))
      .help("allow more than one DHT node on the same /8 network")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--peer"))
      .help("introduce a peer as HOST:PORT; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--dht-node"))
      .help("bootstrap the DHT from a known node as HOST:PORT; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--collab-room"))
      .help("collaborative room name for real-time swarm editing")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--collab-host"))
      .help("collaborative room host fingerprint")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--collab-name"))
      .help("local author display name for collaborative carets")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--auto-unlock"))
      .help("automatically unlock transcopyright spans on launch")
      .default_value(false)
      .implicit_value(true);

  if (detailed) {
    parser.add_group("Batch import and orchestration options");
  }

  hiddenUnlessDetailed(parser.add_argument("--author-name"))
      .help("name to record on publications made from this machine")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--author-email"))
      .help("email to record alongside the author name")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--gpg-key"))
      .help("fingerprint of the secret key to sign authorship records with")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--author-here"))
      .help("keep the --author-* settings in this store rather than per-user "
            "configuration")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--show-config"))
      .help("print the current configuration and quit")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--check-authorship"))
      .help("verify the signature on this publication, report who signed it, "
            "and quit")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--read"))
      .help("open a published document from a manifest file; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--publish"))
      .help("publish the opening document under this name")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--quote"))
      .help("quote a byte range of a torrent-backed file as "
            "FILE_INDEX,OFFSET,LENGTH; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--import"))
      .help("read files into stores as initial operations; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--import-branch"))
      .help("import a file as a new root microversion branch; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--import-break"))
      .help("insert a page break and append text from file; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--insert-text"))
      .help("insert text into document as DOC:POS:FILE_OR_TEXT; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--structure-script"))
      .help("apply a sequential combined Xanadu/Zigzag store script")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--transclude"))
      .help("transclude span from one doc into another as "
            "SRCDOC:START:LEN,DESTDOC:POS; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--transclude-text"))
      .help("transclude text matching query from one doc into another; "
            "repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--format-link"))
      .help("create formatting link as DOC:START:LEN:ATTR[:TIER[:OWNER]]; "
            "repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--dimension-link"))
      .help("create dimension link as DOC1:START:LEN,DOC2:START:LEN:DIMNAME; "
            "repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--permascroll"))
      .help("directory holding the sovereign user permascroll to bind")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--dump-permascroll"))
      .help("dump sovereign user permascroll bytes to a file upon exit")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--open-store"))
      .help(
          "open an existing xanadoc store as an auxiliary document; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--export-osmic"))
      .help("export human-readable OSMIC text spools alongside binary stores")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--headless", "--batch"))
      .help("run batch commands non-interactively and exit without GUI")
      .default_value(false)
      .implicit_value(true);
  hiddenUnlessDetailed(parser.add_argument("--link"))
      .help("create a link between open document spans; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--system-doc"))
      .help("open a sovereign system xanadoc (keymap, settings, layout, ui, "
            "pouches); repeatable")
      .append();
}

std::optional<CliOptions> CliParser::parse(argparse::ArgumentParser &parser,
                                           AppStateRef state, const int argc,
                                           char **argv) {
  try {
    parser.parse_args(argc, argv);
  } catch (const std::exception &err) {
    std::cerr << err.what() << "\n" << parser;
    return std::nullopt;
  }

  CliOptions opts;
  const std::string progName = fs::path(argv[0]).filename().string();
  if (progName == "zigzag") {
    opts.viewMode = ViewMode::ZigzagOnly;
  } else if (progName == "xudu") {
    opts.viewMode = ViewMode::XanadocOnly;
  } else {
    opts.viewMode = ViewMode::Unified;
  }

  if (const auto vmStr = parser.get<std::string>("--view"); !vmStr.empty()) {
    if (const auto mode = parseViewMode(vmStr)) {
      opts.viewMode = *mode;
    }
  }

  opts.storePath = parser.get<std::string>("store");
  if (const auto xuduPath = parser.get<std::string>("--xudu");
      !xuduPath.empty()) {
    opts.storePath = xuduPath;
  }
  if (const auto slicePath = parser.get<std::string>("--slice");
      !slicePath.empty()) {
    opts.slicePath = slicePath;
  }

  opts.rasterMode = (parser["--raster"] == true);
  opts.headless   = (parser["--headless"] == true || parser["--batch"] == true);
  opts.quiet      = (parser["--print-asset-dir"] == true || opts.headless);
  opts.showConfig = (parser["--show-config"] == true);
  opts.checkAuthorshipPath = parser.get<std::string>("--check-authorship");

  opts.askedVersion = parser.get<std::string>("--version-id");
  opts.alongside    = parser.get<std::string>("--alongside");
  opts.publishAs    = parser.get<std::string>("--publish");

  opts.permascrollPath     = parser.get<std::string>("--permascroll");
  opts.dumpPermascrollPath = parser.get<std::string>("--dump-permascroll");
  opts.exportOsmic         = (parser["--export-osmic"] == true);

  opts.onionSkin        = (parser["--onion-skin"] == true);
  opts.mapVisible       = (parser["--map"] == true);
  opts.pouchOpen        = (parser["--pouch"] == true);
  opts.telescopeVisible = (parser["--telescope"] == true);
  opts.physicsEnabled =
      (parser["--physics"] == true || parser["--tension-layout"] == true);
  opts.noBeams  = (parser["--no-beams"] == true);
  opts.noSworph = (parser["--no-sworph"] == true);

  if (parser.present<std::vector<std::string>>("--audio")) {
    opts.audioMrls = parser.get<std::vector<std::string>>("--audio");
  }
  if (parser.present<std::vector<std::string>>("--video")) {
    opts.videoMrls = parser.get<std::vector<std::string>>("--video");
  }
  if (parser.present<std::vector<std::string>>("--alias")) {
    opts.aliases = parser.get<std::vector<std::string>>("--alias");
  }
  if (parser.present<std::vector<std::string>>("--compare")) {
    opts.compares = parser.get<std::vector<std::string>>("--compare");
  }
  if (parser.present<std::vector<std::string>>("--background")) {
    for (const auto &verStr :
         parser.get<std::vector<std::string>>("--background")) {
      opts.background.push_back(xanadu::MicroversionId::parse(verStr));
    }
  }

  opts.backend = gleditor::applyCommonArguments(parser, state, argc, argv);
  return opts;
}

} // namespace xuzz
