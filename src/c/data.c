//
// Copyright (c) 2019 IOTech
//
// SPDX-License-Identifier: Apache-2.0
//
#include "iot/os.h"
#include "iot/data.h"
#include "iot/json.h"
#include "iot/base64.h"

#define IOT_DATA_BLOCK_SIZE 64
#define IOT_JSON_BUFF_SIZE 512

static const char * iot_data_type_names [] = {"Int8","UInt8","Int16","UInt16","Int32","UInt32","Int64","UInt64","Float32","Float64","Bool","String","BLOB","Map","Array"};

typedef union iot_data_union_t
{
  int8_t i8;
  uint8_t ui8;
  int16_t i16;
  uint16_t ui16;
  int32_t i32;
  uint32_t ui32;
  int64_t i64;
  uint64_t ui64;
  float f32;
  double f64;
  bool bl;
  char * str;
} iot_data_union_t;

struct iot_data_t
{
  iot_data_t * next;
  atomic_uint_fast32_t refs;
  iot_data_type_t type;
  bool release;
};

typedef struct iot_data_value_t
{
  iot_data_t base;
  iot_data_union_t value;
} iot_data_value_t;

typedef struct iot_data_blob_t
{
  iot_data_t base;
  uint8_t * data;
  uint32_t size;
} iot_data_blob_t;

typedef struct iot_data_array_t
{
  iot_data_t base;
  uint32_t size;
  iot_data_t ** values;
} iot_data_array_t;

typedef struct iot_data_pair_t
{
  iot_data_t base;
  iot_data_t * key;
  iot_data_t * value;
} iot_data_pair_t;

typedef struct iot_data_map_t
{
  iot_data_t base;
  iot_data_type_t key_type;
  iot_data_pair_t * head;
  iot_data_pair_t * tail;
} iot_data_map_t;

typedef struct iot_string_holder_t
{
  char * str;
  size_t size;
  size_t free;
} iot_string_holder_t;

extern void iot_data_init (void);
extern void iot_data_fini (void);

// Data cache and guard mutex or spin lock

static iot_data_t * iot_data_cache = NULL;

#if defined (_GNU_SOURCE) || defined (__LIBMUSL__)
#define IOT_HAS_SPINLOCK
#endif

#ifdef IOT_HAS_SPINLOCK
static pthread_spinlock_t iot_data_slock;
#else
static pthread_mutex_t iot_data_mutex;
#endif

static iot_data_t * iot_data_all_from_json (iot_json_tok_t ** tokens, const char * json);

static void * iot_data_factory_alloc (void)
{
#ifdef IOT_HAS_SPINLOCK
  pthread_spin_lock (&iot_data_slock);
#else
  pthread_mutex_lock (&iot_data_mutex);
#endif
  iot_data_t * data = iot_data_cache;
  if (data)
  {
    iot_data_cache = data->next;
  }
#ifdef IOT_HAS_SPINLOCK
  pthread_spin_unlock (&iot_data_slock);
#else
  pthread_mutex_unlock (&iot_data_mutex);
#endif
  data = (data) ? memset (data, 0, IOT_DATA_BLOCK_SIZE) : calloc (1, IOT_DATA_BLOCK_SIZE);
  atomic_store (&data->refs, 1);
  return data;
}

static inline void iot_data_factory_free (iot_data_t * data)
{
#ifdef IOT_HAS_SPINLOCK
  pthread_spin_lock (&iot_data_slock);
#else
  pthread_mutex_lock (&iot_data_mutex);
#endif
  data->next = iot_data_cache;
  iot_data_cache = data;
#ifdef IOT_HAS_SPINLOCK
  pthread_spin_unlock (&iot_data_slock);
#else
  pthread_mutex_unlock (&iot_data_mutex);
#endif
}

static inline iot_data_value_t * iot_data_value_alloc (iot_data_type_t type, bool copy)
{
  iot_data_value_t * val = iot_data_factory_alloc ();
  val->base.type = type;
  val->base.release = copy;
  return val;
}

