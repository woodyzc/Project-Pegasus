#!/usr/bin/env python3
"""Plan a cycling route over OpenStreetMap data and write it as a .gpx.

    route2gpx.py out.gpx "14517 Bubbling Spring Rd, Boyds MD" "39.16,-77.29" ...

Each argument after the output file is a waypoint: either an address to
geocode, or a bare "lat,lon". The route runs through them in the order given.

WHY THIS EXISTS
---------------
The head unit follows a .gpx from the card, and there was no way to make one
here. The obvious answer is a routing API, and the two within reach are not:
Mapbox needs a token that deliberately lives only on the phone, and the public
OSRM demo server is unreachable from this machine. Both of those are fine
reasons to stop; neither is a reason the route cannot be planned, because the
road data itself is one Overpass query away and routing over it is a shortest
path.

So this fetches the roads once and runs Dijkstra over them. It is not a
navigation service and does not try to be: no turn restrictions, no one-way
enforcement, no elevation. For drawing a line on a bike computer's map, what
matters is that the line follows real roads, and it does.

ATTRIBUTION
-----------
OpenStreetMap under ODbL, like the road extract. Anything built from it carries
"(c) OpenStreetMap contributors", which the map page already shows.
"""
import argparse
import heapq
import json
import math
import sys
import time
import urllib.parse
import urllib.request
import xml.sax.saxutils as sax

OVERPASS = "https://overpass-api.de/api/interpreter"
NOMINATIM = "https://nominatim.openstreetmap.org/search"
UA = "ProjectPegasus/1.0 (DIY bike computer; personal use)"

# Cost multipliers, not bans. A rider will use a main road for a hundred metres
# to reach a lane, and a router that refuses puts them on a two-mile detour --
# so the fast roads are expensive rather than forbidden. Anything not listed
# gets NEUTRAL, which keeps an unfamiliar tag from silently becoming a shortcut.
PREFERENCE = {
    "cycleway": 0.5, "path": 0.7, "track": 0.9, "footway": 1.2,
    "residential": 0.9, "living_street": 0.9, "unclassified": 1.0,
    "service": 1.3, "tertiary": 1.1, "tertiary_link": 1.1,
    "secondary": 1.6, "secondary_link": 1.6,
    "primary": 2.6, "primary_link": 2.6,
    "trunk": 6.0, "trunk_link": 6.0,
    "motorway": 1000.0, "motorway_link": 1000.0,  # effectively refused
}
NEUTRAL = 1.5

EARTH_R = 6371000.0


def metres(a, b):
    lat1, lon1 = a
    lat2, lon2 = b
    dlat = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1) * math.cos(math.radians((lat1 + lat2) / 2))
    return math.hypot(dlat, dlon) * EARTH_R


def geocode(query):
    q = urllib.parse.urlencode({"q": query, "format": "json", "limit": 1,
                                "countrycodes": "us"})
    req = urllib.request.Request(f"{NOMINATIM}?{q}", headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = json.load(r)
    if not data:
        raise SystemExit(f"cannot find: {query}")
    return float(data[0]["lat"]), float(data[0]["lon"]), data[0]["display_name"]


def waypoint(text):
    """A bare "lat,lon" or something to look up."""
    parts = text.split(",")
    if len(parts) == 2:
        try:
            return float(parts[0]), float(parts[1]), text
        except ValueError:
            pass
    lat, lon, name = geocode(text)
    time.sleep(1.1)  # Nominatim asks for one request a second, and asking is free
    return lat, lon, name


def fetch_ways(south, west, north, east):
    kinds = "|".join(sorted(PREFERENCE))
    query = f"""
[out:json][timeout:120];
way["highway"~"^({kinds})$"]({south},{west},{north},{east});
out geom;
"""
    body = urllib.parse.urlencode({"data": query}).encode()
    req = urllib.request.Request(OVERPASS, data=body, headers={"User-Agent": UA})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=180) as r:
                return json.load(r)
        except Exception as exc:  # Overpass is busy more often than it is broken
            if attempt == 2:
                raise
            print(f"  overpass retry ({exc})", file=sys.stderr)
            time.sleep(5 * (attempt + 1))
    return {}


