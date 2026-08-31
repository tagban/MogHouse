using System.Reflection;
using System.Runtime.InteropServices;

namespace MogHouse.Core.Interop;

/// <summary>What the radar shows, in world coordinates.</summary>
/// <remarks>
/// Blittable and laid out to match MhRadarEntity, so an array of these crosses
/// the boundary as a pointer with no marshalling.
/// </remarks>
[StructLayout(LayoutKind.Sequential)]
public struct NativeRadarEntity
{
    public float X;
    public float Z;

    /// <summary>World height, Y up. The radar ignores it; the body does not.</summary>
    public float Y;

    /// <summary>Compass heading in radians, 0 along +z.</summary>
    public float Heading;

    /// <summary>Cast from <see cref="Ffxi.FfxiEntityKind"/>; the values match.</summary>
    public int Kind;

    /// <summary>
    /// Shown over the body. Fixed width and ASCII so the array stays blittable.
    /// </summary>
    public unsafe fixed byte Name[20];

    /// <summary>
    /// The server's id for this entity. The renderer uses it to find a name in
    /// the zone's own name table, because the server sends NPCs without one.
    /// </summary>
    public uint Id;

    /// <summary>
    /// Non-zero when the name should stay hidden until the entity is targeted.
    /// </summary>
    public int NameHidden;

    /// <summary>
    /// Race, face, head, body, hands, legs, feet - slot tags already stripped.
    /// All zero when the server describes this one as a fixed model instead.
    /// </summary>
    public unsafe fixed ushort Look[7];

    /// <summary>GM level, 0 for an ordinary player.</summary>
    public int GmLevel;

    /// <summary>Writes a look in, or leaves it zeroed.</summary>
    public unsafe void SetLook(Ffxi.FfxiEntityLook? look)
    {
        if (look is null || !look.IsEquipment)
        {
            return;
        }

        fixed (ushort* target = Look)
        {
            target[0] = look.Race;
            target[1] = look.Face;
            target[2] = Ffxi.FfxiEntityLook.ModelOf(look.Head);
            target[3] = Ffxi.FfxiEntityLook.ModelOf(look.Body);
            target[4] = Ffxi.FfxiEntityLook.ModelOf(look.Hands);
            target[5] = Ffxi.FfxiEntityLook.ModelOf(look.Legs);
            target[6] = Ffxi.FfxiEntityLook.ModelOf(look.Feet);
        }
    }

    /// <summary>Writes a name in, truncated and NUL terminated.</summary>
    public unsafe void SetName(string? value)
    {
        fixed (byte* target = Name)
        {
            int written = 0;
            if (!string.IsNullOrEmpty(value))
            {
                // The renderer's font has no lower case and no accents, and it
                // turns anything it does not know into a space. Cutting to
                // ASCII here keeps that decision in one place.
                foreach (char c in value)
                {
                    if (written >= 19)
                    {
                        break;
                    }
                    target[written++] = c < 128 ? (byte)c : (byte)' ';
                }
            }
            target[written] = 0;
        }
    }
}

/// <summary>What to open the viewer on.</summary>
public sealed record NativeViewerOptions
{
    public required string ZonePath { get; init; }
    public required string KeyTablePath { get; init; }
    public required string KeyTable2Path { get; init; }

    /// <summary>"race,face,head,body,hands,legs,feet", or null for no character.</summary>
    public string? Look { get; init; }

    /// <summary>The name to draw over our own character.</summary>
    public string? PlayerName { get; init; }

    /// <summary>The server's Vana'diel clock in seconds; 0 runs the renderer's own.</summary>
    public uint ServerClock { get; init; }

    /// <summary>"x,y,z", or null to let the viewer pick somewhere standable.</summary>
    public string? CharacterAt { get; init; }

    /// <summary>Compass degrees.</summary>
    public string? CharacterFacing { get; init; }

    /// <summary>Shown along the bottom of the radar.</summary>
    public string? ZoneName { get; init; }

    /// <summary>Vana'diel clock as hhmm, or null to let the day run.</summary>
    public int? TimeOfDay { get; init; }

