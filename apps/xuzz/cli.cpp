/**
 * @file cli.cpp
 * @brief Unified CLI option parser and configuration for Xuzz.
 */
#include "cli.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "common/ui/xanadoc/session.hpp"
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
            "it is not there. Defaults to \"default\" in the xanadocs folder, "
            "$XDG_DATA_HOME/xuzz/xanadocs")
      .default_value((xanadu::xanadocsDirectory() / "default").string());

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

  parser.add_argument("--whole-pages")
      .help("frame whole pages, and fit linked documents together, rather than "
            "framing for reading")
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

  parser.add_argument("--compare-diff")
      .help("show hypertime map in comparison diff HUD page")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--link-directory")
      .help("open Link Directory dialog on startup")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--link-directory-drawer")
      .help("open Link Directory dialog as docked sidebar drawer")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--link-directory-search")
      .help("initial search query for Link Directory")
      .default_value(std::string{});

  parser.add_argument("--link-directory-filter")
      .help("initial link type filter for Link Directory")
      .default_value(std::string{});

  parser.add_argument("--link-directory-balance")
      .help("initial link balance filter for Link Directory (1:1, 1:N, N:M)")
      .default_value(std::string{});

  parser.add_argument("--link-directory-advanced")
      .help("open advanced primedia span inspection in Link Directory")
      .default_value(false)
      .implicit_value(true);

  parser.add_argument("--width")
      .help("initial window width in pixels")
      .default_value(0)
      .scan<'i', int>();

  parser.add_argument("--height")
      .help("initial window height in pixels")
      .default_value(0)
      .scan<'i', int>();

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

  hiddenUnlessDetailed(parser.add_argument("--xanadoc", "--xudu"))
      .help("load a Xanadu store path or document")
      .default_value(std::string{});

  hiddenUnlessDetailed(parser.add_argument("--slice"))
      .help("load a ZigZag slice or store path")
      .default_value(std::string{});

  hiddenUnlessDetailed(parser.add_argument("--lens"))
      .help("project a filesystem path or a sql: URI "
            "(sql:sqlite:///abs/db.sqlite) through a lens and show its "
            "cells (use with --raster to print and exit)")
      .default_value(std::string{});

  hiddenUnlessDetailed(parser.add_argument("--sql-browse"))
      .help("with --lens sql:..., browse one schema table or view")
      .default_value(std::string{});

  hiddenUnlessDetailed(parser.add_argument("--sql-query"))
      .help("with --lens sql:..., run one read-only SELECT/VALUES/WITH")
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
  hiddenUnlessDetailed(parser.add_argument("--test-publication-swarm"))
      .help("publication worker listen HOST:PORT with explicit mock identity "
            "verification; testing only")
      .default_value(std::string{});
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
      .help("open a complete signed publication with cached dependencies; "
            "repeatable")
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

  hiddenUnlessDetailed(parser.add_argument("--session-export"))
      .help("export self-contained session package (.xuzzpkg) to destination "
            "path")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--session-import"))
      .help("import self-contained session package (.xuzzpkg) from source path")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--session-doc"))
      .help("document name or store path to export into session package")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--session-password"))
      .help("passphrase for ChaCha20-Poly1305 container encryption/decryption")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--session-station"))
      .help("destination station directory when importing a session package")
      .default_value(std::string{});
  hiddenUnlessDetailed(parser.add_argument("--session-links"))
      .help("link package keys to bundle into session package; repeatable")
      .append();
  hiddenUnlessDetailed(parser.add_argument("--editions"))
      .help("comma-separated edition names to export into session package; "
            "repeatable")
      .append();
}