static bool iot_data_equal (const iot_data_t * v1, const iot_data_t * v2)
{
  assert (v1 && v2);
  if (v1 == v2) return true;
  if (v1->type == v2->type)
  {
    switch (v1->type)
    {
      case IOT_DATA_STRING: return (strcmp (((iot_data_value_t*) v1)->value.str, ((iot_data_value_t*) v2)->value.str) == 0);
      case IOT_DATA_BLOB:
      {
        iot_data_blob_t * b1 = (iot_data_blob_t*) v1;
        iot_data_blob_t * b2 = (iot_data_blob_t*) v2;
        return (b1->size == b2->size && (memcmp (b1->data, b2->data, b1->size) == 0));
      }
      default: return (((iot_data_value_t*) v1)->value.ui64 == ((iot_data_value_t*) v2)->value.ui64);
    }
  }
  return false;
}

void iot_data_init (void)
{
  _Static_assert (sizeof (iot_data_value_t) < IOT_DATA_BLOCK_SIZE, "IOT_DATA_BLOCK_SIZE too small");
  _Static_assert (sizeof (iot_data_map_t) < IOT_DATA_BLOCK_SIZE, "IOT_DATA_BLOCK_SIZE too small");
  _Static_assert (sizeof (iot_data_array_t) < IOT_DATA_BLOCK_SIZE, "IOT_DATA_BLOCK_SIZE too small");
  _Static_assert (sizeof (iot_data_blob_t) < IOT_DATA_BLOCK_SIZE, "IOT_DATA_BLOCK_SIZE too small");
  _Static_assert (sizeof (iot_data_pair_t) < IOT_DATA_BLOCK_SIZE, "IOT_DATA_BLOCK_SIZE too small");
#ifdef IOT_HAS_SPINLOCK
  pthread_spin_init (&iot_data_slock, 0);
#else
  pthread_mutex_init (&iot_data_mutex, NULL);
#endif
}

void iot_data_fini (void)
{
  while (iot_data_cache)
  {
    iot_data_t * data = iot_data_cache;
    iot_data_cache = data->next;
    free (data);
  }
#ifdef IOT_HAS_SPINLOCK
  pthread_spin_destroy (&iot_data_slock);
#else
  pthread_mutex_destroy (&iot_data_mutex);
#endif
}

void iot_data_add_ref (iot_data_t * data)
{
  assert (data);
  atomic_fetch_add (&data->refs, 1);
}

iot_data_type_t iot_data_name_type (const char * name)
{
  int type = 0;
  while (type >= 0)
  {
    if (strcasecmp (name, iot_data_type_names[type]) == 0) break;
    type = (type == IOT_DATA_ARRAY) ? -1 : (type + 1);
  }
  return type;
}

const char * iot_data_type_name (const iot_data_t * data)
{
  assert (data && (data->type <= IOT_DATA_ARRAY));
  return iot_data_type_names[data->type];
}

iot_data_t * iot_data_alloc_map (iot_data_type_t key_type)
{
  assert (key_type < IOT_DATA_MAP);
  iot_data_map_t * map = iot_data_factory_alloc ();
  map->base.type = IOT_DATA_MAP;
  map->key_type = key_type;
  return (iot_data_t*) map;
}

iot_data_t * iot_data_alloc_array (uint32_t size)
{
  assert (size);
  iot_data_array_t * array = iot_data_factory_alloc ();
  array->base.type = IOT_DATA_ARRAY;
  array->size = size;
  array->values = calloc (size, sizeof (iot_data_t*));
  return (iot_data_t*) array;
}

iot_data_type_t iot_data_type (const iot_data_t * data)
{
  assert (data);
  return data->type;
}

void * iot_data_address (const iot_data_t * data)
{
  return (data && data->type <= IOT_DATA_BLOB) ? ((data->type == IOT_DATA_BLOB) ? (void*)((iot_data_blob_t*) data)->data : (void*)&(((iot_data_value_t*) data)->value)) : NULL;
}

