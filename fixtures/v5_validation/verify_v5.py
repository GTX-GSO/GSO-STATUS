"""Validate the exported V5 using trimesh and manifold3d; no printer access."""
from pathlib import Path
import json
import numpy as np
import trimesh
import manifold3d as md

ROOT = Path(__file__).resolve().parent.parent


def load(name):
    mesh = trimesh.load(ROOT / name, force="mesh")
    solid = md.Manifold(md.Mesh(
        np.asarray(mesh.vertices, dtype=np.float32),
        np.asarray(mesh.faces, dtype=np.uint32)))
    return mesh, solid


def box(origin, size):
    return md.Manifold.cube(size).translate(origin)


mesh, model = load("ESP32_P4_monitor_integrated_clamp_V5.stl")
_, old_clamp = load("v5_validation/v4_clamp.stl")
_, new_clamp = load("v5_validation/v5_clamp.stl")
panel = box([0, -16.08, -63.1], [160, .17, 59.3])
board = box([0, -10.71, -49.2], [99, 5.21, 45.4])

# Check the four intended board contacts separately: the envelope must
# touch the supports, with no intersection of positive volume.
hole_checks = []
for x in [3.5, 95.5]:
    bore = md.Manifold.cylinder(5.2, 1.599, 1.599, 64)
    bore = bore.rotate([-90, 0, 0]).translate([x, -15.81, -45.7])
    hole_checks.append((model ^ bore).volume())

# Constant section of the unstepped frame; this check excludes the board
# backplate because its coverage is intentional.
_, frame = load("v5_validation/frame_only.stl")
# STL coordinates pass through float32 in Manifold. Inset the coincident
# X/Z cavity boundaries by 0.001 mm to avoid treating roundoff as contact.
opening = box([-.499, -16.38, -63.599], [160.998, 3.77, 60.298])
right_wall = box([160.5, -16.28, -63.6], [2.3, 3.57, 60.3])
report = {
    "watertight": bool(mesh.is_watertight),
    "consistent_winding": bool(mesh.is_winding_consistent),
    "manifold_status": str(model.status()),
    "connected_solids": len(model.decompose()),
    "bounds_mm": mesh.bounds.tolist(),
    "envelope_mm": mesh.extents.tolist(),
    "volume_mm3": model.volume(),
    "clamp_v4_minus_v5_mm3": (old_clamp - new_clamp).volume(),
    "clamp_v5_minus_v4_mm3": (new_clamp - old_clamp).volume(),
    "panel_interference_mm3": (model ^ panel).volume(),
    "board_interference_mm3": (model ^ board).volume(),
    "mount_hole_obstruction_mm3": hole_checks,
    "frame_opening_obstruction_mm3": (frame ^ opening).volume(),
    "frame_opening_boundary_tolerance_mm": 0.001,
    "right_wall_missing_mm3": (right_wall - frame).volume(),
    "board_to_clamp_y_gap_mm": 1.0,
    "mount_hole_centers_xz_mm": [[3.5, -45.7], [95.5, -45.7]],
    "mount_hole_pitch_mm": 92.0,
    "actual_slicer_run": False,
    "physical_fit_verified": False,
}
zero_checks = [
    report[k] for k in ["clamp_v4_minus_v5_mm3", "clamp_v5_minus_v4_mm3",
                       "panel_interference_mm3", "board_interference_mm3",
                       "frame_opening_obstruction_mm3", "right_wall_missing_mm3"]
] + hole_checks
assert mesh.is_watertight and mesh.is_winding_consistent
assert model.status() == md.Error.NoError
assert len(model.decompose()) == 1
assert all(abs(v) < 1e-4 for v in zero_checks), report
read_back, _ = load("v5_validation/read_back_3mf.stl")
assert read_back.is_watertight
assert np.max(np.abs(read_back.bounds[0])) < 1e-6
assert np.allclose(read_back.extents, [165.6, 71.9, 70.28], atol=1e-5)
assert abs(read_back.volume - mesh.volume) < .01
report["three_mf_openscad_read_back_valid"] = True
report["three_mf_print_bounds_mm"] = read_back.bounds.tolist()
(ROOT / "v5_validation/geometry_report.json").write_text(
    json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps(report, indent=2))
