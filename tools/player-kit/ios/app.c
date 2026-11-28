/* The app's own source file: the game is all in the two libraries the
 * project links (QILIN_LIFTED_LIB, QILIN_KIT_LIB; main is the kit's, which
 * starts SDL and UIKit). Xcode links a target only if it has a file of its
 * own to compile. */
extern const char qilin_ios_app[];
const char qilin_ios_app[] = "Qilin recompilation, built from the player's own disc";
