# The two games' details for the packaging scripts (sourced):
# game=<toycommander|toyracer>. Everything game-specific in a download comes
# from here: the README templates' @NAME@ places (fill) and kit/game.txt
# (write_game_txt), which is all the launcher knows about the game
# (tools/player-kit/launcher/ADOPTING.md).
REPO="Qilin1066/ToyCommander--QilinRecomp"   # both games' releases are published here
case "$game" in
toycommander)
    TITLE="Toy Commander"; PACKAGE="ToyCommander--QilinRecomp"; EXE="ToyCommander.exe"; BOOT="1GUTH.BIN"
    BUILD_TITLE="Toy Commander (USA)"; RELEASE="the USA release, MK-57020, version 1.022"; PRODUCT="MK-57020"; DISC_VERSION="1.022"
    STUDIO="No Cliché / Sega, Dreamcast, 1999"
    ;;
toyracer)
    TITLE="Toy Racer"; PACKAGE="ToyRacer--QilinRecomp"; EXE="ToyRacer.exe"; BOOT="1TOYRACE.BIN"
    BUILD_TITLE="Toy Racer (Europe)"; RELEASE="the European release, MK-5114950, version 1.050"; PRODUCT="MK-5114950"; DISC_VERSION="1.050"
    STUDIO="No Cliché / Sega, Dreamcast, 2000"
    ;;
*)
    echo "unknown game '$game' (toycommander or toyracer)" >&2
    exit 2
    ;;
esac
# Fills a template's @NAME@ places with the game's details.
fill() {
    sed -e "s|@GAME@|$game|g" -e "s|@TITLE@|$TITLE|g" -e "s|@PACKAGE@|$PACKAGE|g" -e "s|@EXE@|$EXE|g" \
        -e "s|@BOOT@|$BOOT|g" -e "s|@BUILD_TITLE@|$BUILD_TITLE|g" -e "s|@RELEASE@|$RELEASE|g" -e "s|@STUDIO@|$STUDIO|g" "$1"
}
# Writes kit/game.txt, one name=value a line:
#   write_game_txt <file> <exe> <compiler> [<compiler_args>]
# with Windows line ends where GAME_TXT_CRLF=1. Every key is written, the
# launcher's defaults too, so the file says the whole story: the game's
# names, the disc it builds from (`build` is recomp/src/bootfile.cpp's known
# build's title, which `qilin identify --build` checks), how it is recompiled
# (one step: qilin lift into 64 files), what a compile needs, how the game
# starts, and where releases are published.
write_game_txt() {
    {
        printf 'game=%s\ntitle=%s\nexe=%s\nboot=%s\n' "$game" "$TITLE" "$2" "$BOOT"
        printf 'build=%s\nproduct=%s\ndisc_version=%s\nrelease=%s\n' "$BUILD_TITLE" "$PRODUCT" "$DISC_VERSION" "$RELEASE"
        printf 'package=%s\nrepo=%s\nrelease_tag_prefix=%s-v\nrelease_asset_prefix=%s-\n' "$PACKAGE" "$REPO" "$game" "$PACKAGE"
        printf 'input=file\ninput_extensions=cue;gdi;chd\ninput_noun=disc\n'
        printf "input_hint=Choose your disc's .cue file (with its .bin tracks beside it), its .gdi or its .chd.\n"
        printf 'identify={qilin} identify {input} --json --build {game:build}\n'
        printf 'step.1={qilin} lift {input} --out {gen} --files 64\n'
        printf 'compile_memory_mb=1200\nbuild_space_mb=2048\ncache_mb=2048\nrun_args={input}\n'
        printf 'compiler=%s\n' "$3"
        if [ -n "${4:-}" ]; then printf 'compiler_args=%s\n' "$4"; fi
    } | if [ "${GAME_TXT_CRLF:-0}" = 1 ]; then sed 's/$/\r/'; else cat; fi > "$1"
}
