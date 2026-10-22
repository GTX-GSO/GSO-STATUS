"""Package the exported V6 STL in the user's final upright orientation."""
from pathlib import Path
import zipfile
import xml.etree.ElementTree as ET
import numpy as np
import trimesh

ROOT = Path(__file__).resolve().parent.parent
NAME = "ESP32_P4_monitor_integrated_clamp_V6"
mesh = trimesh.load(ROOT / (NAME + ".stl"), force="mesh")
# Proper rotation: +90 deg around Y. The former right short edge is down,
# long screen dimension is vertical, PCB remains in the upper half.
vertices = np.column_stack((mesh.vertices[:, 2], mesh.vertices[:, 1],
                            -mesh.vertices[:, 0]))
vertices -= vertices.min(axis=0)
assert np.allclose(np.ptp(vertices, axis=0), [71.9, 76.91, 165.6])

NS = "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
ET.register_namespace("", NS)
def q(tag):
    return "{" + NS + "}" + tag

model = ET.Element(q("model"), {"unit": "millimeter"})
ET.SubElement(model, q("metadata"), {"name": "Title"}).text = (
    "V6 - 6.8mm screen, both PCB screw holes shifted left 1mm")
ET.SubElement(model, q("metadata"), {"name": "Description"}).text = (
    "Upright geometry only; no support/printer settings. "
    "Left is defined looking at the screen FRONT. V4 clamp unchanged.")
obj = ET.SubElement(ET.SubElement(model, q("resources")), q("object"),
                    {"id": "1", "type": "model"})
part = ET.SubElement(obj, q("mesh"))
vs = ET.SubElement(part, q("vertices"))
ts = ET.SubElement(part, q("triangles"))
for v in vertices:
    ET.SubElement(vs, q("vertex"), dict(zip(
        ("x", "y", "z"), (f"{a:.9g}" for a in v))))
for f in mesh.faces:
    ET.SubElement(ts, q("triangle"), dict(zip(
        ("v1", "v2", "v3"), map(str, f))))
ET.SubElement(ET.SubElement(model, q("build")), q("item"), {"objectid": "1"})
with zipfile.ZipFile(ROOT / (NAME + "_upright.3mf"), "w",
                     zipfile.ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml", '''<?xml version="1.0"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
</Types>''')
    z.writestr("_rels/.rels", '''<?xml version="1.0"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>''')
    z.writestr("3D/3dmodel.model", ET.tostring(
        model, encoding="utf-8", xml_declaration=True))
(ROOT / "v6_validation/read_back_upright.scad").write_text(
    f'import("../{NAME}_upright.3mf");\n', encoding="utf-8")
print("Packaged upright 3MF:", np.ptp(vertices, axis=0))