void iot_data_free (iot_data_t * data)
{
  if (data && (atomic_fetch_add (&data->refs, -1) <= 1))
  {
    switch (data->type)
    {
      case IOT_DATA_STRING:
        if (data->release) free (((iot_data_value_t*) data)->value.str);
        break;
      case IOT_DATA_BLOB:
      {
        iot_data_blob_t * blob = (iot_data_blob_t*) data;
        if (blob->base.release) free (blob->data);
        break;
      }
      case IOT_DATA_MAP:
      {
        iot_data_map_t * map = (iot_data_map_t*) data;
        iot_data_pair_t * pair;
        while ((pair = map->head))
        {
          iot_data_free (pair->key);
          iot_data_free (pair->value);
          map->head = (iot_data_pair_t *) pair->base.next;
          iot_data_factory_free (&pair->base);
        }
        break;
      }
      case IOT_DATA_ARRAY:
      {
        iot_data_array_t * array = (iot_data_array_t*) data;
        for (uint32_t i = 0; i < array->size; i++)
        {
          iot_data_free (array->values[i]);
        }
        free (array->values);
        break;
      }
      default: break;
    }
    iot_data_factory_free (data);
  }
}

iot_data_t * iot_data_alloc_from_string (iot_data_type_t type, const char * value)
{
  assert (value && strlen (value));
  switch (type)
  {
    case IOT_DATA_INT8: return iot_data_alloc_i8 ((int8_t) atoi (value));
    case IOT_DATA_UINT8:return iot_data_alloc_ui8 ((uint8_t) atoi (value));
    case IOT_DATA_INT16: return iot_data_alloc_i16 ((int16_t) atoi (value));
    case IOT_DATA_UINT16:return iot_data_alloc_ui16 ((uint16_t) atoi (value));
    case IOT_DATA_INT32: return iot_data_alloc_i32 ((int32_t) atol (value));
    case IOT_DATA_UINT32:return iot_data_alloc_ui32 ((uint32_t) atol (value));
    case IOT_DATA_INT64: return iot_data_alloc_i64 ((int64_t) atoll (value));
    case IOT_DATA_UINT64:return iot_data_alloc_ui64 ((uint64_t) atoll (value));
    case IOT_DATA_FLOAT32: return iot_data_alloc_f32 ((float) atof (value));
    case IOT_DATA_FLOAT64: return iot_data_alloc_f64 (atof (value));
    case IOT_DATA_BOOL: return iot_data_alloc_bool (value[0] == 't' || value[0] == 'T');
    case IOT_DATA_STRING: return iot_data_alloc_string (value, IOT_DATA_COPY);
    default: break;
  }
  return NULL;
}

iot_data_t * iot_data_alloc_from_strings (const char * type, const char * value)
{
  return iot_data_alloc_from_string (iot_data_name_type (type), value);
}

iot_data_t * iot_data_alloc_i8 (int8_t val)
{
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_INT8, false);
  data->value.i8 = val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_ui8 (uint8_t val)
{
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_UINT8, false);
  data->value.ui8 = val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_i16 (int16_t val)
{
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_INT16, false);
  data->value.i16 = val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_ui16 (uint16_t val)
{
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_UINT16, false);
  data->value.ui16 = val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_i32 (int32_t val)
{
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_INT32, false);
  data->value.i32 = val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_ui32 (uint32_t val)
{
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_UINT32, false);
  data->value.ui32 = val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_i64 (int64_t val)
{
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_INT64, false);
  data->value.i64 = val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_ui64 (uint64_t val)
{
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_UINT64, false);
  data->value.ui64 = val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_f32 (float val)
{
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_FLOAT32, false);
  data->value.f32 = val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_f64 (double val)
{
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_FLOAT64, false);
  data->value.f64 = val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_bool (bool val)
{
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_BOOL, false);
  data->value.bl = val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_string (const char * val, iot_data_ownership_t ownership)
{
  assert (val);
  iot_data_value_t * data = iot_data_value_alloc (IOT_DATA_STRING, ownership != IOT_DATA_REF);
  data->value.str = (ownership == IOT_DATA_COPY) ? iot_strdup (val) : (char*) val;
  return (iot_data_t*) data;
}

