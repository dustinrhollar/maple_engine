\
//-------------------------------------------------------------------------------------------------------------------//
// HEADERS
//-------------------------------------------------------------------------------------------------------------------//

//~ C Headers

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <math.h.>

//~ Type System

#include "utils/maple_types.h"
#include "../graphics/vulkan/vulkan_core.h"
#include "platform/globals.h"

#include "platform/platform.h"

//~ Memory Management

#include "mm/memory.h"

//~ Util stuff

#define STB_DS_IMPLEMENTATION
#include "utils/stb_ds.h"

#define USE_MAPLE_MSTR_IMPLEMENTATION
#include "utils/mstr.h"

#define MAPLE_VECTOR_MATH_IMPLEMENTATION
#include "utils/vector_math.h"

#define MAPLE_CAMERA_IMPLEMENTATION
#include "utils/camera.h"

#define MAPLE_HASH_FUNCTION_IMPLEMENTATION
#include "utils/hash_functions.h"

//~ Kinda anything else

#include "platform/win32/asset_sys.h"

#include "frame_params/frame_params.h"

//~ Function pointers for Dlls and their globals

#include "../graphics/maple_graphics.h"

#include "../game/voxel_pfn.h"
#include "platform/library_loader.h"

//~ World Creator thingy

#include "world/polygonal_world.h"

//-------------------------------------------------------------------------------------------------------------------//
// SOURCE
//-------------------------------------------------------------------------------------------------------------------//

#include "mm/memory.c"
#include "world/polygonal_world.c"
#include "platform/globals.c"
#include "platform/platform_entry.c"