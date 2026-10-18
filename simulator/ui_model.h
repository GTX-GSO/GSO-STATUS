#pragma once
#include "gso_snapshot.h"
/* Call on the LVGL thread only. The UI owns a copy used by every view. */
void desk_ui_apply_snapshot(const gso_snapshot *snapshot);
