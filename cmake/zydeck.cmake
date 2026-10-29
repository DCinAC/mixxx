# ZyDeck's own sources, added to Mixxx's library target (CMakeLists.txt includes this with -DZYDECK=ON, the default in
# this fork). Everything here lives in src/zydeck/; upstream files only get the hooks listed in ZYDECK.md.
target_sources(
  mixxx-lib
  PRIVATE
    src/zydeck/fitsfeature.cpp
    src/zydeck/fitstablemodel.cpp
    src/zydeck/webfolder.cpp
    src/zydeck/stems.cpp
    src/zydeck/sidechaineffect.cpp
    src/zydeck/effectlist.cpp
    src/zydeck/decklist.cpp
    src/zydeck/prefetch.cpp
    src/zydeck/loadrequest.cpp
    src/zydeck/autodjrequest.cpp
    src/zydeck/discordbroadcast.cpp
)
target_compile_definitions(mixxx-lib PUBLIC __ZYDECK__)
