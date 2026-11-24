#ifndef MIDI_CORE_H
#define MIDI_CORE_H

typedef unsigned int MidiU32;
typedef unsigned short MidiU16;
typedef unsigned char MidiU8;

enum { MIDI_MAX_EVENTS = 6144, MIDI_MAX_TRACKS = 32, MIDI_FILE_LIMIT = 65536 };
enum { MIDI_NOTE_OFF = 0, MIDI_NOTE_ON = 1, MIDI_TEMPO = 2 };
enum {
 MIDI_OK = 0, MIDI_SHORT_HEADER = 1, MIDI_BAD_HEADER = 2,
 MIDI_BAD_FORMAT = 3, MIDI_BAD_DIVISION = 4, MIDI_TRACKS = 5,
 MIDI_TRUNCATED = 6, MIDI_BAD_EVENT = 7, MIDI_EVENTS_FULL = 8,
 MIDI_TIME_OVERFLOW = 9, MIDI_NO_NOTES = 10
};

struct MidiEvent {
 MidiU32 tick;
 MidiU32 due_us;
 MidiU32 extra; // tempo for MIDI_TEMPO
 MidiU16 order;
 MidiU8 note, velocity, kind, channel;
};
struct MidiInfo {
 MidiU32 count, notes, tracks, division, duration_us;
 MidiU32 error;
};
int midi_parse(const MidiU8 *data, MidiU32 bytes, MidiEvent *out,
               MidiU32 capacity, MidiInfo *info);
#endif
