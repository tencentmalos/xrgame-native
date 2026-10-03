// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
#include "../../main/cpp/xrgame_profiler.h"
bool xrgameProfileEnabled(bool detail = false) noexcept;
std::string xrgameProfileCommand(const std::vector<std::string>& args);
void xrgameProfileCounter(int id, int64_t value) noexcept;
