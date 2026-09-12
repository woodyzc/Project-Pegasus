#!/usr/bin/env python3
"""Fetch real roads from OpenStreetMap and write the .prd the head unit reads.

    osm2prd.py --bbox S W N E  OUT.prd
    osm2prd.py --postcode 20874 OUT.prd

Data comes from the Overpass API, which is the right tool for this and not the
same thing as scraping tiles. Overpass exists to answer targeted queries for
map DATA; the tile servers serve rendered PICTURES and their usage policy
forbids bulk downloading. One query for a postcode-sized area is well within
what Overpass is for.

ATTRIBUTION
-----------
The data is OpenStreetMap, licensed ODbL. Anything built from it has to carry
"(c) OpenStreetMap contributors" somewhere a user can see. On this device that
is a line on the map page, and it is not optional.

WHAT IT KEEPS
-------------
Roads a cyclist needs to recognise a junction, and water because a river is the
strongest landmark on a small screen. Everything else OSM knows -- buildings,
landuse, addresses, turn restrictions -- is dropped. At 240px a map dense
enough to be pretty is too dense to read at speed.
"""
import argparse
import json
import math
import struct
import sys
import time
import urllib.parse
import urllib.request

OVERPASS = "https://overpass-api.de/api/interpreter"
UA = "ProjectPegasus/1.0 (DIY bike computer; personal use)"

# OSM highway tag -> our four classes. The grouping is about how a road reads
# on a 240px panel, not about OSM's hierarchy: a rider needs "big road",
# "through road", "side street", and the three OSM levels above tertiary all
# look the same at this scale.
CLASS_MINOR, CLASS_SECONDARY, CLASS_ARTERY, CLASS_WATER = 0, 1, 2, 3

HIGHWAY_CLASS = {
    "motorway": CLASS_ARTERY, "motorway_link": CLASS_ARTERY,
    "trunk": CLASS_ARTERY, "trunk_link": CLASS_ARTERY,
    "primary": CLASS_ARTERY, "primary_link": CLASS_ARTERY,
    "secondary": CLASS_SECONDARY, "secondary_link": CLASS_SECONDARY,
    "tertiary": CLASS_SECONDARY, "tertiary_link": CLASS_SECONDARY,
    "residential": CLASS_MINOR, "unclassified": CLASS_MINOR,
    "living_street": CLASS_MINOR, "cycleway": CLASS_MINOR,
}
CLASS_NAME = {0: "minor", 1: "secondary", 2: "artery", 3: "water"}

# Overpass caps a way at 2000 nodes, but the firmware stores a count as u16 and
# long ways are split anyway. 1000 keeps each record small enough that culling
# one bounding box rejects a useful amount of geometry.
MAX_WAY_POINTS = 1000


def geocode_postcode(code: str, country: str = "us"):
    q = urllib.parse.urlencode(
        {"postalcode": code, "country": country, "format": "json", "limit": 1})
    req = urllib.request.Request(
        f"https://nominatim.openstreetmap.org/search?{q}", headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = json.load(r)
    if not data:
        raise SystemExit(f"no such postcode: {code}")
    s, n, w, e = (float(v) for v in data[0]["boundingbox"])
    return s, w, n, e, data[0]["display_name"]


def overpass(south, west, north, east):
    bbox = f"{south},{west},{north},{east}"
    highways = "|".join(HIGHWAY_CLASS)
    query = f"""
[out:json][timeout:90];
(
  way["highway"~"^({highways})$"]({bbox});
  way["waterway"~"^(river|stream|canal)$"]({bbox});
);
out geom;
"""
    body = urllib.parse.urlencode({"data": query}).encode()
    req = urllib.request.Request(OVERPASS, data=body, headers={"User-Agent": UA})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return json.load(r)
        except Exception as exc:
            # Overpass rate-limits and sheds load under contention; backing off
            # is expected behaviour, not an error to give up on.
            if attempt == 2:
                raise
            print(f"  attempt {attempt + 1} failed ({exc}), retrying...")
            time.sleep(5 * (attempt + 1))
    return None


def classify(tags):
    if "waterway" in tags:
        return CLASS_WATER
    return HIGHWAY_CLASS.get(tags.get("highway", ""))


def build(elements):
    ways = []
    for el in elements:
        if el.get("type") != "way" or "geometry" not in el:
            continue
        klass = classify(el.get("tags", {}))
        if klass is None:
            continue
        pts = [(int(round(p["lat"] * 1e7)), int(round(p["lon"] * 1e7)))
               for p in el["geometry"]]
        if len(pts) < 2:
            continue
        # Split over-long ways, overlapping by one point so the line stays
        # continuous across the join rather than showing a gap.
        for i in range(0, len(pts) - 1, MAX_WAY_POINTS - 1):
            chunk = pts[i:i + MAX_WAY_POINTS]
            if len(chunk) >= 2:
                ways.append((klass, chunk))
    return ways


def write_prd(path, ways):
    lats = [p[0] for _, pts in ways for p in pts]
    lons = [p[1] for _, pts in ways for p in pts]

    blob = bytearray(b"PRD1")
    blob += struct.pack("<I", len(ways))
    blob += struct.pack("<iiii", min(lats), min(lons), max(lats), max(lons))
    for klass, pts in ways:
        blob += struct.pack("<BBH", klass, 0, len(pts))
        for la, lo in pts:
            blob += struct.pack("<ii", la, lo)
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--bbox", nargs=4, type=float, metavar=("S", "W", "N", "E"))
    ap.add_argument("--postcode")
    ap.add_argument("--country", default="us")
    args = ap.parse_args()

    if args.postcode:
        south, west, north, east, name = geocode_postcode(args.postcode, args.country)
        print(f"{args.postcode}: {name}")
    elif args.bbox:
        south, west, north, east = args.bbox
        name = "bbox"
    else:
        raise SystemExit("need --bbox or --postcode")

    km_ns = (north - south) * 111.32
    km_ew = (east - west) * 111.32 * math.cos(math.radians((north + south) / 2))
    print(f"bbox {south:.4f},{west:.4f} .. {north:.4f},{east:.4f}"
          f"  ({km_ew:.1f} x {km_ns:.1f} km)")
    print("querying Overpass...")

    data = overpass(south, west, north, east)
    ways = build(data.get("elements", []))
    if not ways:
        raise SystemExit("no roads found in that box")

    size = write_prd(args.out, ways)
    points = sum(len(p) for _, p in ways)

    counts = {}
    for klass, pts in ways:
        counts[klass] = counts.get(klass, 0) + 1

    print()
    print(f"{len(ways)} ways, {points} points -> {args.out}")
    for k in sorted(counts):
        print(f"    {CLASS_NAME[k]:10} {counts[k]:6}")
    print(f"  {size} bytes ({size / 1024:.0f} KB)")

    # The comparison that justified this architecture, on real data this time.
    tiles = math.ceil(km_ew / 0.936) * math.ceil(km_ns / 0.936) * 131076
    print(f"  same area as z15 raster tiles: {tiles / 1024 / 1024:.0f} MB"
          f"  ({tiles / size:.0f}x larger)")
    print()
    print("Data (c) OpenStreetMap contributors, ODbL.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
