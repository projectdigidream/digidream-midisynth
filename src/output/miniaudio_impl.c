/* Single-TU miniaudio implementation (playback only; keeps the build lean).
 * Compiled with warnings relaxed -- third-party code, not held to -Werror. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio/miniaudio.h"
