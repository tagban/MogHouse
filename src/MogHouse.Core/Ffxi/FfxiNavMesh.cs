using DotRecast.Core;
using DotRecast.Core.Numerics;
using DotRecast.Detour;
using DotRecast.Detour.Io;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// Ground height lookup, from the same navmeshes the server uses to walk mobs
/// around. This is what lets a character move without ending up inside the
/// terrain or floating over it - the client has no zone geometry of its own,
/// but the navmesh already encodes every walkable surface and its height.
///
/// The files are the standard Recast/Detour "MSET" tiled format that
/// LandSandBoat ships in its `navmeshes/` directory, one per zone, named after
/// the zone (`Bastok_Markets.nav`). Reading them needs no server and no game
/// client, and DotRecast is pure .NET, so this works the same on Windows,
/// macOS and Linux.
///
/// **Coordinates are not the same in both systems.** Detour is right-handed
/// Y-up and FFXI is left-handed Y-up, so Y and Z are sign-flipped between
/// them: `detour = (x, -y, -z)`. That is taken verbatim from the server's own
/// conversion, and getting it wrong yields heights that look plausible and are
/// silently wrong - the worst kind of bug in this area.
///
/// Like the compression tables, navmeshes are not bundled. They are GPLv3 data
/// derived from retail zone geometry, so where they come from is the
/// distributor's decision, not something to settle by committing them.
/// </summary>
public sealed class FfxiNavMesh
{
    private readonly DtNavMesh _navMesh;
    private readonly DtNavMeshQuery _query;

    /// <summary>
    /// How far above and below the query point to search for ground. Vertical
    /// slack has to be generous: a character standing on a step or a slope can
    /// be a couple of units off the polygon that actually holds it.
    /// </summary>
    private static readonly RcVec3f SearchExtents = new(2.0f, 8.0f, 2.0f);

    /// <summary>Matches the server's include filter - walkable surfaces and doors, not disabled polys.</summary>
    private readonly IDtQueryFilter _filter = new DtQueryDefaultFilter(
        includeFlags: 0x0001 | 0x0002 | 0x0004 | 0x0008,
        excludeFlags: 0x0010,
        areaCost: new float[64]);

    private FfxiNavMesh(DtNavMesh navMesh)
    {
        _navMesh = navMesh;
        _query = new DtNavMeshQuery(navMesh);
    }

    /// <summary>Recast's tiled-navmesh file marker, 'MSET'.</summary>
    private const int NavMeshSetMagic = ('M' << 24) | ('S' << 16) | ('E' << 8) | 'T';
    private const int NavMeshSetVersion = 1;

    /// <summary>
    /// Loads a single zone's `.nav` file.
    ///
    /// DotRecast's own <see cref="DtMeshSetReader"/> can't read these: it
    /// assumes 64-bit tile references, while the server builds its meshes with
    /// Detour's default 32-bit `dtTileRef`, so every tile header is misread and
    /// the first tile fails with "Invalid magic". The file walk is short enough
    /// to do here against the exact layout the server writes:
    ///
    ///   header: int magic, int version, int numTiles, dtNavMeshParams params
    ///   tile:   uint32 tileRef, int32 dataSize, then dataSize bytes
    /// </summary>
    public static FfxiNavMesh Load(string path)
    {
        byte[] bytes = File.ReadAllBytes(path);
        var reader = new BinaryReader(new MemoryStream(bytes));

        int magic = reader.ReadInt32();
        if (magic != NavMeshSetMagic)
        {
            throw new InvalidDataException($"{path} is not a Recast navmesh set (magic 0x{magic:X8}).");
        }

        int version = reader.ReadInt32();
        if (version != NavMeshSetVersion)
        {
            throw new InvalidDataException($"{path} is navmesh set version {version}, expected {NavMeshSetVersion}.");
        }

        int tileCount = reader.ReadInt32();

        var parameters = new DtNavMeshParams
        {
            orig = new RcVec3f(reader.ReadSingle(), reader.ReadSingle(), reader.ReadSingle()),
            tileWidth = reader.ReadSingle(),
            tileHeight = reader.ReadSingle(),
            maxTiles = reader.ReadInt32(),
            maxPolys = reader.ReadInt32(),
        };

        var mesh = new DtNavMesh();
        mesh.Init(ref parameters, MaxVertsPerPoly);

        for (int i = 0; i < tileCount; i++)
        {
            uint tileRef = reader.ReadUInt32();
            int dataSize = reader.ReadInt32();

            // The server stops at the first empty tile rather than treating it
            // as an error, so this does too.
            if (tileRef == 0 || dataSize == 0)
            {
                break;
            }

            byte[] tileData = reader.ReadBytes(dataSize);
            DtMeshData data = new DtMeshDataReader().Read(new RcByteBuffer(tileData), MaxVertsPerPoly, is32Bit: true);
            // Let Detour assign the tile reference rather than forcing the
            // file's: DotRecast validates a supplied lastRef against its own
            // tile indexing and silently rejects nearly every tile if it
            // disagrees. Nothing here needs the original refs preserved.
            mesh.AddTile(data, 0, 0, out _);
        }

        return new FfxiNavMesh(mesh);
    }

