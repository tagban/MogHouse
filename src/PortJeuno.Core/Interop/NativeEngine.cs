using System.Runtime.InteropServices;

namespace PortJeuno.Core.Interop;

// P/Invoke boundary matching native/portjeuno_interop/include/portjeuno_interop.h.
// NOT YET VERIFIED AGAINST A REAL NATIVE LIBRARY - portjeuno_interop hasn't
// been built yet (see native/portjeuno_interop/README.md). Signatures here
// are believed correct against the header but every call will throw
// DllNotFoundException until that native library exists on disk.
internal static partial class NativeMethods
{
    private const string LibraryName = "portjeuno_interop";

    [LibraryImport(LibraryName)]
    internal static partial IntPtr pj_game_create(
        [MarshalAs(UnmanagedType.LPStr)] string appName,
        uint appVersion);

    [LibraryImport(LibraryName)]
    internal static partial void pj_game_destroy(IntPtr game);

    [LibraryImport(LibraryName)]
    internal static unsafe partial void pj_game_set_tick_callback(
        IntPtr game,
        delegate* unmanaged[Cdecl]<double, IntPtr, void> callback,
        IntPtr userData);

    [LibraryImport(LibraryName)]
    internal static unsafe partial void pj_game_set_error_callback(
        IntPtr game,
        delegate* unmanaged[Cdecl]<IntPtr, IntPtr, void> callback,
        IntPtr userData);

    // Blocking - see the header. Must be called from a dedicated thread.
    [LibraryImport(LibraryName)]
    internal static partial void pj_game_run(IntPtr game);

    [LibraryImport(LibraryName)]
    internal static partial void pj_game_close(IntPtr game);
}

/// <summary>
/// Managed wrapper around a native PortJeuno engine instance. The native
/// engine owns its own main-loop thread once <see cref="Run"/> is called
/// (mirroring lotus-ffxi's own usage - see native/portjeuno_interop/README.md),
/// so <see cref="Run"/> must be invoked from a dedicated background thread,
/// never from a UI thread.
/// </summary>
public sealed class PortJeunoEngine : IDisposable
{
    private readonly IntPtr _handle;
    private readonly GCHandle _selfHandle;
    private bool _disposed;

    public event Action<double>? Tick;
    public event Action<string>? EngineError;

    private unsafe PortJeunoEngine(string appName, uint appVersion)
    {
        _selfHandle = GCHandle.Alloc(this, GCHandleType.Normal);
        _handle = NativeMethods.pj_game_create(appName, appVersion);
        if (_handle == IntPtr.Zero)
        {
            _selfHandle.Free();
            throw new InvalidOperationException("pj_game_create returned a null handle.");
        }

        NativeMethods.pj_game_set_tick_callback(_handle, &OnTick, GCHandle.ToIntPtr(_selfHandle));
        NativeMethods.pj_game_set_error_callback(_handle, &OnError, GCHandle.ToIntPtr(_selfHandle));
    }

    public static PortJeunoEngine Create(string appName, uint appVersion) => new(appName, appVersion);

    /// <summary>
    /// Blocks the calling thread until <see cref="Close"/> is observed by
    /// the native loop. Call this from a dedicated background thread.
    /// </summary>
    public void Run() => NativeMethods.pj_game_run(_handle);

    /// <summary>Safe to call from any thread, including from a Tick handler.</summary>
    public void Close() => NativeMethods.pj_game_close(_handle);

    [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static void OnTick(double deltaSeconds, IntPtr userData)
    {
        if (GCHandle.FromIntPtr(userData).Target is PortJeunoEngine engine)
        {
            engine.Tick?.Invoke(deltaSeconds);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static void OnError(IntPtr messagePtr, IntPtr userData)
    {
        if (GCHandle.FromIntPtr(userData).Target is PortJeunoEngine engine)
        {
            string message = Marshal.PtrToStringUTF8(messagePtr) ?? string.Empty;
            engine.EngineError?.Invoke(message);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        NativeMethods.pj_game_destroy(_handle);
        _selfHandle.Free();
    }
}
