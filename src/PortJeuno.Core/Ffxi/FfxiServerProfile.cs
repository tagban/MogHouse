using System.Text.Json.Serialization;
using PortJeuno.Core.Crypto;

namespace PortJeuno.Core.Ffxi;

/// <summary>
/// A saved FFXI server connection - the user can be logged into more than
/// one private server, so each gets its own profile rather than one global
/// login (mirrors Invigoration's HotlineServerProfile/BattlenetCredentialProfile
/// shape). See FfxiServerProfileStore.
/// </summary>
public sealed class FfxiServerProfile
{
    /// <summary>Stable identity, assigned once at creation - same pattern as Invigoration's profile Ids.</summary>
    public string Id { get; set; } = Guid.NewGuid().ToString("N");

    public string Name { get; set; } = "New Server";

    public string Host { get; set; } = "";

    public int AuthPort { get; set; } = FfxiConstants.AuthPort;

    public int DataPort { get; set; } = FfxiConstants.DataPort;

    public int ViewPort { get; set; } = FfxiConstants.ViewPort;

    public string Username { get; set; } = "";

    [JsonConverter(typeof(ObfuscatedPasswordJsonConverter))]
    public string Password { get; set; } = "";

    /// <summary>
    /// A prior LOGIN_ATTEMPT's trust_token (see FfxiAuthClient.LoginAsync's
    /// trustToken parameter) - once saved, lets a later login skip re-entering
    /// a TOTP code for accounts with 2FA enabled, same as real xiloader's own
    /// trust-this-computer behavior. Empty until the user opts in and a
    /// server that actually issues one is used.
    /// </summary>
    [JsonConverter(typeof(ObfuscatedPasswordJsonConverter))]
    public string TrustToken { get; set; } = "";

    /// <summary>Whether to request a new trust token on the next login that needs one - see FfxiAuthClient.LoginAsync's trustThisComputer parameter.</summary>
    public bool TrustThisComputer { get; set; }
}
