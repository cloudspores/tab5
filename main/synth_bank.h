/**
 * @file synth_bank.h
 * @brief The factory voice bank: 32 original six-operator voices in DX7 packed format.
 *
 * The engine only ships one built-in voice, so the synth needs a bank of its own
 * before the user has loaded anything from the SD card. The voices are described
 * in synth_bank.cpp as readable parameter tables and packed into the 4096-byte
 * DX7 bulk format on first use, which is also the format the SD card banks use.
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace synth_bank {

constexpr size_t BANK_BYTES = 4096;   ///< 32 voices x 128 bytes, DX7 bulk voice format

/** The packed factory bank, built on first call and then cached. */
const uint8_t *factory();

}
