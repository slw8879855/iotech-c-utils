//
// Copyright (c) 2018 IOTech
//
// SPDX-License-Identifier: Apache-2.0
//
#include "iot/os.h"

static pthread_mutex_t atomic_mutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t atomic_load (uint32_t * av)
{
  uint32_t result;
  pthread_mutex_lock (&atomic_mutex);
  result = *av;
  pthread_mutex_unlock (&atomic_mutex);
  return result;
}

void atomic_store (uint32_t * av, uint32_t v)
{
  pthread_mutex_lock (&atomic_mutex);
  *av = v;
  pthread_mutex_unlock (&atomic_mutex);
}

uint32_t atomic_fetch_add (uint32_t * av, int arg)
{
  uint32_t result;
  pthread_mutex_lock (&atomic_mutex);
  result = *av;
  *av += arg;
  pthread_mutex_unlock (&atomic_mutex);
  return result;
}