iot_data_t * iot_data_alloc_blob (uint8_t * data, uint32_t size, iot_data_ownership_t ownership)
{
  assert (data && size);
  iot_data_blob_t * blob = iot_data_factory_alloc ();
  blob->base.type = IOT_DATA_BLOB;
  blob->size = size;
  blob->base.release = ownership != IOT_DATA_REF;
  if (ownership == IOT_DATA_COPY)
  {
    blob->data = malloc (size);
    memcpy (blob->data, data, size);
  }
  else
  {
    blob->data = data;
  }
  return (iot_data_t*) blob;
}

iot_data_t * iot_data_alloc_blob_from_base64 (const char * value)
{
  size_t len;
  uint8_t * data;
  iot_data_t * result = NULL;
  assert (value);
  len = iot_b64_maxdecodesize (value);
  data = malloc (len);
  assert (data);

  if (iot_b64_decode (value, data, &len))
  {
    result = iot_data_alloc_blob (data, len, IOT_DATA_TAKE);
  }
  else
  {
    free (data);
  }

  return result;
}

int8_t iot_data_i8 (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_INT8));
  return ((iot_data_value_t*) data)->value.i8;
}

uint8_t iot_data_ui8 (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_UINT8));
  return ((iot_data_value_t*) data)->value.ui8;
}

int16_t iot_data_i16 (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_INT16));
  return ((iot_data_value_t*) data)->value.i16;
}

uint16_t iot_data_ui16 (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_UINT16));
  return ((iot_data_value_t*) data)->value.ui16;
}

int32_t iot_data_i32 (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_INT32));
  return ((iot_data_value_t*) data)->value.i32;
}

uint32_t iot_data_ui32 (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_UINT32));
  return ((iot_data_value_t*) data)->value.ui32;
}

int64_t iot_data_i64 (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_INT64));
  return ((iot_data_value_t*) data)->value.i64;
}

uint64_t iot_data_ui64 (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_UINT64));
  return ((iot_data_value_t*) data)->value.ui64;
}

float iot_data_f32 (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_FLOAT32));
  return ((iot_data_value_t*) data)->value.f32;
}

double iot_data_f64 (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_FLOAT64));
  return ((iot_data_value_t*) data)->value.f64;
}

bool iot_data_bool (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_BOOL));
  return ((iot_data_value_t*) data)->value.bl;
}

const char * iot_data_string (const iot_data_t * data)
{
  assert (data && (data->type == IOT_DATA_STRING));
  return ((iot_data_value_t*) data)->value.str;
}

const uint8_t * iot_data_blob (const iot_data_t * data, uint32_t * size)
{
  assert (data && (data->type == IOT_DATA_BLOB));
  if (size) *size =  ((iot_data_blob_t*) data)->size;
  return ((iot_data_blob_t*) data)->data;
}

static iot_data_pair_t * iot_data_map_find (iot_data_map_t * map, const iot_data_t * key)
{
  iot_data_pair_t * pair =  map->head;
  while (pair)
  {
    if (iot_data_equal (pair->key, key))
    {
      break;
    }
    pair = (iot_data_pair_t*) pair->base.next;
  }
  return pair;
}

void iot_data_string_map_add (iot_data_t * map, const char * key, iot_data_t * val)
{
  assert (key);
  iot_data_map_add (map, iot_data_alloc_string (key, IOT_DATA_REF), val);
}

