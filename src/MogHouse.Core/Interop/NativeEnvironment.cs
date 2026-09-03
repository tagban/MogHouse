using System.Runtime.InteropServices;

namespace MogHouse.Core.Interop;

/// <summary>
/// Sets an environment variable so that native code in this process can also
/// see it.
///
/// <para>
/// <see cref="Environment.SetEnvironmentVariable(string, string)"/> is not
/// enough on its own, on any platform, and for a different reason on each.
/// </para>
///
/// <para>
/// On Unix - macOS and Linux both - .NET keeps its own managed copy and never
/// touches the C library's environment, so native <c>getenv</c> returns null
/// for anything set this way.
/// </para>
///
/// <para>
/// On Windows the managed setter does reach the real process environment, via
/// <c>SetEnvironmentVariableW</c>. But the C runtime's <c>getenv</c> does not
/// read the process environment: it reads a private copy the runtime took when
/// it started, and only the runtime's own <c>_putenv</c> family updates that
/// copy. So a variable set from managed code is visible to Win32 and invisible
/// to <c>std::getenv</c>, which is the only thing the renderer reads with.
/// </para>
///
/// <para>
/// The renderer is native and reads every one of its settings through
/// <c>getenv</c>, so on a Mac it silently received none of them: no log path,
/// so it wrote no log at all and there was nothing to read when it went wrong;
/// no <c>MOGHOUSE_NATIVE_DIR</c>, so it did not know where its own assets were.
/// Nothing failed loudly, which is what made it expensive - the client looked
/// like it had lost its renderer when in fact the renderer had lost its
/// configuration. Windows had the same fault and hid it: a development
/// checkout is run from the repository root, and the renderer's fallback of
/// looking under the current directory found the atlas there. A packaged
/// build keeps the assets in <c>data\</c>, where only the variable can point,
/// and so shipped with no HUD.
/// </para>
/// </summary>
public static class NativeEnvironment
{
    // Third argument is overwrite: 1 replaces an existing value, which matches
    // what the managed setter does.
    [DllImport("libc", EntryPoint = "setenv")]
    private static extern int SetEnvNative(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string value,
        int overwrite);

    [DllImport("libc", EntryPoint = "unsetenv")]
    private static extern int UnsetEnvNative([MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    // The universal C runtime, which the renderer links dynamically, so this
    // updates the very copy its getenv reads. The wide form so that a path
    // with anything outside the system code page survives the trip; the
    // runtime keeps its narrow copy in step. An empty value removes the
    // variable, which is what the managed setter does with null.
    [DllImport("ucrtbase", EntryPoint = "_wputenv_s", CharSet = CharSet.Unicode)]
    private static extern int PutEnvWindows(string name, string value);

    /// <summary>
    /// Sets it for managed and native code alike. A null or empty value clears
    /// it, matching <see cref="Environment.SetEnvironmentVariable(string, string)"/>.
    /// </summary>
    public static void Set(string name, string? value)
    {
        Environment.SetEnvironmentVariable(name, value);

        try
        {
            if (OperatingSystem.IsWindows())
            {
                PutEnvWindows(name, value ?? "");
            }
            else if (string.IsNullOrEmpty(value))
            {
                UnsetEnvNative(name);
            }
            else
            {
                SetEnvNative(name, value, 1);
            }
        }
        catch (Exception)
        {
            // A missing or renamed C runtime would be extraordinary, and the
            // managed value is set either way - so this is worth surviving
            // rather than taking the client down over.
        }
    }
}
