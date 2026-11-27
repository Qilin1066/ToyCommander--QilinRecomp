# The two games' details for package.sh (sourced): game=<toycommander|toyracer>.
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
