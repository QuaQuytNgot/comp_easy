#include <stdlib.h>
#include <proto_comp/define.h>
#include <proto_comp/tile_selection.h>
#include <proto_comp/viewport_prediction.h>
// define viewport mxn

RET tile_selection_init(tile_selection_t  *tse,
                        int type)
{
  switch (type)
  {
  case FIXED_TILE_SELECTION:
    tse->select_viewport = define_viewport;
    break;

  default:

    break;
  }
  return RET_SUCCESS;
}

void define_viewport_with_margin(float yaw,
                                 float pitch,
                                 float margin_degrees,  // Extra coverage
                                 int *tile_ids,
                                 int max_tiles,
                                 int *num_tiles)
{
    // Expand viewport by margin
    float expanded_width = VIEWPORT_WIDTH_DEGREES + 2 * margin_degrees;
    float expanded_height = VIEWPORT_HEIGHT_DEGREES + 2 * margin_degrees;
    
    // Use expanded dimensions in calculation...
    // (Same logic as define_viewport but with larger FOV)
}

void define_viewport(float yaw, 
                     float pitch,
                     int *tile_ids,
                     int max_tiles,
                     int *num_tiles)
{
    // Normalize angles
    yaw = wrap_angle_360(yaw);
    pitch = clamp_pitch(pitch);
    
    // Calculate viewport boundaries
    float vp_yaw_min = yaw - (VIEWPORT_WIDTH_DEGREES / 2.0);
    float vp_yaw_max = yaw + (VIEWPORT_WIDTH_DEGREES / 2.0);
    float vp_pitch_min = pitch - (VIEWPORT_HEIGHT_DEGREES / 2.0);
    float vp_pitch_max = pitch + (VIEWPORT_HEIGHT_DEGREES / 2.0);
    
    // Clamp pitch viewport
    if (vp_pitch_min < -90.0) vp_pitch_min = -90.0;
    if (vp_pitch_max > 90.0) vp_pitch_max = 90.0;
    
    int count = 0;
    
    // Iterate through all tiles
    for (int row = 0; row < NO_OF_ROWS; row++) {
        for (int col = 0; col < NO_OF_COLS; col++) {
            // Calculate tile boundaries
            float tile_yaw_min = col * TILE_WIDTH;
            float tile_yaw_max = (col + 1) * TILE_WIDTH;
            float tile_pitch_min = -90.0 + row * TILE_HEIGHT;
            float tile_pitch_max = -90.0 + (row + 1) * TILE_HEIGHT;
            
            // Check pitch intersection (simple)
            int pitch_intersects = intersects(vp_pitch_min, vp_pitch_max,
                                             tile_pitch_min, tile_pitch_max);
            
            // Check yaw intersection (handle 360° wrapping)
            int yaw_intersects = 0;
            
            if (vp_yaw_min < 0.0) {
                // Viewport wraps left (e.g., -30° to 30°)
                float wrapped_min = vp_yaw_min + 360.0;
                yaw_intersects = intersects(wrapped_min, 360.0, 
                                          tile_yaw_min, tile_yaw_max) ||
                                intersects(0.0, vp_yaw_max,
                                          tile_yaw_min, tile_yaw_max);
            } else if (vp_yaw_max > 360.0) {
                // Viewport wraps right (e.g., 330° to 390°)
                float wrapped_max = vp_yaw_max - 360.0;
                yaw_intersects = intersects(vp_yaw_min, 360.0,
                                          tile_yaw_min, tile_yaw_max) ||
                                intersects(0.0, wrapped_max,
                                          tile_yaw_min, tile_yaw_max);
            } else {
                // Normal case
                yaw_intersects = intersects(vp_yaw_min, vp_yaw_max,
                                          tile_yaw_min, tile_yaw_max);
            }
            
            if (pitch_intersects && yaw_intersects) {
                if (count < max_tiles) {
                    tile_ids[count] = row * NO_OF_COLS + col;
                    count++;
                }
            }
        }
    }
    
    *num_tiles = count;
}

int intersects(float a_min, float a_max, float b_min, float b_max)
{
    return !(a_max < b_min || b_max < a_min);
}
// int intersects(float a_min, float a_max, float b_min, float b_max)
// {
//   return a_min < b_max && a_max > b_min;
// }

// void define_viewport(float yaw,
//                      float pitch,
//                      int **tiles,
//                      int  *num_tiles)
// {
//   *num_tiles    = 0;

//   float W_TILE  = W / (float)NO_OF_COLS;
//   float H_TILE  = H / (float)NO_OF_ROWS;