def build_graph(elements):
    """node id -> (lat, lon), and node id -> [(neighbour, cost, metres)]."""
    coords = {}
    edges = {}
    for el in elements:
        if el.get("type") != "way":
            continue
        geometry = el.get("geometry") or []
        nodes = el.get("nodes") or []
        if len(geometry) < 2 or len(nodes) != len(geometry):
            continue
        weight = PREFERENCE.get(el.get("tags", {}).get("highway", ""), NEUTRAL)
        for i in range(len(nodes) - 1):
            a, b = nodes[i], nodes[i + 1]
            pa = (geometry[i]["lat"], geometry[i]["lon"])
            pb = (geometry[i + 1]["lat"], geometry[i + 1]["lon"])
            coords[a] = pa
            coords[b] = pb
            d = metres(pa, pb)
            # Undirected. One-way streets matter to a car and rarely to this
            # job: the output is a line to follow on a map, not a set of legal
            # instructions, and honouring them here would send the route the
            # long way round for no gain a rider would notice.
            edges.setdefault(a, []).append((b, d * weight, d))
            edges.setdefault(b, []).append((a, d * weight, d))
    return coords, edges


def nearest_node(coords, point):
    best = None
    best_d = None
    for node, p in coords.items():
        d = metres(point, p)
        if best_d is None or d < best_d:
            best_d, best = d, node
    return best, best_d


def shortest_path(edges, start, goal):
    """Dijkstra. A* would be faster and this runs once on a laptop."""
    dist = {start: 0.0}
    prev = {}
    seen = set()
    queue = [(0.0, start)]
    while queue:
        cost, node = heapq.heappop(queue)
        if node in seen:
            continue
        seen.add(node)
        if node == goal:
            break
        for nxt, weight, _ in edges.get(node, ()):
            new = cost + weight
            if new < dist.get(nxt, float("inf")):
                dist[nxt] = new
                prev[nxt] = node
                heapq.heappush(queue, (new, nxt))
    if goal not in dist:
        return None
    path = [goal]
    while path[-1] != start:
        path.append(prev[path[-1]])
    path.reverse()
    return path


def write_gpx(path, points, name):
    with open(path, "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
        f.write('<gpx version="1.1" creator="Project Pegasus route2gpx"\n')
        f.write('     xmlns="http://www.topografix.com/GPX/1/1">\n')
        f.write("  <metadata>\n")
        f.write(f"    <name>{sax.escape(name)}</name>\n")
        f.write("    <copyright author=\"OpenStreetMap contributors\">\n")
        f.write("      <license>https://opendatacommons.org/licenses/odbl/</license>\n")
        f.write("    </copyright>\n")
        f.write("  </metadata>\n")
        f.write("  <trk>\n")
        f.write(f"    <name>{sax.escape(name)}</name>\n")
        f.write("    <trkseg>\n")
        for lat, lon in points:
            f.write(f'      <trkpt lat="{lat:.7f}" lon="{lon:.7f}"></trkpt>\n')
        f.write("    </trkseg>\n  </trk>\n</gpx>\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("waypoints", nargs="+")
    ap.add_argument("--name", default="Route")
    ap.add_argument("--loop", action="store_true",
                    help="return to the first waypoint at the end")
    args = ap.parse_args()

    stops = [waypoint(w) for w in args.waypoints]
    if args.loop:
        stops.append(stops[0])
    for lat, lon, name in stops:
        print(f"  {lat:.5f},{lon:.5f}  {name}")

    lats = [s[0] for s in stops]
    lons = [s[1] for s in stops]
    pad = 0.03  # room for the route to bulge outside the waypoints' own box
    print("querying Overpass...")
    data = fetch_ways(min(lats) - pad, min(lons) - pad, max(lats) + pad, max(lons) + pad)

    coords, edges = build_graph(data.get("elements", []))
    print(f"  {len(coords)} nodes, {sum(len(v) for v in edges.values()) // 2} edges")
    if not coords:
        raise SystemExit("no roads in that area")

    points = []
    total = 0.0
    for i in range(len(stops) - 1):
        a, da = nearest_node(coords, (stops[i][0], stops[i][1]))
        b, db = nearest_node(coords, (stops[i + 1][0], stops[i + 1][1]))
        if da > 500 or db > 500:
            print(f"  warning: a waypoint is {max(da, db):.0f} m from the nearest road")
        leg = shortest_path(edges, a, b)
        if leg is None:
            raise SystemExit(f"no route between waypoints {i + 1} and {i + 2}")
        for node in (leg if not points else leg[1:]):
            points.append(coords[node])
        for j in range(len(leg) - 1):
            total += metres(coords[leg[j]], coords[leg[j + 1]])
        print(f"  leg {i + 1}: {len(leg)} points")

    write_gpx(args.out, points, args.name)
    print(f"{len(points)} points, {total / 1000:.2f} km -> {args.out}")
    print("Data (c) OpenStreetMap contributors, ODbL.")


if __name__ == "__main__":
    main()
