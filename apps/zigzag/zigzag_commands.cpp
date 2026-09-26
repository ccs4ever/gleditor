/**
 * @file zigzag_commands.cpp
 * @brief ZigZag's keyboard commands, for every program that shows a slice.
 */
#include "zigzag_commands.hpp"

#include <string>
#include <string_view>
#include <utility>

#include <gleditor/app.hpp>

#include "common/xanadu/system_docs.hpp"
#include "zigzag_visualizer.hpp"

namespace zigzag {

namespace settings = xanadu::settings;

void registerZigzagCommands(gleditor::CommandTable &table,
                            const std::shared_ptr<ZigzagVisualizer> &viz,
                            ZigzagCommandHooks hooks) {
  const auto hooksPtr = std::make_shared<ZigzagCommandHooks>(std::move(hooks));
  // One action under both its names: the bare key while ZigZag has the
  // keyboard, the Alt twin from anywhere.
  const auto both =
      [&table](const std::string_view name, const std::string_view twin,
               const std::string &help, const std::function<void()> &run) {
        table.registerAction(std::string(name), help, run);
        if (!twin.empty()) {
          table.registerAction(std::string(twin), help, run);
        }
      };
  const auto moving = [viz, hooksPtr](const std::string_view action) {
    return [viz, hooksPtr, action] {
      viz->dispatchAction(action);
      if (hooksPtr->focusMoved) {
        hooksPtr->focusMoved();
      }
    };
  };

  // Views and bundles.
  const auto content = [viz] {
    viz->setViewMode(ZigzagVisualizer::ViewMode::CellContent);
  };
  const auto topology = [viz] {
    viz->setViewMode(ZigzagVisualizer::ViewMode::Topology);
  };
  both(settings::kKeymapViewModeContent1,
       settings::kKeymapZigzagViewModeContent,
       "switch to Cell Content View (full content & XYZ alignment)", content);
  both(settings::kKeymapViewModeContentV, {},
       "switch to Cell Content View (full content & XYZ alignment)", content);
  both(settings::kKeymapViewModeTopology,
       settings::kKeymapZigzagViewModeTopology,
       "switch to Topology View (fixed-size cells & lattice geometry)",
       topology);
  both(settings::kKeymapViewModeTopologyT, {},
       "switch to Topology View (fixed-size cells & lattice geometry)",
       topology);
  const auto bundle = [viz](const ZigzagVisualizer::DimensionBundle which) {
    return [viz, which] { viz->setDimensionBundle(which); };
  };
  both(settings::kKeymapBundleExecution, settings::kKeymapZigzagBundleExecution,
       "switch to Execution dimension bundle (d.spin, d.step, d.branch)",
       bundle(ZigzagVisualizer::DimensionBundle::Execution));
  both(settings::kKeymapBundleScope, settings::kKeymapZigzagBundleScope,
       "switch to Scope dimension bundle (d.lexical, d.dynamic, d.env)",
       bundle(ZigzagVisualizer::DimensionBundle::Scope));
  both(settings::kKeymapBundleContract, settings::kKeymapZigzagBundleContract,
       "switch to Contract dimension bundle (d.require, d.ensure, "
       "d.invariant)",
       bundle(ZigzagVisualizer::DimensionBundle::Contract));
  both(settings::kKeymapBundleLogic, settings::kKeymapZigzagBundleLogic,
       "switch to Logic dimension bundle (d.clause, d.predicate, d.var)",
       bundle(ZigzagVisualizer::DimensionBundle::Logic));
  both(settings::kKeymapBundleStdlib, settings::kKeymapZigzagBundleStdlib,
       "switch to Stdlib dimension bundle (d.stdlib, d.symbol, d.version)",
       bundle(ZigzagVisualizer::DimensionBundle::Stdlib));
  both(settings::kKeymapBundleCycle, settings::kKeymapZigzagBundleCycle,
       "cycle active dimension bundle forward",
       [viz] { viz->cycleDimensionBundle(true); });

  // The palette and the omnibar.
  both(settings::kKeymapTogglePalette, settings::kKeymapZigzagTogglePalette,
       "toggle Vortex opcode and library palette HUD",
       [viz] { viz->togglePalette(); });
  both(settings::kKeymapVqlTranslateAttach,
       settings::kKeymapZigzagVqlTranslateAttach,
       "translate VQL filter text and attach to active chain", [viz] {
         if (viz->isPaletteVisible()) {
           viz->paletteTranslateVQL();
           viz->setPaletteVisible(false);
         }
       });
  both(settings::kKeymapToggleCommandBar,
       settings::kKeymapZigzagToggleCommandBar,
       "toggle interactive VQL Command Omnibar",
       [viz] { viz->toggleCommandBar(); });
  const auto openBar = [viz](const char prefix) {
    return [viz, prefix] {
      viz->setCommandBarVisible(true);
      if (viz->commandBarText().empty()) {
        viz->commandBarInputChar(prefix);
      }
    };
  };
  both(settings::kKeymapOpenCommandBarSlash,
       settings::kKeymapZigzagOpenCommandBarSlash,
       "open VQL Command Omnibar with '/' navigation prefix", openBar('/'));
  both(settings::kKeymapOpenCommandBarColon,
       settings::kKeymapZigzagOpenCommandBarColon,
       "open VQL Command Omnibar with ':' command prefix", openBar(':'));
  both(settings::kKeymapConfirmAction, {},
       "run the Omnibar, clone the palette's symbol, or follow the focused "
       "cell",
       [viz, hooksPtr] {
         if (viz->isCommandBarVisible()) {
           viz->executeCommandBar();
         } else if (viz->isPaletteVisible()) {
           viz->paletteCloneSelectedToFocus();
           viz->setPaletteVisible(false);
         } else if (hooksPtr->activateFocus) {
           hooksPtr->activateFocus();
         }
       });
  both(settings::kKeymapDismissOverlay, {},
       "dismiss the Omnibar or palette, or leave ZigZag", [viz, hooksPtr] {
         if (viz->isCommandBarVisible()) {
           viz->setCommandBarVisible(false);
         } else if (viz->isPaletteVisible()) {
           viz->setPaletteVisible(false);
         } else if (hooksPtr->leave) {
           hooksPtr->leave();
         }
       });

  // Moving the focus. Up and down walk the palette while it is open.
  both(settings::kKeymapStepXPos, settings::kKeymapZigzagStepXPos,
       "step focus positive along X dimension", moving("step-x-pos"));
  both(settings::kKeymapStepXNeg, settings::kKeymapZigzagStepXNeg,
       "step focus negative along X dimension", moving("step-x-neg"));
  const auto upDown = [viz, hooksPtr](const bool up) {
    return [viz, hooksPtr, up] {
      if (viz->isPaletteVisible()) {
        up ? viz->palettePrev() : viz->paletteNext();
        return;
      }
      viz->dispatchAction(up ? "step-y-pos" : "step-y-neg");
      if (hooksPtr->focusMoved) {
        hooksPtr->focusMoved();
      }
    };
  };
  both(settings::kKeymapStepYPos, settings::kKeymapZigzagStepYPos,
       "step focus positive along Y dimension", upDown(true));
  both(settings::kKeymapStepYNeg, settings::kKeymapZigzagStepYNeg,
       "step focus negative along Y dimension", upDown(false));
  both(settings::kKeymapStepZPos, settings::kKeymapZigzagStepZPos,
       "step focus positive along Z dimension", moving("step-z-pos"));
  both(settings::kKeymapStepZNeg, settings::kKeymapZigzagStepZNeg,
       "step focus negative along Z dimension", moving("step-z-neg"));
  both(settings::kKeymapSwapXY, settings::kKeymapZigzagSwapXY,
       "swap X and Y dimension bindings", moving("swap-xy"));
  both(settings::kKeymapCycleDimsForward,
       settings::kKeymapZigzagCycleDimsForward,
       "cycle active dimension bindings forward", moving("cycle-dims-forward"));
  both(settings::kKeymapCycleDimsBackward,
       settings::kKeymapZigzagCycleDimsBackward,
       "cycle active dimension bindings backward",
       moving("cycle-dims-backward"));
  both(settings::kKeymapJumpHome, settings::kKeymapZigzagJumpHome,
       "jump focus to home cell", moving("jump-home"));
  both(settings::kKeymapHopHead, settings::kKeymapZigzagHopHead,
       "hop to head of current rank along X dimension", moving("hop-head"));
  both(settings::kKeymapHopTail, settings::kKeymapZigzagHopTail,
       "hop to tail of current rank along X dimension", moving("hop-tail"));

  // Editing the slice.
  both(settings::kKeymapDuplicateFocusCell,
       settings::kKeymapZigzagDuplicateCell,
       "duplicate focused cell along d.clone", moving("duplicate-focus-cell"));
  both(settings::kKeymapInsertCellXPos, {},
       "insert connected cell positive along active X dimension",
       moving("insert-cell-x-pos"));
  both(settings::kKeymapInsertCellXNeg, {},
       "insert connected cell negative along active X dimension",
       moving("insert-cell-x-neg"));
  both(settings::kKeymapInsertCellYPos, {},
       "insert connected cell positive along active Y dimension",
       moving("insert-cell-y-pos"));
  both(settings::kKeymapInsertCellYNeg, {},
       "insert connected cell negative along active Y dimension",
       moving("insert-cell-y-neg"));
  both(settings::kKeymapUnlinkXPos, {},
       "unlink focused cell along positive X dimension",
       moving("unlink-x-pos"));
  both(settings::kKeymapUnlinkXNeg, {},
       "unlink focused cell along negative X dimension",
       moving("unlink-x-neg"));
  both(settings::kKeymapDeleteFocusCell, {}, "delete currently focused cell",
       moving("delete-focus-cell"));
  both(settings::kKeymapDeleteFocusCellBksp, {},
       "delete currently focused cell", moving("delete-focus-cell"));
}

} // namespace zigzag