void iot_data_map_add (iot_data_t * map, iot_data_t * key, iot_data_t * val)
{
  iot_data_map_t * mp = (iot_data_map_t*) map;

  assert (mp && (mp->base.type == IOT_DATA_MAP));
  assert (key && key->type == mp->key_type);

  iot_data_pair_t * pair = iot_data_map_find (mp, key);
  if (pair)
  {
    iot_data_free (pair->value);
    iot_data_free (pair->key);
  }
  else
  {
    pair = (iot_data_pair_t*) iot_data_factory_alloc ();
    if (mp->tail) mp->tail->base.next = &pair->base;
    mp->tail = pair;
    if (mp->head == NULL) mp->head = pair;
  }
  pair->value = val;
  pair->key = key;
}

bool iot_data_map_base64_to_blob (iot_data_t * map, const iot_data_t * key)
{
  bool result = false;
  iot_data_map_t * mp = (iot_data_map_t*) map;

  assert (mp && (mp->base.type == IOT_DATA_MAP));
  assert (key && key->type == mp->key_type);

  iot_data_pair_t * pair = iot_data_map_find (mp, key);
  if (pair && (pair->value->type == IOT_DATA_STRING))
  {
    const char * str = ((iot_data_value_t*) pair->value)->value.str;
    iot_data_t * blob = iot_data_alloc_blob_from_base64 (str);

    result = (blob != NULL);

    if (result)
    {
      iot_data_free (pair->value);
      pair->value = blob;
    }
  }

  return result;
}

const iot_data_t * iot_data_map_get (const iot_data_t * map, const iot_data_t * key)
{
  iot_data_map_t * mp = (iot_data_map_t*) map;
  assert (mp && key && (mp->base.type == IOT_DATA_MAP));
  iot_data_pair_t * pair = iot_data_map_find (mp, key);
  return pair ? pair->value : NULL;
}

const iot_data_t * iot_data_string_map_get (const iot_data_t * map, const char * key)
{
  assert (map && key);
  iot_data_t * dkey = iot_data_alloc_string (key, IOT_DATA_REF);
  const iot_data_t * value = iot_data_map_get (map, dkey);
  iot_data_free (dkey);
  return value;
}

const char * iot_data_string_map_get_string (const iot_data_t * map, const char * key)
{
  const iot_data_t * data = iot_data_string_map_get (map, key);
  return data ? iot_data_string (data) : NULL;
}

iot_data_type_t iot_data_map_key_type (const iot_data_t * map)
{
  assert (map);
  return ((iot_data_map_t*) map)->key_type;
}

void iot_data_array_add (iot_data_t * array, uint32_t index, iot_data_t * val)
{
  iot_data_array_t * arr = (iot_data_array_t*) array;
  assert (array && (array->type == IOT_DATA_ARRAY));
  assert (val);
  assert (index < arr->size);
  iot_data_t * element = arr->values[index];
  iot_data_free (element);
  arr->values[index] = val;

}

const iot_data_t * iot_data_array_get (const iot_data_t * array, uint32_t index)
{
  iot_data_array_t * arr = (iot_data_array_t*) array;
  assert (array && (array->type == IOT_DATA_ARRAY));
  assert (index < arr->size);
  return arr->values[index];
}

uint32_t iot_data_array_size (const iot_data_t * array)
{
  assert (array && (array->type == IOT_DATA_ARRAY));
  return ((iot_data_array_t*) array)->size;
}

void iot_data_map_iter (const iot_data_t * map, iot_data_map_iter_t * iter)
{
  assert (iter);
  assert (map && map->type == IOT_DATA_MAP);
  iter->pair = NULL;
  iter->map = (iot_data_map_t*) map;
}

bool iot_data_map_iter_next (iot_data_map_iter_t * iter)
{
  iter->pair = iter->pair ? (iot_data_pair_t*) iter->pair->base.next : iter->map->head;
  return (iter->pair != NULL);
}

const iot_data_t * iot_data_map_iter_key (const iot_data_map_iter_t * iter)
{
  return (iter->pair) ? iter->pair->key : NULL;
}

const iot_data_t * iot_data_map_iter_value (const iot_data_map_iter_t * iter)
{
  return (iter->pair) ? iter->pair->value : NULL;
}

