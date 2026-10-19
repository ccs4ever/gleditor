# Native AT-SPI citation download validation

The final frozen Accessibility-enabled Xuzz binary was run through the real publication download
form in an isolated X11/Xvfb session. The run verified the requested carrier, edition, publisher
label, unchecked verification state, unavailable-connection message, and an AT-SPI `Action` on
`Retry download`. The action selected the option; an ordinary scheduled Return submitted it. The
form then showed `Queued`, and the next scheduled Return refreshed it to the same explicit
no-connection failure. The app exited with code 0.

This is a hybrid accessibility interaction. The requested carrier fingerprint, edition, verification
value, and status are exposed as accessible names on native nodes. The requested carrier and edition
are list items under combo boxes; the failure is a label. Those fields do not expose AT-SPI `Text`
interfaces in the captured tree. The retry option does expose an `Action` interface and its `click`
action. The archive preserves both snapshots, the event sequence, the application’s `--dump-a11y`
output, and the native frames so this distinction can be checked directly.

The form displayed carrier `f8ff688c5680ab88c27e4297decb5e7640b86a31`, edition `document / a34 #2`,
publication name `document`, and publisher `1: 4c6e92c9b3e4abbd75ca25d8fc7cfaa3`. Verification
remained `Not checked`. The status said no publication connection was configured and content
availability had not been checked. The Retry submission produced a `Queued` status, then Refresh
returned to the no-connection failure. This run used no mock transport and establishes no claim
about remote content or publisher availability.

The run used app SHA-256 `a1ff64ffb8462dec9594cf4523a3879940140a7f55123ec7e61da00ff628637c` and
AccessKit-enabled `libgleditor.so.0` SHA-256
`dbc8f98265ce47d0900b85a65550a4b5d0e3c43a8a4ba0be6d0055558f65dec1`. A fresh profile copied only the
sixteenth-increment UI-authored data and config; cache and GPG home were new and empty. The source
document operation dump and store-file hashes matched before and after. Package visibility bytes
were identical. Activity decoding showed one completed visit with current visit 1 both before and
after; the activity operation dump was unchanged.

The five 1440×1000 PPM captures are preserved losslessly as compressed PPM. Each PNG was decoded and
compared pixel-for-pixel with its PPM source. `frame-provenance.json` records the PPM, RGB, and PNG
hashes. The focused A11y build and test logs are included separately; the test log records 33
passing tests from six suites, and both build and test exit files record 0.

Evidence is under [`native-atspi/`](native-atspi/). The plan, exact helper source, invocation,
environment, native accessibility snapshots, app logs, frame hashes, and focused A11y logs are
included. No profile, key material, or executable binary is archived.
