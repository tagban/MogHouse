using System.Runtime.InteropServices;

namespace MogHouse.Core.Interop;

/// <summary>
/// Sets an environment variable so that native code in this process can also
/// see it.
///
/// <para>
/// <see cref="Environment.SetEnvironmentVariable(string, string)"/> is not
/// enough on its own. On Windows it calls the Win32 setter, so the change lands
/// in the real process environment and a <c>getenv</c> in native code returns
/// it. On Unix - macOS and Linux both - .NET keeps its own managed copy and
/// never touches the C library's environment, so native <c>getenv</c> returns
/// null for anything set this way.
/// </para>
///
/// <para>
/// The renderer is native and reads every one of its settings through
/// <c>getenv</c>, so on a Mac it silently received none of them: no log path,
/// so it wrote no log at all and there was nothing to read when it went wrong;
/// no <c>MOGHOUSE_NATIVE_DIR</c>, so it did not know where its own assets were.
/// Nothing failed loudly, which is what made it expensive - the client looked
/// like it had lost its renderer when in fact the renderer had lost its
/// configuration.
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

    /// <summary>
    /// Sets it for managed and native code alike. A null or empty value clears
    /// it, matching <see cref="Environment.SetEnvironmentVariable(string, string)"/>.
    /// </summary>
    public static void Set(string name, string? value)
    {
        Environment.SetEnvironmentVariable(name, value);

        if (OperatingSystem.IsWindows())
        {
            // Already went to the real environment.
            return;
        }

        try
        {
            if (string.IsNullOrEmpty(value))
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
            // A missing or renamed libc would be extraordinary, and the managed
            // value is set either way - so this is worth surviving rather than
            // taking the client down over.
        }
    }
}
