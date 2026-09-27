"""Create schema-valid temporary cinema room probes for Dolby Atmos Renderer.

Files describe physical speaker XYZ coordinates and 32 MADI outputs. They do not
alter renderer settings; importing and restoring them is a separate UI action.
"""

import json
import math
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

NS = "http://www.dolby.com/cp/atmos/config"
XSI = "http://www.w3.org/2001/XMLSchema-instance"
ET.register_namespace("", NS)
ET.register_namespace("xsi", XSI)


def add(parent, name, text=None, **attrs):
    value = ET.SubElement(parent, f"{{{NS}}}{name}", attrs)
    if text is not None:
        value.text = str(text)
    return value


def dimensions(parent):
    d = add(parent, "dimensions")
    for name in ("screenWallWidth", "boothWallWidth", "houseLeftWallWidth", "houseRightWallWidth"):
        add(d, name, 10)
    for name in ("screenHouseLeftInteriorAngle", "screenHouseRightInteriorAngle",
                 "boothHouseLeftInteriorAngle", "boothHouseRightInteriorAngle"):
        add(d, name, "right")
    for name, value in (("floorElevationAtScreen", 0), ("ceilingElevationAtScreen", 6),
                        ("floorElevationAtTwoThirds", 0), ("ceilingElevationAtTwoThirds", 6),
                        ("floorElevationAtBooth", 0), ("ceilingElevationAtBooth", 6),
                        ("floorProfile", "flat"), ("ceilingProfile", "flat")):
        add(d, name, value)


def write_room(path, speakers):
    root = ET.Element(f"{{{NS}}}atmosConfigSuiteData", {"version": "1.3.0"})
    processor = add(root, "processorConfiguration")
    room_config = add(processor, "roomConfiguration")
    add(room_config, "name", path.stem)
    room = add(room_config, "room")
    dimensions(room)
    region = add(room, "region")
    add(region, "name", "Research room")
    dimensions(region)
    for i, (name, xyz, group, kind) in enumerate(speakers, 1):
        speaker = add(region, "speakerEndpoint", id=f"speaker_{i}")
        speaker.set(f"{{{XSI}}}type", kind)
        add(speaker, "name", name)
        position = add(speaker, "position")
        for axis, coordinate in zip(("x", "y", "z"), xyz):
            add(position, axis, f"{coordinate:.9f}")
        if group:
            add(speaker, "array", group)
    output = add(processor, "outputConfiguration", isLimiterEnabled="false")
    output.set(f"{{{XSI}}}type", "rmuOutputConfiguration")
    for i in range(1, len(speakers) + 1):
        route = add(output, "outputRoute")
        add(route, "outputRef", f"madi_{i}")
        add(route, "speakerEndpointRef", f"speaker_{i}")
    for i in range(1, 33):
        port = add(output, "madiOutput", id=f"madi_{i}")
        add(port, "peakOutputDbuPk", 24)
    add(output, "outputVoltage", 24)
    ET.indent(root)
    with path.open("xb") as stream:
        ET.ElementTree(root).write(stream, encoding="UTF-8", xml_declaration=True)


def main():
    destination = Path(sys.argv[1]).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    control = [
        ("L", (2, 0, 2), "L", "screenSpeaker"), ("R", (8, 0, 2), "R", "screenSpeaker"),
        ("C", (5, 0, 2), "C", "screenSpeaker"), ("LFE", (3, 0, .5), "LFE", "subwooferSpeaker"),
        ("Lss", (0, 6, 2), "Lss", "surroundSpeaker"), ("Rss", (10, 6, 2), "Rss", "surroundSpeaker"),
        ("Lrs", (1, 10, 2), "Lrs", "surroundSpeaker"), ("Rrs", (9, 10, 2), "Rrs", "surroundSpeaker"),
        ("Ltf", (2, 2, 5), "Lts", "surroundSpeaker"), ("Rtf", (8, 2, 5), "Rts", "surroundSpeaker"),
        ("Ltr", (2, 8, 5), "Lts", "surroundSpeaker"), ("Rtr", (8, 8, 5), "Rts", "surroundSpeaker"),
    ]
    # Same nominal 22.2 angles as the project's 9+10+3 layout. The listening
    # reference is (5,5,2) m; radius=3 m keeps the lower speakers above the floor.
    angles = [
        ("M+060",60,0,"L"), ("M-060",-60,0,"R"), ("M+000",0,0,"C"),
        ("LFE1",45,-30,"LFE"), ("M+135",135,0,"Lrs"), ("M-135",-135,0,"Rrs"),
        ("M+030",30,0,"Lc"), ("M-030",-30,0,"Rc"), ("M+180",180,0,"Crs"),
        ("LFE2",-45,-30,"LFE"), ("M+090",90,0,"Lss"), ("M-090",-90,0,"Rss"),
        ("U+045",45,30,"Lts"), ("U-045",-45,30,"Rts"), ("U+000",0,30,"Cts"),
        ("T+000",0,90,"Cts"), ("U+135",135,30,"Lts"), ("U-135",-135,30,"Rts"),
        ("U+090",90,30,"Lts"), ("U-090",-90,30,"Rts"), ("U+180",180,30,"Cts"),
        ("B+000",0,-30,None), ("B+045",45,-30,None), ("B-045",-45,-30,None),
    ]
    target = []
    for name, azimuth, elevation, group in angles:
        a, e = math.radians(azimuth), math.radians(elevation)
        xyz = (5 - 3*math.sin(a)*math.cos(e), 5 - 3*math.cos(a)*math.cos(e), 2 + 3*math.sin(e))
        kind = "subwooferSpeaker" if name.startswith("LFE") else "screenSpeaker" if abs(azimuth) <= 60 and elevation <= 0 else "surroundSpeaker"
        target.append((name, xyz, group, kind))
    schema = "/Applications/Dolby/Dolby Atmos Renderer/Dolby Atmos Renderer.app/Contents/Resources/atmosconfig/AtmosConfigSuiteData.xsd"
    results = []
    for name, speakers in (("control-714", control), ("target-222", target)):
        path = destination / f"{name}.dac"
        write_room(path, speakers)
        result = subprocess.run(["xmllint", "--noout", "--schema", schema, str(path)], capture_output=True, text=True)
        results.append(dict(file=str(path), speakers=len(speakers), schema_status=result.returncode,
                            message=result.stderr.strip(), coordinates=speakers))
        if result.returncode:
            raise RuntimeError(result.stderr)
    (destination / "validation.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps([{k:r[k] for k in ("file", "speakers", "schema_status")} for r in results]))


if __name__ == "__main__":
    main()