const char * iot_data_map_iter_string_key (const iot_data_map_iter_t * iter)
{
  return iot_data_string (iot_data_map_iter_key (iter));
}

const char * iot_data_map_iter_string_value (const iot_data_map_iter_t * iter)
{
  return iot_data_string (iot_data_map_iter_value (iter));
}

void iot_data_array_iter (const iot_data_t * array, iot_data_array_iter_t * iter)
{
  assert (iter);
  assert (array && array->type == IOT_DATA_ARRAY);
  iter->array = (iot_data_array_t*) array;
  iter->index = 0;
}

bool iot_data_array_iter_next (iot_data_array_iter_t * iter)
{
  iter->index++;
  return (iter->index <= iter->array->size);
}

uint32_t iot_data_array_iter_index (const iot_data_array_iter_t * iter)
{
  return (iter->index - 1);
}

const iot_data_t * iot_data_array_iter_value (const iot_data_array_iter_t * iter)
{
  return (iter->index <= iter->array->size) ? iter->array->values[iter->index - 1] : NULL;
}

const char * iot_data_array_iter_string (const iot_data_array_iter_t * iter)
{
  return (iter->index <= iter->array->size) ? iot_data_string (iter->array->values[iter->index - 1]) : NULL;
}

static size_t iot_data_repr_size (char c)
{
  if (strchr ("\"\\\b\f\n\r\t", c)) return 2;
  if (c >= '\x00' && c <=  '\x1f') return 6;
  return 1;
}

static void iot_data_strcat_escape (iot_string_holder_t * holder, const char * add, bool escape)
{
  size_t len = strlen (add);
  size_t adj_len = len;
  size_t i;
  if (escape)
  {
    adj_len = 0;
    for (i = 0; i < len; i++)
    {
      adj_len += iot_data_repr_size (add[i]);
    }
  }
  if (holder->free < adj_len)
  {
    holder->size += adj_len;
    holder->free += adj_len;
    holder->str = realloc (holder->str, holder->size);
  }
  if (len == adj_len)
  {
    strcat (holder->str, add);
  }
  else
  {
    static const char * hex = "0123456789abcdef";
    assert (strlen (holder->str) == (holder->size - holder->free - 1));
    char * ptr = holder->str + holder->size - holder->free - 1;
    for (i = 0; i < len; i++)
    {
      char c = add[i];
      switch (iot_data_repr_size (c))
      {
        case 1:
        {
          *ptr++ = c;
          break;
        }
        case 2:
        {
          *ptr++ = '\\';
          switch (c)
          {
            case '\"': *ptr++ = '\"'; break;
            case '\\': *ptr++ = '\\'; break;
            case '\b': *ptr++ = 'b'; break;
            case '\f': *ptr++ = 'f'; break;
            case '\n': *ptr++ = 'n'; break;
            case '\r': *ptr++ = 'r'; break;
            case '\t': *ptr++ = 't'; break;
          }
          break;
        }
        case 6:
        {
          *ptr++ = '\\';
          *ptr++ = 'u';
          *ptr++ = '0';
          *ptr++ = '0';
          *ptr++ = (c & 0x10) ? '1' : '0';
          *ptr++ = hex[c & 0xf];
          break;
        }
      }
    }
    *ptr = '\0';
  }
  holder->free -= adj_len;
}

static void iot_data_strcat (iot_string_holder_t * holder, const char * add)
{
  iot_data_strcat_escape (holder, add, true);
}

static void iot_data_add_quote (iot_string_holder_t * holder)
{
  iot_data_strcat_escape (holder, "\"", false);
}