    /// <summary>Writes one frame here and closes. For checking unattended.</summary>
    public string? ScreenshotPath { get; init; }

    /// <summary>Frames to wait before that shot.</summary>
    public int ScreenshotAfterFrames { get; init; }
}

/// <summary>
/// The renderer, loaded into this process rather than run as a second one.
///
/// MogHouse ships as a single application, so the client owns the renderer
/// rather than talking to it. <see cref="Run"/> blocks and owns the window and
/// its event loop, so it wants a thread of its own; everything else here is
/// safe to call from another one while it runs.
/// </summary>
public sealed partial class NativeViewer : IDisposable
{
    private const string LibraryName = "moghouse_interop";

    private IntPtr _handle;
    private bool _disposed;

    static NativeViewer()
    {
        // The native library sits with the renderer build rather than beside
        // the managed assemblies, and it needs Dawn and SDL from that same
        // directory. Resolving it explicitly beats copying four DLLs around
        // and getting one of them stale.
        NativeLibrary.SetDllImportResolver(typeof(NativeViewer).Assembly, Resolve);
    }

    private static IntPtr Resolve(string name, Assembly assembly, DllImportSearchPath? path)
    {
        if (name != LibraryName)
        {
            return IntPtr.Zero;
        }

        foreach (string directory in SearchDirectories())
        {
            string candidate = Path.Combine(directory, OperatingSystem.IsWindows()
                ? $"{LibraryName}.dll"
                : OperatingSystem.IsMacOS() ? $"lib{LibraryName}.dylib" : $"lib{LibraryName}.so");

            if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out IntPtr loaded))
            {
                // The renderer looks for its glyph atlas beside whatever loaded
                // it, and in-process that is a .NET host somewhere else. Only
                // this code knows where the shim actually came from, so it says.
                Environment.SetEnvironmentVariable("MOGHOUSE_NATIVE_DIR", directory);
                return loaded;
            }
        }

        // Fall through to the default search, which is right once everything
        // is published into one directory.
        return IntPtr.Zero;
    }

    private static IEnumerable<string> SearchDirectories()
    {
        if (Environment.GetEnvironmentVariable("MOGHOUSE_NATIVE_DIR") is { Length: > 0 } configured)
        {
            yield return configured;
        }

        yield return AppContext.BaseDirectory;

        // The development layout: the renderer builds into build-renderer at
        // the repository root, which is several levels above bin/Debug/net10.0.
        string? walk = AppContext.BaseDirectory;
        for (int i = 0; i < 6 && walk is not null; i++)
        {
            yield return Path.Combine(walk, "build-renderer", "moghouse_interop");
            walk = Path.GetDirectoryName(walk.TrimEnd(Path.DirectorySeparatorChar));
        }
    }

    public NativeViewer(NativeViewerOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);

        // Every string is copied on the native side during the create call, so
        // these can be freed the moment it returns.
        IntPtr zone = Utf8(options.ZonePath);
        IntPtr keys = Utf8(options.KeyTablePath);
        IntPtr keys2 = Utf8(options.KeyTable2Path);
        IntPtr look = Utf8(options.Look);
        IntPtr at = Utf8(options.CharacterAt);
        IntPtr facing = Utf8(options.CharacterFacing);
        IntPtr shot = Utf8(options.ScreenshotPath);
        IntPtr zoneName = Utf8(options.ZoneName);
        IntPtr playerName = Utf8(options.PlayerName);

        try
        {
            Options native = new()
            {
                ZonePath = zone,
                KeyTablePath = keys,
                KeyTable2Path = keys2,
                Look = look,
                ServerClock = options.ServerClock,
                CharacterAt = at,
                CharacterFacing = facing,
                ZoneName = zoneName,
                PlayerName = playerName,
                TimeOfDay = options.TimeOfDay ?? -1,
                ScreenshotPath = shot,
                ScreenshotAfterFrames = options.ScreenshotAfterFrames,
            };

            _handle = mh_viewer_create(in native);
            if (_handle == IntPtr.Zero)
            {
                throw new InvalidOperationException("the renderer refused to start");
            }
        }
        finally
        {
            foreach (IntPtr held in new[] { zone, keys, keys2, look, at, facing, shot, zoneName })
            {
                if (held != IntPtr.Zero)
                {
                    Marshal.FreeCoTaskMem(held);
                }
            }
        }
    }

    /// <summary>Opens the window and runs until it closes. Blocking.</summary>
    public int Run()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        return mh_viewer_run(_handle);
    }

    /// <summary>Replaces what the radar shows. Safe to call while Run is going.</summary>
    public void SetEntities(ReadOnlySpan<NativeRadarEntity> entities)
    {
        if (_disposed)
        {
            return;
        }
        mh_viewer_set_entities(_handle, entities, entities.Length);
    }

    /// <summary>
    /// Shows one line in the renderer's chat panel.
    /// </summary>
    public void PushChat(string line)
    {
        if (_disposed || string.IsNullOrEmpty(line))
        {
            return;
        }
        mh_viewer_push_chat(_handle, line);
    }

    /// <summary>
    /// Where the character has walked to, or false before the first frame.
    /// Y is up; FFXI's own vertical is the negation.
    /// </summary>
    public bool TryGetCharacter(out float x, out float y, out float z, out float heading)
    {
        x = y = z = heading = 0;
        if (_disposed)
        {
            return false;
        }
        return mh_viewer_get_character(_handle, out x, out y, out z, out heading) != 0;
    }

    /// <summary>
    /// Whether the player asked to jump since this was last called. Consumes
    /// it, so each jump is reported once.
    /// </summary>
    public bool TakeJump() => !_disposed && _handle != IntPtr.Zero && mh_viewer_take_jump(_handle) != 0;

    /// <summary>
    /// The next line the player typed, or null if they have not pressed return
    /// since this was last called.
    /// </summary>
    public unsafe string? TakeChat()
    {
        if (_disposed || _handle == IntPtr.Zero)
        {
            return null;
        }

        // Chat is capped well under this on the wire; the buffer only has to
        // outlast one line.
        byte* buffer = stackalloc byte[256];
        if (mh_viewer_take_chat(_handle, buffer, 256) == 0)
        {
            return null;
        }

        return Marshal.PtrToStringUTF8((IntPtr)buffer);
    }

    /// <summary>Asks the viewer to close. Run returns shortly afterwards.</summary>
    public void Stop()
    {
        if (!_disposed)
        {
            mh_viewer_stop(_handle);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;

        if (_handle != IntPtr.Zero)
        {
            mh_viewer_destroy(_handle);
            _handle = IntPtr.Zero;
        }
    }

    private static IntPtr Utf8(string? text) => text is null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(text);

    [StructLayout(LayoutKind.Sequential)]
    private struct Options
    {
        public IntPtr ZonePath;
        public IntPtr KeyTablePath;
        public IntPtr KeyTable2Path;
        public IntPtr Look;
        public uint ServerClock;
        public IntPtr CharacterAt;
        public IntPtr CharacterFacing;
        public IntPtr ZoneName;
        public IntPtr PlayerName;
        public int TimeOfDay;
        public IntPtr ScreenshotPath;
        public int ScreenshotAfterFrames;
    }

    [LibraryImport(LibraryName)]
    private static partial IntPtr mh_viewer_create(in Options options);

    [LibraryImport(LibraryName)]
    private static partial int mh_viewer_run(IntPtr viewer);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_entities(IntPtr viewer, ReadOnlySpan<NativeRadarEntity> entities, int count);

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    private static partial void mh_viewer_push_chat(IntPtr viewer, string line);

    [LibraryImport(LibraryName)]
    private static partial int mh_viewer_get_character(IntPtr viewer, out float x, out float y, out float z,
                                                       out float heading);

    [LibraryImport(LibraryName)]
    private static partial int mh_viewer_take_jump(IntPtr viewer);

    [LibraryImport(LibraryName)]
    private static unsafe partial int mh_viewer_take_chat(IntPtr viewer, byte* buffer, int capacity);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_stop(IntPtr viewer);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_destroy(IntPtr viewer);
}
