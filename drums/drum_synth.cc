// Copyright 2012 Olivier Gillet.
//
// Author: Olivier Gillet (ol.gillet@gmail.com)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "drum_synth.h"

#include "avrlib/op.h"
#include "avrlib/random.h"
#include "avrlib/time.h"

#include "dsp_utils.h"

#include "resources.h"

using namespace avrlib;

/* extern */
DrumSynth drum_synth;

static const uint8_t kAudioBlockSize = 32;
RingBuffer<AudioBufferSpecs> DrumSynth::AudioBuffer;
RingBuffer<AudioBufferSpecs> DrumSynth::CFBuffer;

/* static */
DrumPatch DrumSynth::patch_[kNumDrumInstruments];

/* static */
DrumState DrumSynth::state_[kNumDrumInstruments];

/* static */
uint8_t DrumSynth::sample_rate_ = 0;

/* static */
uint8_t DrumSynth::sample_counter_ = 0;

/* static */
uint8_t DrumSynth::sample_ = 0;

/* static */
uint32_t DrumSynth::last_event_time_ = 0;

static const prog_uint8_t preset_bd_1[] PROGMEM = { SYNTH_BD, 60, 18, 104, 120, 0 };
static const prog_uint8_t preset_bd_2[] PROGMEM = { SYNTH_BD, 56, 60, 120, 150, 0 };
static const prog_uint8_t preset_bd_3[] PROGMEM = { SYNTH_BD, 60, 42, 130, 180, 14 };
static const prog_uint8_t preset_bd_4[] PROGMEM = { SYNTH_BD, 72, 20, 66, 224, 0 };
static const prog_uint8_t preset_bd_5[] PROGMEM = { SYNTH_BD, 42, 52, 106, 160, 60 };

static const prog_uint8_t preset_sd_1[] PROGMEM = { SYNTH_SD, 108, 18, 16, 72, 64 };
static const prog_uint8_t preset_sd_2[] PROGMEM = { SYNTH_SD, 108, 36, 32, 96, 140 };
static const prog_uint8_t preset_sd_3[] PROGMEM = { SYNTH_SD, 108, 36, 50, 90, 180 };
static const prog_uint8_t preset_sd_4[] PROGMEM = { SYNTH_SD, 116, 36, 32, 80, 150 };
static const prog_uint8_t preset_sd_5[] PROGMEM = { SYNTH_SD, 124, 40, 190, 90, 40 };

static const prog_uint8_t preset_hh_1[] PROGMEM = { SYNTH_HH, 124, 0, 0, 80, 0 };
static const prog_uint8_t preset_hh_2[] PROGMEM = { SYNTH_HH, 150, 0, 0, 80, 0 };
static const prog_uint8_t preset_hh_3[] PROGMEM = { SYNTH_HH, 136, 0, 0, 110, 0 };
static const prog_uint8_t preset_hh_4[] PROGMEM = { SYNTH_HH, 148, 0, 0, 90, 0 };
static const prog_uint8_t preset_hh_5[] PROGMEM = { SYNTH_HH, 154, 0, 0, 45, 0 };

static const prog_uint8_t* drum_presets[] =
{
	preset_bd_1,
	preset_bd_2,
	preset_bd_3,
	preset_bd_4,
	preset_bd_5,

	preset_sd_1,
	preset_sd_2,
	preset_sd_3,
	preset_sd_4,
	preset_sd_5,

	preset_hh_1,
	preset_hh_2,
	preset_hh_3,
	preset_hh_4,
	preset_hh_5,
};

/* static */
void DrumSynth::Init()
{
	memset(state_, 0, sizeof(DrumState) * kNumDrumInstruments);

	LoadPatch(0, 0);
	LoadPatch(1, 8);
	LoadPatch(2, 14);
	LoadPatch(3, 12);
}

/* static */
void DrumSynth::Trigger(uint8_t instrument, uint8_t level)
{
	last_event_time_ = milliseconds();

	// Reset all phases.
	state_[instrument].phase = 0;
	state_[instrument].pitch_env_phase = 0;
	state_[instrument].amp_env_phase = 0;

	// Initialize envelope increments
	state_[instrument].pitch_env_increment = pgm_read_word(lut_res_drm_env_increments + patch_[instrument].pitch_decay);
	state_[instrument].amp_env_increment = pgm_read_word( lut_res_drm_env_increments + patch_[instrument].amp_decay);
	state_[instrument].level = U8U8MulShift8(level, patch_[instrument].level);
}

/* static */
void DrumSynth::LoadPatch(uint8_t instrument, uint8_t patch)
{
	patch_[instrument].level = 255;
	const prog_uint8_t* a = drum_presets[patch];
	uint8_t* address = (uint8_t*)(&patch_[instrument].synth);
	for (uint8_t i = 0; i < 6; ++i)
	{
		address[i] = pgm_read_byte(a + i);
	}
}

/* static */
void DrumSynth::MorphPatch(uint8_t instrument, uint8_t value)
{
	uint8_t synth = patch_[instrument].synth;
	patch_[instrument].level = 255;
	uint8_t offset = instrument * 5 + (value >> 6);
	uint8_t balance = value << 2;
	const prog_uint8_t* a = drum_presets[offset];
	const prog_uint8_t* b = drum_presets[offset + 1];
	uint8_t* address = (uint8_t*)(&patch_[instrument].synth);
	for (uint8_t i = 0; i < 6; ++i)
	{
		address[i] = U8Mix(pgm_read_byte(a + i), pgm_read_byte(b + i), balance);
	}

	patch_[instrument].synth = synth;
}

static const prog_uint8_t drums_cc_map[] PROGMEM =
{
	// BD
	0, 1, 2, 3, 4, 5,
	// SD
	6, 7, 8, 9, 10, 11,
	//
	12, 15, 17
};

