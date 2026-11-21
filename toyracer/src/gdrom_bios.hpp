// The boot ROM's GD-ROM system calls (through the vector at 0x8C0000BC), in
// place of the boot ROM, which the port doesn't have: Shinobi's gdd (the
// GD-ROM driver) and gdFs (the file system over it) are lifted, and their
// wrappers at 0x8C0D5D20-0x8C0D5E1C, one per call, are replaced by these.
// The drive is the player's own disc dump. Commands complete as they are
// sent; what each one does and what gdd expects back is in gdrom_bios.cpp.
#pragma once

namespace qilin::gdrom {
class Disc;
}

namespace toyracer {

// The disc the drive holds (it must outlive the run).
void insert_disc(const qilin::gdrom::Disc& disc);
// Registers the system calls' natives.
void provide_gdrom_bios();

}  // namespace toyracer
