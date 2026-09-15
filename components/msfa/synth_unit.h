/*
 * Copyright 2012 Google Inc.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "controllers.h"
#include "dx7note.h"
#include "lfo.h"
#include "ringbuffer.h"
#include "resofilter.h"

struct ActiveNote {
  int midi_note;
  bool keydown;
  bool sustained;
  bool live;
  int silent_blocks;   // tab5: consecutive near-silent blocks after key-up (see GetSamples)
  Dx7Note *dx7_note;
};

class SynthUnit {
 public:
  static void Init(double sample_rate);

  explicit SynthUnit(RingBuffer *ring_buffer);

  void GetSamples(int n_samples, int16_t *buffer);

  // tab5: accessors used by the Tab5 synth app (voice editing, meters).
  void ProgramChange(int p);
  int  CurrentPatch() const { return current_patch_; }
  void GetUnpacked(char *out156) const { for (int i = 0; i < 156; i++) out156[i] = unpacked_patch_[i]; }
  void SetUnpacked(const char *in156) { for (int i = 0; i < 156; i++) unpacked_patch_[i] = in156[i]; lfo_.reset(unpacked_patch_ + 137); }
  const uint8_t *PatchData() const { return patch_data_; }
  int  LiveNotes() const { int n = 0; for (int i = 0; i < max_active_notes; i++) if (active_note_[i].live) n++; return n; }
 private:
  void TransferInput();

  void ConsumeInput(int n_input_bytes);

  // Choose a note for a new key-down, returns note number, or -1 if
  // none available.
  int AllocateNote();

  // zero-based (declared public above; tab5)

  void SetController(int controller, int value);

  int ProcessMidiMessage(const uint8_t *buf, int buf_size);

  RingBuffer *ring_buffer_;
  static const int max_active_notes = 16;
  ActiveNote active_note_[max_active_notes];
  int current_note_;
  uint8_t input_buffer_[8192];
  size_t input_buffer_index_;

  uint8_t patch_data_[4096];
  int current_patch_;

  char unpacked_patch_[156];

  // The original DX7 had one single LFO. Later units had an LFO per note.
  Lfo lfo_;

  // in MIDI units (0x4000 is neutral)
  Controllers controllers_;

  ResoFilter filter_;
  int32_t filter_control_[3];
  bool sustain_;

  // Extra buffering for when GetSamples wants a buffer not a multiple of N
  int16_t extra_buf_[N];
  int extra_buf_size_;
};