/* static */
void DrumSynth::SetParameterCc(uint8_t cc, uint8_t value)
{
	if (cc < 16 || cc > 30)
	{
		return;
	}
	uint8_t address = pgm_read_byte(drums_cc_map + cc - 16);
	uint8_t* data = static_cast<uint8_t*>(static_cast<void*>(patch_));
	data[address] = value << 1;
}

/* static */
void DrumSynth::SetBandwidth(uint8_t bandwidth)
{
	bandwidth = ~bandwidth;
	// sample_rate = U8U8MulShift8(sample_rate, sample_rate);
	sample_rate_ = bandwidth >> 3;
}

/* static */
void DrumSynth::SetBalance(uint8_t mix)
{
	if (mix < 128)
	{
		patch_[0].level = 255;
		patch_[1].level = mix << 1;
	}
	else
	{
		patch_[0].level = ~((mix - 128) << 1);
		patch_[1].level = 255;
	}
	patch_[2].level = patch_[1].level >> 1;
}

/* static */
void DrumSynth::Render(uint8_t cutoff, avrlib::RingBuffer<AudioBufferSpecs>& buffer, uint8_t instruments)
{
	if (drum_synth.idle_time_ms() > 300000)
	{
		buffer.Overwrite(0);
		return;
	}

	uint8_t sample = sample_;
	uint8_t sample_counter = sample_counter_;
	while (buffer.writable() >= kAudioBlockSize)
	{
		UpdateModulations();
		uint8_t noise = Random::state_msb();

		for (uint8_t i = 0; i < kAudioBlockSize; ++i)
		{
			++sample_counter;
			int16_t cf = cutoff;
			int16_t mix = 128;
			noise = (noise * 73) + 1;

			// Linear interpolation optimized for the case when the delta
			// between adjacent samples is in the -127..+127 range.

			for(uint8_t j = 0; j < kNumDrumInstruments; ++j)
			{
				uint16_t& phase = state_[j].phase;							
				phase += state_[j].phase_increment;

				switch (patch_[j].synth)
				{
				default:
				case SYNTH_BD:
					{
						Word bd_sample_pair;
						bd_sample_pair.value = pgm_read_word(wav_res_sine + (phase >> 8));
						int8_t bd = bd_sample_pair.bytes[0];
						int8_t bd_2 = bd_sample_pair.bytes[1];
						bd += S8U8MulShift8(bd_2 - bd, phase);
						mix += S8U8MulShift8(bd, state_[j].amp_level);
						cf += state_[j].amp_level;
						break;
					}
				case SYNTH_SD:
					{
						int8_t sd = pgm_read_byte(wav_res_sine + (phase >> 8));
						mix += S8U8MulShift8(sd, state_[j].amp_level);
						mix += S8U8MulShift8(noise, state_[j].amp_level_noise);
						cf += state_[j].amp_level;
						break;
					}
				case SYNTH_HH:
					{
						int8_t hh = pgm_read_byte(wav_res_hh + U16ShiftRight4(phase));
						mix += S8U8MulShift8(hh, state_[j].amp_level);
						cf += state_[j].amp_level;
						break;
					}
				}
			}

			if (sample_counter > sample_rate_)
			{
				if (mix > 255) mix = 255;
				if (mix < 0) mix = 0;
				sample = mix;
				sample_counter = 0;
			}

			//sample = state_[0].amp_level;

			if (cf > 255) cf = 255;
			if (cf < 0) cf = 0;

			CFBuffer.Overwrite(cf);
			buffer.Overwrite(sample);
		}

		//state_[0].phase = phase_0;
		//state_[1].phase = phase_1;
		//state_[2].phase = phase_2;
	}
	sample_ = sample;
	sample_counter_ = sample_counter;
}

/* static */
void DrumSynth::UpdateModulations()
{
	for (uint8_t i = 0; i < kNumDrumInstruments; ++i)
	{
		// Step amp envelope.
		state_[i].amp_env_phase += state_[i].amp_env_increment;
		if (state_[i].amp_env_phase < state_[i].amp_env_increment)
		{
			state_[i].amp_env_phase = 0xffff;
			state_[i].amp_env_increment = 0;
		}
		state_[i].amp_level = U8U8MulShift8(state_[i].level, InterpolateSample(wav_res_drm_envelope, state_[i].amp_env_phase));

		// Step pitch envelope.
		state_[i].pitch_env_phase += state_[i].pitch_env_increment;
		if (state_[i].pitch_env_phase < state_[i].pitch_env_increment)
		{
			state_[i].pitch_env_phase = 0xffff;
			state_[i].pitch_env_increment = 0;
		}

		// Compute pitch
		uint16_t pitch = static_cast<uint16_t>(patch_[i].pitch) << 8;
		if (i == 0)
		{
			pitch += U8U8Mul(Random::GetByte(), patch_[i].crunchiness);
		}
		pitch += U8U8Mul(patch_[i].pitch_mod, InterpolateSample(wav_res_drm_envelope, state_[i].pitch_env_phase));

		state_[i].phase_increment = InterpolateIncreasing(lut_res_drm_phase_increments, pitch);

		switch (patch_[i].synth)
		{
		default:
		case SYNTH_BD:
			break;
		case SYNTH_SD:
			state_[i].amp_level_noise = U8U8MulShift8( state_[i].amp_level, patch_[i].crunchiness);
			state_[i].amp_level = U8U8MulShift8( state_[i].amp_level, ~patch_[i].crunchiness);
			break;
		case SYNTH_HH:
			state_[i].phase_increment >>= 6;
			break;
		}
	}
}

/* static */
uint32_t DrumSynth::idle_time_ms()
{
	uint32_t now = milliseconds();
	return now - last_event_time_;
}