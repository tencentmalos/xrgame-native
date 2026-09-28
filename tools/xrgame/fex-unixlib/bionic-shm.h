// SPDX-License-Identifier: MIT
#pragma once
#include <sys/types.h>
#include <sys/stat.h>
extern "C" int xrgame_shm_open(const char* name, int flags, mode_t mode);
extern "C" int xrgame_shm_unlink(const char* name);
