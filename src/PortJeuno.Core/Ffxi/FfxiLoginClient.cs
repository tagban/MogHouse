namespace PortJeuno.Core.Ffxi;

/// <summary>Ties auth (TLS/JSON) and roster fetch (data+view TCP) together into one login call.</summary>
public sealed class FfxiLoginClient
{
    public async Task<(FfxiLoginResponse Login, IReadOnlyList<FfxiCharacter> Characters)> LoginAsync(
        string host, string username, string password, string? otp = null, CancellationToken ct = default)
    {
        using var auth = new FfxiAuthClient();
        await auth.ConnectAsync(host, ct: ct);
        FfxiLoginResponse login = await auth.LoginAsync(username, password, otp, ct: ct);

        if (login.Result != FfxiLoginResult.Success || login.AccountId is null || login.SessionHash is null)
        {
            return (login, []);
        }

        using var roster = new FfxiRosterClient();
        IReadOnlyList<FfxiCharacter> characters = await roster.FetchCharactersAsync(host, login.AccountId.Value, login.SessionHash, ct);
        return (login, characters);
    }
}
