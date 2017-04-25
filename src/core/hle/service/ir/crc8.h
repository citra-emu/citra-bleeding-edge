// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include "common/common_types.h"

namespace Service {
namespace IR {

/**
 * Calculates CRC-8-CCITT for the given data.
 * @param data The pointer to the begining of the data.
 * @param size Size of the data.
 * @returns the CRC value of the data.
 */
u8 Crc8(const u8* data, size_t size);

} // namespace IR
} // namespace Service