//   float x       = (yaw + 180) / 360 * W;
//   float y       = (90 - pitch) / 180 * H;

//   float delta_x = (FOV_h / 360.0) * W;
//   float delta_y = (FOV_v / 180.0) * H;

//   // bounding box
//   float x_min   = x - delta_x / 2;
//   float x_max   = x + delta_x / 2;
//   float y_min   = y - delta_y / 2;
//   float y_max   = y + delta_y / 2;

//   // Debug information
//   printf("Debug: yaw=%.1f, pitch=%.1f\n", yaw, pitch);
//   printf("Debug: x=%.1f, y=%.1f\n", x, y);
//   printf("Debug: x_min=%.1f, x_max=%.1f, y_min=%.1f, y_max=%.1f\n",
//          x_min,
//          x_max,
//          y_min,
//          y_max);
//   printf("Debug: W_TILE=%.1f, H_TILE=%.1f\n", W_TILE, H_TILE);

//   // wrap-around
//   if (x_min < 0) // x_min < 0
//   {
//     printf("Debug: Wrap-around case 1 (x_min < 0)\n");
//     float part1_x_min = W + x_min;
//     float part1_x_max = W;
//     float part2_x_min = 0;
//     float part2_x_max = x_max;

//     for (COUNT row = 0; row < NO_OF_ROWS; row++)
//     {
//       for (COUNT cols = 0; cols < NO_OF_COLS; cols++)
//       {
//         float tile_x_min = cols * W_TILE;
//         float tile_x_max = (cols + 1) * W_TILE;
//         float tile_y_min = row * H_TILE;
//         float tile_y_max = (row + 1) * H_TILE;

//         if (intersects(
//                 tile_x_min, tile_x_max, part1_x_min, part1_x_max) &&
//             intersects(tile_y_min, tile_y_max, y_min, y_max))
//         {
//           int tile_id              = row * NO_OF_COLS + cols;
//           (*tiles)[(*num_tiles)++] = tile_id;
//         }
//         else if (intersects(tile_x_min,
//                             tile_x_max,
//                             part2_x_min,
//                             part2_x_max) &&
//                  intersects(tile_y_min, tile_y_max, y_min, y_max))
//         {
//           int tile_id              = row * NO_OF_COLS + cols;
//           (*tiles)[(*num_tiles)++] = tile_id;
//         }
//       }
//     }
//   }
//   else if (x_max > W)
//   {
//     printf("Debug: Wrap-around case 2 (x_max > W)\n");
//     float part1_x_min = 0;
//     float part1_x_max = x_max - W;
//     float part2_x_min = x_min;
//     float part2_x_max = W;

//     for (int row = 0; row < NO_OF_ROWS; ++row)
//     {
//       for (int col = 0; col < NO_OF_COLS; ++col)
//       {
//         float tile_x_min = col * W_TILE;
//         float tile_x_max = (col + 1) * W_TILE;
//         float tile_y_min = row * H_TILE;
//         float tile_y_max = (row + 1) * H_TILE;

//         if (intersects(
//                 tile_x_min, tile_x_max, part1_x_min, part1_x_max) &&
//             intersects(tile_y_min, tile_y_max, y_min, y_max))
//         {
//           int tile_id              = row * NO_OF_COLS + col;
//           (*tiles)[(*num_tiles)++] = tile_id;
//         }
//         else if (intersects(tile_x_min,
//                             tile_x_max,
//                             part2_x_min,
//                             part2_x_max) &&
//                  intersects(tile_y_min, tile_y_max, y_min, y_max))
//         {
//           int tile_id              = row * NO_OF_COLS + col;
//           (*tiles)[(*num_tiles)++] = tile_id;
//         }
//       }
//     }
//   }
//   else
//   {
//     printf("Debug: Normal case (no wrap-around)\n");
//     for (int row = 0; row < NO_OF_ROWS; ++row)
//     {
//       for (int col = 0; col < NO_OF_COLS; ++col)
//       {
//         float tile_x_min = col * W_TILE;
//         float tile_x_max = (col + 1) * W_TILE;
//         float tile_y_min = row * H_TILE;
//         float tile_y_max = (row + 1) * H_TILE;

//         if (intersects(tile_x_min, tile_x_max, x_min, x_max) &&
//             intersects(tile_y_min, tile_y_max, y_min, y_max))
//         {
//           int tile_id              = row * NO_OF_COLS + col;
//           (*tiles)[(*num_tiles)++] = tile_id;
//           printf("Debug: Found tile %d at row=%d, col=%d\n",
//                  tile_id,
//                  row,
//                  col);
//         }
//       }
//     }
//   }
// }


