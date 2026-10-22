"""Check V6 geometry, thickness clearance, shifted bores, and 3MF readback."""
from pathlib import Path
import json
import numpy as np
import trimesh
import manifold3d as md

ROOT = Path(__file__).resolve().parent.parent
def load(name):
    mesh = trimesh.load(ROOT / name, force="mesh")
    solid = md.Manifold(md.Mesh(np.asarray(mesh.vertices, dtype=np.float32),
                                np.asarray(mesh.faces, dtype=np.uint32)))
    assert solid.status() == md.Error.NoError
    return mesh, solid

def box(origin, size):
    return md.Manifold.cube(size).translate(origin)

mesh, model = load("ESP32_P4_monitor_integrated_clamp_V6.stl")
_, previous = load("ESP32_P4_monitor_integrated_clamp_V5.stl")
_, clamp = load("v6_validation/clamp_only.stl")
_, old_clamp = load("v5_validation/v4_clamp.stl")
_, frame = load("v6_validation/frame_only.stl")
panel = box([0, -22.71, -63.1], [160, 6.8, 59.3])
board = box([0, -10.71, -49.2], [99, 5.21, 45.4])
# 0.001mm inset avoids float32 noise on coincident cavity boundaries.
opening = box([-.499, -23.01, -63.599], [160.998, 10.4, 60.298])
right_wall = box([160.5, -22.91, -63.6], [2.3, 10.2, 60.3])
hole_checks, nut_checks = [], []
for x in [2.5, 94.5]:
    bore = md.Manifold.cylinder(5.2, 1.599, 1.599, 64)
    bore = bore.rotate([-90, 0, 0]).translate([x, -15.81, -45.7])
    hole_checks.append((model ^ bore).volume())
    radius = (5.7 - .002) / (2 * np.cos(np.deg2rad(30)))
    nut = md.Manifold.cylinder(2.498, radius, radius, 6)
    nut = nut.rotate([0, 0, 30]).rotate([-90, 0, 0])
    nut = nut.translate([x, -15.709, -45.7])
    nut_checks.append((model ^ nut).volume())

# With leftward motion, only the lower screw bosses/nuts/bores change.
# The extra frame depth is ahead of V5. Everything outside these two
# explicitly allowed regions must be unchanged (within mesh roundoff).
allowed_changes = box([-3, -23, -66], [167, 6.721, 73])
allowed_changes += box([-1, -15.82, -49.2], [101, 5.22, 7.1])
unintended_change = ((model - previous) - allowed_changes).volume()
unintended_change += ((previous - model) - allowed_changes).volume()

readback, _ = load("v6_validation/read_back_upright.stl")
report = {
    "screen_envelope_mm": [160, 59.3, 6.8],
    "screen_opening_mm": [161, 60.3],
    "panel_pocket_depth_mm": 7.2,
    "frame_total_depth_mm": 10.2,
    "glue_gap_behind_panel_mm": .2,
    "watertight": bool(mesh.is_watertight),
    "consistent_winding": bool(mesh.is_winding_consistent),
    "connected_solids": len(model.decompose()),
    "bounds_mm": mesh.bounds.tolist(),
    "volume_mm3": float(mesh.volume),
    "clamp_v4_minus_v6_mm3": (old_clamp - clamp).volume(),
    "clamp_v6_minus_v4_mm3": (clamp - old_clamp).volume(),
    "panel_interference_mm3": (model ^ panel).volume(),
    "board_interference_mm3": (model ^ board).volume(),
    "mount_hole_obstruction_mm3": hole_checks,
    "nut_pocket_obstruction_mm3": nut_checks,
    "frame_opening_obstruction_mm3": (frame ^ opening).volume(),
    "right_wall_missing_mm3": (right_wall - frame).volume(),
    "unexpected_change_outside_requested_regions_mm3": unintended_change,
    "hole_shift_x_mm_screen_front_view": -1,
    "mount_hole_centers_xz_mm": [[2.5, -45.7], [94.5, -45.7]],
    "mount_hole_pitch_mm": 92,
    "upper_pad_centers_xz_mm": [[3.5, -7.3], [95.5, -7.3]],
    "upright_readback_watertight": bool(readback.is_watertight),
    "upright_bounds_mm": readback.bounds.tolist(),
    "actual_slicer_run": False,
    "physical_fit_verified": False,
}
zero_checks = [report[k] for k in [
    "clamp_v4_minus_v6_mm3", "clamp_v6_minus_v4_mm3",
    "panel_interference_mm3", "board_interference_mm3",
    "frame_opening_obstruction_mm3", "right_wall_missing_mm3",
]] + hole_checks + nut_checks
assert mesh.is_watertight and mesh.is_winding_consistent
assert len(model.decompose()) == 1
assert all(abs(v) < 1e-4 for v in zero_checks), report
assert abs(unintended_change) < .05, report
assert readback.is_watertight and readback.is_winding_consistent
assert np.max(np.abs(readback.bounds[0])) < 1e-6
assert np.allclose(readback.extents, [71.9, 76.91, 165.6], atol=1e-5)
assert abs(mesh.volume - readback.volume) < .02
(ROOT / "v6_validation/geometry_report.json").write_text(
    json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps(report, indent=2))
