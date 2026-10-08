ARG SWARM_IMAGE_BASE=debian:forky-slim
FROM ${SWARM_IMAGE_BASE}

# Keep the compiler and source in this test image so diagnostics and additional
# harnesses can be built against exactly the libraries used by the peers.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      build-essential pkg-config git ca-certificates libboost-dev libglm-dev \
      libgtest-dev libgmock-dev libfreetype-dev libharfbuzz-dev libfribidi-dev \
      libunibreak-dev libfontconfig-dev libpoppler-cpp-dev libpoppler-private-dev \
      libmagic-dev libvlc-dev libsdl3-dev libsdl3-image-dev \
      libgl-dev libegl1 libgl1-mesa-dri fonts-dejavu-core \
      libtorrent-rasterbar-dev libssl-dev liblmdb-dev librnp-dev libsqlite3-dev \
      libspdlog-dev gnupg iproute2 util-linux python3 xvfb xauth \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/gleditor
COPY . .

ARG GLEDITOR_VERSION=0.0.0
ENV SDL_VIDEODRIVER=offscreen \
    SDL_AUDIODRIVER=dummy \
    LIBGL_ALWAYS_SOFTWARE=1 \
    XDG_DATA_HOME=/work/data \
    XDG_CONFIG_HOME=/work/config \
    XDG_CACHE_HOME=/work/cache \
    PATH=/opt/gleditor/build:/usr/local/sbin:/usr/local/bin:/usr/bin \
    GLEDITOR_DISABLE_VULKAN=1 \
    GLEDITOR_ENABLE_A11Y=0

# Debian's private Poppler headers contain unused parameters in inline stubs.
# Treat those external headers as system headers while keeping project warnings.
# The standard distribution toolchain is selected by the Makefile's compiler
# probe; no compiler or language standard is forced by this recipe.
RUN make -j$(nproc) GLEDITOR_VERSION="${GLEDITOR_VERSION}" \
      CXXFLAGS="-isystem /usr/include/poppler -isystem /usr/include/poppler/cpp" \
      xuzz xudu-dump xudu-swarm-peer xudu_test xuzz_test ui_test \
    && ./build/xudu_test \
      --gtest_filter='PublicationTest.*:PublicationInventoryTest.*:StoreTablesTest.*:PublicationOutboxTest.*:PublicationInboxTest.*:PublicationSubscriptionsTest.*:AuthorCatalogTest.*:PublicationDiscoveryTest.*:BinaryOpsTest.*:MutableLinkTest.*:LinkPackageExchangeTest.*:ReaderLinkPackagesTest.*:LinkPackageTest.*:LinkPackageInteractionTest.*:SpanfiladeBenchmarkTest.ScaledOccurrencesOfSpeedup:HolefiladeBenchmarkTest.ScaledSpanDecompositionSpeedup'

# The network suites need peers supplied by the runner; the default command
# exercises the publication primitives without pretending those peers exist.
CMD ["./build/xudu_test", "--gtest_filter=PublicationTest.*:PublicationInventoryTest.*:StoreTablesTest.*:PublicationOutboxTest.*:PublicationInboxTest.*:PublicationSubscriptionsTest.*:AuthorCatalogTest.*:PublicationDiscoveryTest.*:BinaryOpsTest.*:MutableLinkTest.*:LinkPackageExchangeTest.*:ReaderLinkPackagesTest.*:LinkPackageTest.*:LinkPackageInteractionTest.*:SpanfiladeBenchmarkTest.ScaledOccurrencesOfSpeedup:HolefiladeBenchmarkTest.ScaledSpanDecompositionSpeedup"]