static void iot_data_base64_encode (iot_string_holder_t * holder, const iot_data_t * blob)
{
  uint32_t inLen;
  const uint8_t * data = iot_data_blob (blob, &inLen);
  assert (strlen (holder->str) == (holder->size - holder->free - 1));
  char * out = holder->str + holder->size - holder->free - 1;
  size_t len = iot_b64_encodesize (inLen) - 1; /* Allow for string terminator */

  if (holder->free < len)
  {
    holder->size += len;
    holder->free += len;
    holder->str = realloc (holder->str, holder->size);
  }

  iot_b64_encode (data, inLen, out, holder->free);
  holder->free -= len;
}

static void iot_data_dump_raw (iot_string_holder_t * holder, const iot_data_t * data, bool wrap)
{
  char buff [128];
  wrap = wrap || data->type == IOT_DATA_BOOL;

  switch (data->type)
  {
    case IOT_DATA_INT8: sprintf (buff, "%" PRId8 , iot_data_i8 (data)); break;
    case IOT_DATA_UINT8: sprintf (buff, "%" PRIu8, iot_data_ui8 (data)); break;
    case IOT_DATA_INT16: sprintf (buff, "%" PRId16, iot_data_i16 (data)); break;
    case IOT_DATA_UINT16: sprintf (buff, "%" PRIu16, iot_data_ui16 (data)); break;
    case IOT_DATA_INT32: sprintf (buff, "%" PRId32, iot_data_i32 (data)); break;
    case IOT_DATA_UINT32: sprintf (buff, "%" PRIu32, iot_data_ui32 (data)); break;
    case IOT_DATA_INT64: sprintf (buff, "%" PRId64, iot_data_i64 (data)); break;
    case IOT_DATA_UINT64: sprintf (buff, "%" PRIu64, iot_data_ui64 (data)); break;
    case IOT_DATA_FLOAT32: sprintf (buff, "%f", iot_data_f32 (data)); break;
    case IOT_DATA_FLOAT64: sprintf (buff, "%lf", iot_data_f64 (data)); break;
    default: strcpy (buff, iot_data_bool (data) ? "true" : "false"); break;
  }
  if (wrap)
  {
    iot_data_add_quote (holder);
    strcat (buff, "\"");
  }
  iot_data_strcat_escape (holder, buff, false);
}

static void iot_data_dump (iot_string_holder_t * holder, const iot_data_t * data, bool wrap)
{
  switch (data->type)
  {
    case IOT_DATA_STRING:
    {
      iot_data_add_quote (holder);
      iot_data_strcat (holder, iot_data_string (data));
      iot_data_add_quote (holder);
      break;
    }
    case IOT_DATA_BLOB:
    {
      iot_data_add_quote (holder);
      iot_data_base64_encode (holder, data);
      iot_data_add_quote (holder);
      break;
    }
    case IOT_DATA_MAP:
    {
      iot_data_map_iter_t iter;
      iot_data_map_iter (data, &iter);
      iot_data_strcat (holder, "{");
      while (iot_data_map_iter_next (&iter))
      {
        const iot_data_t * key = iot_data_map_iter_key (&iter);
        const iot_data_t * value = iot_data_map_iter_value (&iter);
        iot_data_dump (holder, key, true);
        iot_data_strcat (holder, ":");
        iot_data_dump (holder, value, wrap);
        if (iter.pair->base.next)
        {
          iot_data_strcat (holder, ",");
        }
      }
      iot_data_strcat (holder, "}");
      break;
    }
    case IOT_DATA_ARRAY:
    {
      iot_data_array_iter_t iter;
      iot_data_array_iter (data, &iter);
      iot_data_strcat (holder, "[");
      while (iot_data_array_iter_next (&iter))
      {
        const iot_data_t * value = iot_data_array_iter_value (&iter);
        iot_data_dump (holder, value, wrap);
        if (iter.index < iter.array->size)
        {
          iot_data_strcat (holder, ",");
        }
      }
      iot_data_strcat (holder, "]");
      break;
    }
    default: iot_data_dump_raw (holder, data, wrap);
  }
}

