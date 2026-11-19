// The dr_wav, dr_flac, dr_mp3, and stb_vorbis implementations, compiled once.

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>
#include <stb_vorbis.c>  // NOLINT(bugprone-suspicious-include): stb ships its decoder as one .c file
