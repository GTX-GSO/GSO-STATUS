#pragma once
#include <stdbool.h>
/* Spirit level from a three-axis accelerometer at rest. The sensor reads the reaction to gravity, so a display standing
   upright reads about +1 g along its own "up" axis. Axes are the screen's: x to the right, y up, z out of the glass. */
typedef struct {float x,y,z;} gso_accel;
typedef enum {
 GSO_LEVEL_NO_SENSOR,
 GSO_LEVEL_MOVING,  /* the reading is not 1 g: the display is being moved, the last angles are kept */
 GSO_LEVEL_FLAT,    /* glass facing up or down: there is no left/right tilt to report */
 GSO_LEVEL_OK
} gso_level_state;
typedef struct {
 gso_level_state state;
 bool seeded;
 gso_accel filtered;
 float roll_deg;  /* > 0: the right side is lower */
 float tilt_deg;  /* > 0: the top leans back (glass faces upward) */
} gso_level;
void gso_level_reset(gso_level *l);
void gso_level_no_sensor(gso_level *l);
void gso_level_update(gso_level *l,gso_accel sample);
/* Display bands for the roll angle. */
typedef enum {GSO_LEVEL_FLATNESS_LEVEL,GSO_LEVEL_FLATNESS_NEAR,GSO_LEVEL_FLATNESS_OFF} gso_level_band;
gso_level_band gso_level_band_of(float roll_deg);
