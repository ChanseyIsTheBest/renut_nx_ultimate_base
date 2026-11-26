// ilomilo-nx (2026-11-25, ilomilo_renut_gs_no_clip): point_sprite_gs.hlsl without the clip distance, for the draws
// whose vertex shader writes none (see rectangle_list_gs.hlsl).
#define RENUT_GS_NO_CLIP 1
#include "point_sprite_gs.hlsl"
