// The sound chip's output (the AICA's 64 channels and its two external
// inputs, which carry the GD-ROM drive's CD audio), mixed natively.
//
// The channels play from the registers the driver stand-in writes
// (aica.cpp): the start address, the loop's start and end, the format
// (16- and 8-bit PCM, 4-bit ADPCM), loop on or off, octave and FNS, the
// direct send level and pan, the total level; a channel starts and stops
// when KYONEX is written, each by its own KYONB, as on the chip. CD audio
// is the drive's: frame addresses advance 75 a second while it plays, and
// its samples come from the dump's audio tracks.
//
// Everything runs by guest time (clock.hpp): the guest's reads see where a
// channel or the disc is at that moment, and the mixer thread renders up to
// the present every few milliseconds, to the host's sound output
// (QILIN_HEADLESS: none) and to QILIN_WAV=<file> if set. One lock guards it
// all: the sound chip's device functions hold it.
#pragma once

#include <cstdint>
#include <mutex>

namespace qilin::gdrom {
class Disc;
}

namespace toycommander::sound {

constexpr unsigned kRate = 44100;  // the chip's output rate

// The lock, and the chip's state under it. `registers` is the sound chip's
// register file (64 KB, aica.cpp), `ram` its 2 MB of sound RAM.
std::unique_lock<std::mutex> lock();
void attach(const uint8_t* registers, const uint8_t* ram, const qilin::gdrom::Disc& disc);

// With the lock held:
void sync();          // render up to the present guest time
void key_execute();   // KYONEX written: each channel keyed on or off by its KYONB
uint32_t position(unsigned channel);  // the sample a channel plays (its CA)
bool finished(unsigned channel);      // a channel without a loop has played to its loop end

// CD audio, with the lock held. Play tracks `first` to `last` (from the
// first's index 1 to the last's end), `repeats` more times (15: for ever).
// The state's frame addresses are the sector + 150.
void cd_play(unsigned first, unsigned last, uint32_t repeats);
void cd_pause();
void cd_resume();
struct CdState {
    bool playing = false;
    uint32_t at = 0;  // the frame playing, or where play stopped; 0 before any play
};
CdState cd_state();

// Called by the mixer thread with the lock held after each render: the
// driver stand-in's per-frame work (aica.cpp).
void set_tick(void (*tick)());

// The mixer thread and the host's output; after attach.
void start();

}  // namespace toycommander::sound