char * iot_data_to_json (const iot_data_t * data, bool wrap)
{
  iot_string_holder_t holder;
  assert (data);
  holder.str = calloc (1, IOT_JSON_BUFF_SIZE);
  holder.size = IOT_JSON_BUFF_SIZE;
  holder.free = IOT_JSON_BUFF_SIZE - 1; // Allowing for string terminator
  iot_data_dump (&holder, data, wrap);
  return holder.str;
}

static char * iot_data_string_from_json_token (const char * json, iot_json_tok_t * token)
{
  size_t len = (size_t) (token->end - token->start);
  char * str = malloc (len + 1);
  memcpy (str, json + token->start, len);
  str[len] = 0;
  return str;
}

static iot_data_t * iot_data_string_from_json (iot_json_tok_t ** tokens, const char * json)
{
  char * str = iot_data_string_from_json_token (json, *tokens);
  (*tokens)++;
  return iot_data_alloc_string (str, IOT_DATA_TAKE);
}

static iot_data_t * iot_data_primitive_from_json (iot_json_tok_t ** tokens, const char * json)
{
  iot_data_t * ret;
  char * str = iot_data_string_from_json_token (json, *tokens);
  (*tokens)++;
  switch (str[0])
  {
    case 't': case 'f': ret = iot_data_alloc_bool (str[0] == 't'); break; // true/false
    case 'n': ret = iot_data_alloc_string ("null", IOT_DATA_REF); break; // null
    default: // Handle all floating point numbers as doubles and integers as uint64_t
      ret = (strchr (str, '.') || strchr (str, 'e') || strchr (str, 'E')) ?
        iot_data_alloc_f64 (strtod (str, NULL)) : iot_data_alloc_i64 (strtol (str, NULL, 0));
      break;
  }
  free (str);
  return ret;
}

static iot_data_t * iot_data_map_from_json (iot_json_tok_t ** tokens, const char * json)
{
  uint32_t elements = (*tokens)->size;
  iot_data_t * map = iot_data_alloc_map (IOT_DATA_STRING);

  (*tokens)++;
  while  (elements--)
  {
    iot_data_t * key = iot_data_string_from_json (tokens, json);
    iot_data_map_add (map, key, iot_data_all_from_json (tokens, json));
  }
  return map;
}

static iot_data_t * iot_data_array_from_json (iot_json_tok_t ** tokens, const char * json)
{
  uint32_t elements = (*tokens)->size;
  uint32_t index = 0;
  iot_data_t * array = iot_data_alloc_array (elements);

  (*tokens)++;
  while (elements--)
  {
    iot_data_array_add (array, index++, iot_data_all_from_json (tokens, json));
  }
  return array;
}

static iot_data_t * iot_data_all_from_json (iot_json_tok_t ** tokens, const char * json)
{
  iot_data_t * data = NULL;
  switch ((*tokens)->type)
  {
    case IOT_JSON_PRIMITIVE: data = iot_data_primitive_from_json (tokens, json); break;
    case IOT_JSON_OBJECT: data = iot_data_map_from_json (tokens, json); break;
    case IOT_JSON_ARRAY: data = iot_data_array_from_json (tokens, json); break;
    default: data = iot_data_string_from_json (tokens, json); break;
  }
  return data;
}

iot_data_t * iot_data_from_json (const char * json)
{
  iot_json_parser parser;
  iot_data_t * data = NULL;
  int32_t used;
  const char * ptr = json;
  uint32_t count = 1;

  // Approximate token count
  while (*ptr != '\0')
  {
    switch (*ptr)
    {
      case ',': case '{': count++; break;
      case ':': case '[': count += 2; break;
      default: break;
    }
    ptr++;
  }
  iot_json_tok_t * tokens = calloc (1, sizeof (*tokens) * count);
  iot_json_tok_t * tptr = tokens;

  iot_json_init (&parser);
  used = iot_json_parse (&parser, json, strlen (json), tptr, count);
  if (used)
  {
    assert (used <= count);
    data = iot_data_all_from_json (&tptr, json);
  }
  free (tokens);
  return data;
}