    private const int MaxVertsPerPoly = 6;

    /// <summary>
    /// Finds a zone's navmesh by name in <paramref name="directory"/>, or
    /// returns null if it isn't there. Zone names match the server's own file
    /// names, e.g. "Bastok_Markets".
    /// </summary>
    public static FfxiNavMesh? TryLoadZone(string directory, string zoneName)
    {
        string path = Path.Combine(directory, $"{zoneName}.nav");
        return File.Exists(path) ? Load(path) : null;
    }

    /// <summary>
    /// Ground height at an FFXI (x, z) position, using <paramref name="nearVertical"/>
    /// as the starting guess for which surface is meant - zones stack, so a
    /// bridge and the ground beneath it are both valid answers and only the
    /// current height distinguishes them.
    ///
    /// Returns false when the point is off the navmesh entirely, which is a
    /// real answer: it means the character cannot stand there.
    /// </summary>
    public bool TryGetGroundHeight(float x, float nearVertical, float z, out float height)
    {
        height = nearVertical;

        RcVec3f centre = ToDetour(x, nearVertical, z);

        DtStatus status = _query.FindNearestPoly(centre, SearchExtents, _filter, out long polyRef, out RcVec3f nearest, out _);
        if (status.Failed() || polyRef == 0)
        {
            return false;
        }

        if (_query.GetPolyHeight(polyRef, nearest, out float detourHeight).Failed())
        {
            // The polygon was found but has no height at that point - fall
            // back to the nearest point on it rather than reporting failure.
            height = -nearest.Y;
            return true;
        }

        height = -detourHeight;
        return true;
    }


