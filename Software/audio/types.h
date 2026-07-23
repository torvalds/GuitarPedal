#ifndef _AUDIO_TYPES_H
#define _AUDIO_TYPES_H

typedef int s32;
typedef unsigned int u32;
typedef long long s64;
typedef unsigned long long u64;

typedef struct {
	s32 left, right;
} raw_sample_t;

typedef struct {
	float left, right;
} sample_t;

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#endif // _AUDIO_TYPES_H
