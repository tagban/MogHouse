using System.Net.Security;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace PortJeuno.Core.Ffxi;

/// <summary>Result of an auth_session login attempt.</summary>
public sealed record FfxiLoginResponse(
    FfxiLoginResult Result,
    uint? AccountId,
    byte[]? SessionHash,
    string? TrustToken,
    string? ErrorMessage);

/// <summary>
/// TLS+JSON handshake against a LandSandBoat auth_session (port 54231).
/// Request/response JSON shape and the major.minor version gate are read
/// directly from LandSandBoat/server's src/login/auth_session.cpp (branch
/// `base`, 2026-08-28), not guessed. NOT live-tested against a real server
/// from this codebase - see native/portjeuno_interop/README.md's sibling
/// note in project memory for why that matters here.
/// </summary>
public sealed class FfxiAuthClient : IDisposable
{
    private readonly TcpClient _tcp = new();
    private SslStream? _ssl;

    /// <summary>
    /// Connects and completes the TLS handshake. LandSandBoat private
    /// servers overwhelmingly use self-signed certificates, so this accepts
    /// any certificate the way private-server loaders commonly do - tighten
    /// this if a specific server enforces a real CA chain.
    /// </summary>
    public async Task ConnectAsync(string host, int port = FfxiConstants.AuthPort, CancellationToken ct = default)
    {
        await _tcp.ConnectAsync(host, port, ct);
        _ssl = new SslStream(_tcp.GetStream(), leaveInnerStreamOpen: false, (_, _, _, _) => true);

        // The single-string-argument AuthenticateAsClientAsync overload has
        // no CancellationToken parameter at all - a hung/slow handshake
        // would ignore a caller's timeout entirely. Use the options-based
        // overload instead so ConnectAsync's ct actually has teeth here.
        await _ssl.AuthenticateAsClientAsync(new SslClientAuthenticationOptions { TargetHost = host }, ct);
    }

    public Task<FfxiLoginResponse> LoginAsync(string username, string password, string? otp = null, string? trustToken = null, bool trustThisComputer = false, CancellationToken ct = default) =>
        SendAsync(BuildRequest(FfxiLoginCommand.Attempt, username, password, otp, trustToken, trustThisComputer), ct);

    /// <summary>
    /// LOGIN_CREATE (auth_session.cpp): same request shape as a login
    /// attempt, gated on settings/login.lua's ACCOUNT_CREATION. Success
    /// only returns {"result": SuccessCreate} - no account_id/session_hash
    /// (see sendLoginResult vs. the richer hand-built JSON on the
    /// LOGIN_ATTEMPT success path) - call LoginAsync separately afterward
    /// to actually get a session.
    /// </summary>
    public Task<FfxiLoginResponse> CreateAccountAsync(string username, string password, CancellationToken ct = default) =>
        SendAsync(BuildRequest(FfxiLoginCommand.Create, username, password), ct);

    private static JsonObject BuildRequest(FfxiLoginCommand command, string username, string password, string? otp = null, string? trustToken = null, bool trustThisComputer = false) =>
        new()
        {
            ["command"] = (int)command,
            ["username"] = username,
            ["password"] = password,
            ["otp"] = otp ?? string.Empty,
            ["trust_token"] = trustToken ?? string.Empty,
            ["trust_this_computer"] = trustThisComputer,
            ["version"] = new JsonArray(FfxiConstants.SupportedXiloaderVersion[0], FfxiConstants.SupportedXiloaderVersion[1], FfxiConstants.SupportedXiloaderVersion[2]),
        };

    private async Task<FfxiLoginResponse> SendAsync(JsonObject request, CancellationToken ct)
    {
        if (_ssl is null)
        {
            throw new InvalidOperationException("Call ConnectAsync first.");
        }

        byte[] payload = Encoding.UTF8.GetBytes(request.ToJsonString());
        await _ssl.WriteAsync(payload, ct);

        // auth_session::do_write writes exactly the reply's own length (no
        // padding), and these JSON replies are small - a single read is
        // assumed sufficient rather than looping until a full parse
        // succeeds. Revisit if real-world testing shows replies arriving
        // split across multiple reads.
        var buffer = new byte[4096];
        int read = await _ssl.ReadAsync(buffer, ct);
        return ParseResponse(buffer.AsSpan(0, read));
    }

    internal static FfxiLoginResponse ParseResponse(ReadOnlySpan<byte> json)
    {
        using var doc = JsonDocument.Parse(json.ToArray());
        JsonElement root = doc.RootElement;

        FfxiLoginResult result = root.TryGetProperty("result", out JsonElement resultEl)
            ? (FfxiLoginResult)resultEl.GetByte()
            : FfxiLoginResult.Error;

        uint? accountId = root.TryGetProperty("account_id", out JsonElement accId) ? accId.GetUInt32() : null;

        byte[]? sessionHash = null;
        if (root.TryGetProperty("session_hash", out JsonElement hashEl) && hashEl.ValueKind == JsonValueKind.Array)
        {
            sessionHash = hashEl.EnumerateArray().Select(e => (byte)e.GetInt32()).ToArray();
        }

        string? trustToken = root.TryGetProperty("trust_token", out JsonElement tt) ? tt.GetString() : null;
        string? errorMessage = root.TryGetProperty("error_message", out JsonElement em) ? em.GetString() : null;

        return new FfxiLoginResponse(result, accountId, sessionHash, trustToken, errorMessage);
    }

    public void Dispose()
    {
        _ssl?.Dispose();
        _tcp.Dispose();
    }
}
