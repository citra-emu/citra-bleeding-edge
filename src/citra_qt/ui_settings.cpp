// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "ui_settings.h"

namespace UISettings {

Values values = {};

const std::unordered_set<std::string> allowed_file_extensions = {".3ds", ".3dsx", ".elf",
                                                                 ".axf", ".cci",  ".cxi"};
}
