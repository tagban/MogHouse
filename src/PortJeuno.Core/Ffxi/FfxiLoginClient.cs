namespace PortJeuno.Core.Ffxi;

/// <summary>
/// Ties auth (TLS/JSON) and roster fetch (data+view TCP) together into one
/// login call. The returned <see cref="FfxiRosterClient"/> (null unless
/// login succeeded) stays open - the character-select handshake
/// (SelectCharacterAsync) reuses the same data/view sockets the roster came
/// from, matching the real protocol's single continuous session. Callers
/// own disposing it once they're done (whether or not they call
/// SelectCharacterAsync).
/// </summary>
public sealed class FfxiLoginClient
{
    public async Task<(FfxiLoginResponse Login, IReadOnlyList<FfxiCharacter> Characters, FfxiRosterClient? Roster)> LoginAsync(
        string host, string username, string password, string? otp = null,
        int authPort = FfxiConstants.AuthPort, int dataPort = FfxiConstants.DataPort, int viewPort = FfxiConstants.ViewPort,
        string? trustToken = null, bool trustThisComputer = false, CancellationToken ct = default)
    {
        using var auth = new FfxiAuthClient();
        await auth.ConnectAsync(host, authPort, ct);
        FfxiLoginResponse login = await auth.LoginAsync(username, password, otp, trustToken, trustThisComputer, ct);

        if (login.Result != FfxiLoginResult.Success || login.AccountId is null || login.SessionHash is null)
        {
            return (login, [], null);
        }

        var roster = new FfxiRosterClient();
        IReadOnlyList<FfxiCharacter> characters = await roster.FetchCharactersAsync(host, login.AccountId.Value, login.SessionHash, dataPort, viewPort, ct);
        return (login, characters, roster);
    }

    /// <summary>Convenience overload driven directly by a saved profile - see FfxiServerProfileStore.</summary>
    public Task<(FfxiLoginResponse Login, IReadOnlyList<FfxiCharacter> Characters, FfxiRosterClient? Roster)> LoginAsync(FfxiServerProfile profile, string? otp = null, CancellationToken ct = default) =>
        LoginAsync(profile.Host, profile.Username, profile.Password, otp, profile.AuthPort, profile.DataPort, profile.ViewPort, profile.TrustToken, profile.TrustThisComputer, ct);
}