    /// <summary>
    /// Walkable polygon outlines near a point, in FFXI coordinates, for drawing
    /// a map. Returns each polygon as a list of (x, depth) corners with height
    /// dropped, since this is for a top-down view.
    ///
    /// Only the 3x3 tile block around the point is walked, which is the same
    /// neighbourhood Detour itself queries - a whole zone would be thousands of
    /// polygons and none of the distant ones would be visible anyway.
    /// </summary>
    public IReadOnlyList<IReadOnlyList<(float X, float Depth)>> WalkablePolygons(float x, float z, float range)
    {
        var result = new List<IReadOnlyList<(float, float)>>();

        RcVec3f centre = ToDetour(x, 0f, z);
        _navMesh.CalcTileLoc(centre, out int tileX, out int tileY);

        var tiles = new DtMeshTile[8];
        float rangeSquared = range * range;

        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                int found = _navMesh.GetTilesAt(tileX + dx, tileY + dy, tiles, tiles.Length);

                for (int t = 0; t < found; t++)
                {
                    DtMeshData? data = tiles[t]?.data;
                    if (data?.polys is null || data.verts is null)
                    {
                        continue;
                    }

                    foreach (DtPoly poly in data.polys)
                    {
                        if (poly is null || poly.vertCount < 3)
                        {
                            continue;
                        }

                        // Off-mesh connections are links, not surfaces.
                        if ((poly.areaAndtype >> 6) != 0)
                        {
                            continue;
                        }

                        var corners = new List<(float, float)>(poly.vertCount);
                        bool near = false;

                        for (int v = 0; v < poly.vertCount; v++)
                        {
                            int index = poly.verts[v] * 3;
                            if (index + 2 >= data.verts.Length)
                            {
                                break;
                            }

                            // Undo the handedness flip on the way back out.
                            float px = data.verts[index];
                            float pz = -data.verts[index + 2];

                            corners.Add((px, pz));

                            float ddx = px - x;
                            float ddz = pz - z;
                            if ((ddx * ddx) + (ddz * ddz) <= rangeSquared)
                            {
                                near = true;
                            }
                        }

                        if (near && corners.Count >= 3)
                        {
                            result.Add(corners);
                        }
                    }
                }
            }
        }

        return result;
    }


    /// <summary>
    /// A walkable route between two points, as world-space waypoints.
    ///
    /// This is the same pathfinding the server uses to move mobs, so a route
    /// it returns is one the server will accept us walking: around walls,
    /// never through them. Returns an empty list when either end is off the
    /// mesh or no route exists.
    /// </summary>
    public IReadOnlyList<(float X, float Vertical, float Depth)> FindPath(
        float fromX, float fromVertical, float fromZ,
        float toX, float toVertical, float toZ)
    {
        RcVec3f start = ToDetour(fromX, fromVertical, fromZ);
        RcVec3f end = ToDetour(toX, toVertical, toZ);

        if (_query.FindNearestPoly(start, SearchExtents, _filter, out long startRef, out RcVec3f startOn, out _).Failed() || startRef == 0)
        {
            return [];
        }

        if (_query.FindNearestPoly(end, SearchExtents, _filter, out long endRef, out RcVec3f endOn, out _).Failed() || endRef == 0)
        {
            return [];
        }

        Span<long> polys = stackalloc long[MaxPathPolygons];
        if (_query.FindPath(startRef, endRef, startOn, endOn, _filter, polys, out int polyCount, polys.Length).Failed() || polyCount == 0)
        {
            return [];
        }

        // FindPath gives a corridor of polygons; FindStraightPath turns that
        // into the actual corner points to walk between.
        Span<DtStraightPath> straight = new DtStraightPath[MaxPathPoints];
        if (_query.FindStraightPath(startOn, endOn, polys[..polyCount], polyCount, straight, out int pointCount, straight.Length, 0).Failed())
        {
            return [];
        }

        var route = new List<(float, float, float)>(pointCount);
        for (int i = 0; i < pointCount; i++)
        {
            RcVec3f p = straight[i].pos;
            route.Add((p.X, -p.Y, -p.Z));
        }

        return route;
    }

    private const int MaxPathPolygons = 256;
    private const int MaxPathPoints = 128;

    /// <summary>True when a character could stand at this position at all.</summary>
    public bool IsWalkable(float x, float vertical, float z) =>
        TryGetGroundHeight(x, vertical, z, out _);

    /// <summary>
    /// Walks from one point towards another, stopping at the first wall.
    ///
    /// This is what collision actually needs, and a nearest-polygon lookup at
    /// the destination is not: FindNearestPoly searches a box and returns the
    /// closest walkable polygon, so a point *inside* a wall happily finds the
    /// floor on the other side and reports success. That looks exactly like
    /// working collision right up until you walk through a wall.
    ///
    /// Detour's raycast is the right tool: it traces along the surface and
    /// reports how far it got before meeting an edge with no neighbour, which
    /// is precisely a wall.
    /// </summary>
    /// <returns>False if the starting point isn't on the mesh at all.</returns>
    public bool TryMove(
        float fromX, float fromVertical, float fromZ,
        float toX, float toZ,
        out float resultX, out float resultVertical, out float resultZ, out bool blocked)
    {
        resultX = fromX;
        resultVertical = fromVertical;
        resultZ = fromZ;
        blocked = false;

        RcVec3f start = ToDetour(fromX, fromVertical, fromZ);

        if (_query.FindNearestPoly(start, SearchExtents, _filter, out long startRef, out RcVec3f startOnMesh, out _).Failed()
            || startRef == 0)
        {
            return false;
        }

        RcVec3f end = ToDetour(toX, fromVertical, toZ);

        Span<long> path = stackalloc long[16];
        DtStatus status = _query.Raycast(startRef, startOnMesh, end, _filter, out float t, out _, path, out _, path.Length);

        if (status.Failed())
        {
            blocked = true;
            return true;
        }

        // t >= 1 means the whole segment was walkable. Less than that is a wall
        // partway along, so stop just short of it - ending exactly on the
        // boundary tends to leave the next step wedged.
        float travelled = Math.Min(t, 1.0f);
        if (t < 1.0f)
        {
            blocked = true;
            travelled = Math.Max(0f, t - 0.1f);
        }

        resultX = fromX + ((toX - fromX) * travelled);
        resultZ = fromZ + ((toZ - fromZ) * travelled);

        if (TryGetGroundHeight(resultX, fromVertical, resultZ, out float ground))
        {
            resultVertical = ground;
        }

        return true;
    }

    /// <summary>
    /// FFXI to Detour. Right-handed versus left-handed: Y and Z flip sign.
    /// Taken from the server's own `toDetour`, not derived independently.
    /// </summary>
    private static RcVec3f ToDetour(float x, float y, float z) => new(x, -y, -z);
}
