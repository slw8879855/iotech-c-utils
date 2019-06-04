//
// Copyright (c) 2018 IOTech
//
// SPDX-License-Identifier: Apache-2.0
//
#ifndef _IOT_OS_H_
#define _IOT_OS_H_

#ifdef __ZEPHYR__
#include "iot/os/zephyr.h"
#else
#include "iot/os/linux.h"
#endif

#define GCC_VERSION (__GNUC__ * 10000       \
                     + __GNUC_MINOR__ * 100 \
                     + __GNUC_PATCHLEVEL__)

#include <pthread.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#if GCC_VERSION >= 40900
#include <stdatomic.h>
#else
#include "iot/os/atomic.h"
#endif
#endif