std::optional<CliOptions> CliParser::parse(argparse::ArgumentParser &parser,
                                           AppStateRef state, const int argc,
                                           char **argv) {
  if (argc > 1 && std::string_view(argv[1]) == "session") {
    CliOptions opts;
    opts.isSessionCommand = true;
    if (argc < 3) {
      std::cerr << "Usage: xuzz session <export|import> [options]\n";
      return std::nullopt;
    }
    const std::string_view action = argv[2];
    if (action == "export") {
      opts.isSessionExport = true;
      argparse::ArgumentParser exportParser("xuzz session export");
      exportParser.add_argument("--out", "-o")
          .help("output .xuzzpkg path")
          .required();
      exportParser.add_argument("--doc", "-d")
          .help("document name or store path")
          .default_value(std::string{});
      exportParser.add_argument("--store", "-s")
          .help("store path")
          .default_value(std::string{});
      exportParser.add_argument("--password", "-p")
          .help("passphrase for encryption")
          .default_value(std::string{});
      exportParser.add_argument("--version-id", "-v")
          .help("version IDs or aliases to include (comma-separated)")
          .append();
      exportParser.add_argument("--editions", "-e")
          .help("editions to include (comma-separated)")
          .append();
      exportParser.add_argument("--links", "-l")
          .help("link package keys to include")
          .append();
      exportParser.add_argument("--permascroll")
          .help("custom permascroll path")
          .default_value(std::string{});
      exportParser.add_argument("--headless", "--batch")
          .help("run headless")
          .default_value(false)
          .implicit_value(true);

      std::vector<std::string> subArgs;
      subArgs.push_back("xuzz session export");
      for (int i = 3; i < argc; ++i) {
        subArgs.push_back(argv[i]);
      }
      try {
        exportParser.parse_args(subArgs);
      } catch (const std::exception &err) {
        std::cerr << err.what() << "\n" << exportParser;
        return std::nullopt;
      }

      opts.sessionExportPath = exportParser.get<std::string>("--out");
      opts.sessionDoc        = exportParser.get<std::string>("--doc");
      opts.storePath         = exportParser.get<std::string>("--store");
      if (opts.storePath.empty() && !opts.sessionDoc.empty()) {
        opts.storePath = opts.sessionDoc;
      }
      if (opts.storePath.empty()) {
        opts.storePath = (xanadu::xanadocsDirectory() / "default").string();
      }
      opts.sessionPassword = exportParser.get<std::string>("--password");
      opts.permascrollPath = exportParser.get<std::string>("--permascroll");
      opts.headless        = (exportParser["--headless"] == true ||
                              exportParser["--batch"] == true);
      if (exportParser.present<std::vector<std::string>>("--version-id")) {
        opts.sessionVersions =
            exportParser.get<std::vector<std::string>>("--version-id");
      }
      if (exportParser.present<std::vector<std::string>>("--editions")) {
        opts.sessionEditions =
            exportParser.get<std::vector<std::string>>("--editions");
      }
      if (exportParser.present<std::vector<std::string>>("--links")) {
        opts.sessionLinks =
            exportParser.get<std::vector<std::string>>("--links");
      }
      return opts;
    } else if (action == "import") {
      opts.isSessionImport = true;
      argparse::ArgumentParser importParser("xuzz session import");
      importParser.add_argument("--in", "-i")
          .help("input .xuzzpkg path")
          .required();
      importParser.add_argument("--station", "-s")
          .help("destination station directory")
          .default_value(std::string{});
      importParser.add_argument("--password", "-p")
          .help("passphrase for decryption")
          .default_value(std::string{});
      importParser.add_argument("--permascroll")
          .help("custom permascroll path")
          .default_value(std::string{});
      importParser.add_argument("--headless", "--batch")
          .help("run headless")
          .default_value(false)
          .implicit_value(true);

      std::vector<std::string> subArgs;
      subArgs.push_back("xuzz session import");
      for (int i = 3; i < argc; ++i) {
        subArgs.push_back(argv[i]);
      }
      try {
        importParser.parse_args(subArgs);
      } catch (const std::exception &err) {
        std::cerr << err.what() << "\n" << importParser;
        return std::nullopt;
      }

      opts.sessionImportPath = importParser.get<std::string>("--in");
      opts.sessionStationDir = importParser.get<std::string>("--station");
      opts.sessionPassword   = importParser.get<std::string>("--password");
      opts.permascrollPath   = importParser.get<std::string>("--permascroll");
      opts.headless          = (importParser["--headless"] == true ||
                                importParser["--batch"] == true);
      return opts;
    } else {
      std::cerr << "Unknown session command: " << action << "\n";
      return std::nullopt;
    }
  }

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

  opts.hasExplicitStore = parser.is_used("store");
  opts.storePath        = parser.get<std::string>("store");
  if (opts.storePath.empty()) {
    opts.storePath = (xanadu::xanadocsDirectory() / "default").string();
  }
  if (const auto xanadocPath = parser.get<std::string>("--xanadoc");
      !xanadocPath.empty()) {
    opts.storePath = xanadocPath;
  }
  if (const auto slicePath = parser.get<std::string>("--slice");
      !slicePath.empty()) {
    opts.slicePath = slicePath;
  }
  if (const auto lensPath = parser.get<std::string>("--lens");
      !lensPath.empty()) {
    opts.lensPath = lensPath;
  }
  if (const auto sqlBrowse = parser.get<std::string>("--sql-browse");
      !sqlBrowse.empty()) {
    opts.sqlBrowse = sqlBrowse;
  }
  if (const auto sqlQuery = parser.get<std::string>("--sql-query");
      !sqlQuery.empty()) {
    opts.sqlQuery = sqlQuery;
  }

  opts.rasterMode = (parser["--raster"] == true);
  opts.headless   = (parser["--headless"] == true || parser["--batch"] == true);
  opts.quiet      = (parser["--print-asset-dir"] == true || opts.headless);
  opts.showConfig = (parser["--show-config"] == true);
  opts.checkAuthorshipPath = parser.get<std::string>("--check-authorship");

  opts.askedVersion = parser.get<std::string>("--version-id");
  opts.alongside    = parser.get<std::string>("--alongside");
  opts.publishAs    = parser.get<std::string>("--publish");
  opts.testPublicationSwarm =
      parser.get<std::string>("--test-publication-swarm");

  opts.permascrollPath     = parser.get<std::string>("--permascroll");
  opts.dumpPermascrollPath = parser.get<std::string>("--dump-permascroll");
  opts.exportOsmic         = (parser["--export-osmic"] == true);

  opts.onionSkin        = (parser["--onion-skin"] == true);
  opts.mapVisible       = (parser["--map"] == true);
  opts.compareDiff      = (parser["--compare-diff"] == true);
  opts.pouchOpen        = (parser["--pouch"] == true);
  opts.telescopeVisible = (parser["--telescope"] == true);
  opts.physicsEnabled =
      (parser["--physics"] == true || parser["--tension-layout"] == true);
  opts.noBeams    = (parser["--no-beams"] == true);
  opts.noSworph   = (parser["--no-sworph"] == true);
  opts.wholePages = (parser["--whole-pages"] == true);

  opts.linkDirectoryVisible = (parser["--link-directory"] == true);
  opts.linkDirectoryDrawer  = (parser["--link-directory-drawer"] == true);
  opts.linkDirectorySearch = parser.get<std::string>("--link-directory-search");
  opts.linkDirectoryFilter = parser.get<std::string>("--link-directory-filter");
  opts.linkDirectoryBalance =
      parser.get<std::string>("--link-directory-balance");
  opts.linkDirectoryAdvanced = (parser["--link-directory-advanced"] == true);

  const int winW = parser.get<int>("--width");
  const int winH = parser.get<int>("--height");
  if (winW > 0) state->view.screenWidth = winW;
  if (winH > 0) state->view.screenHeight = winH;

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
  opts.sessionExportPath = parser.get<std::string>("--session-export");
  opts.sessionImportPath = parser.get<std::string>("--session-import");
  opts.sessionDoc        = parser.get<std::string>("--session-doc");
  opts.sessionPassword   = parser.get<std::string>("--session-password");
  opts.sessionStationDir = parser.get<std::string>("--session-station");
  if (!opts.sessionExportPath.empty()) {
    opts.isSessionCommand = true;
    opts.isSessionExport  = true;
  }
  if (!opts.sessionImportPath.empty()) {
    opts.isSessionCommand = true;
    opts.isSessionImport  = true;
  }
  if (parser.present<std::vector<std::string>>("--session-links")) {
    opts.sessionLinks = parser.get<std::vector<std::string>>("--session-links");
  }
  if (parser.present<std::vector<std::string>>("--editions")) {
    opts.sessionEditions = parser.get<std::vector<std::string>>("--editions");
  }

  opts.backend = gleditor::applyCommonArguments(parser, state, argc, argv);
  return opts;
}

} // namespace xuzz
