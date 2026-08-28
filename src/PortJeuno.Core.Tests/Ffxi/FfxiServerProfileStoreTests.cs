using PortJeuno.Core.Ffxi;

namespace PortJeuno.Core.Tests.Ffxi;

/// <summary>Redirects FfxiServerProfileStore to an isolated temp directory for this test collection - a hardcoded real-%AppData% path would otherwise leak permanent junk profiles into the user's actual config on every test run.</summary>
public sealed class FfxiServerProfileStoreFixture : IDisposable
{
    private readonly string _tempDir = Path.Combine(Path.GetTempPath(), $"portjeuno-test-ffxiprofiles-{Guid.NewGuid():N}");

    public FfxiServerProfileStoreFixture() => FfxiServerProfileStore.ConfigDirectoryOverride = _tempDir;

    public void Dispose()
    {
        FfxiServerProfileStore.ConfigDirectoryOverride = null;
        try
        {
            Directory.Delete(_tempDir, recursive: true);
        }
        catch (DirectoryNotFoundException)
        {
        }
    }
}

[CollectionDefinition("FfxiServerProfileStore")]
public class FfxiServerProfileStoreCollection : ICollectionFixture<FfxiServerProfileStoreFixture>;

[Collection("FfxiServerProfileStore")]
public class FfxiServerProfileStoreTests
{
    [Fact]
    public void CreateAndSave_AssignsIdAndPersists()
    {
        var profile = FfxiServerProfileStore.CreateAndSave("Test Server", "ffxi.example.com");

        Assert.False(string.IsNullOrEmpty(profile.Id));
        Assert.Equal("Test Server", profile.Name);
        Assert.Equal("ffxi.example.com", profile.Host);
        Assert.Contains(FfxiServerProfileStore.Profiles, p => p.Id == profile.Id);
    }

    [Fact]
    public void Find_UnknownId_ReturnsNull()
    {
        Assert.Null(FfxiServerProfileStore.Find($"nonexistent-{Guid.NewGuid():N}"));
    }

    [Fact]
    public void Delete_RemovesProfile()
    {
        var profile = FfxiServerProfileStore.CreateAndSave("To Delete", "host");

        FfxiServerProfileStore.Delete(profile.Id);

        Assert.DoesNotContain(FfxiServerProfileStore.Profiles, p => p.Id == profile.Id);
    }

    [Fact]
    public void Save_PersistsAcrossCacheReload()
    {
        var profile = FfxiServerProfileStore.CreateAndSave("Persisted", "host2");
        profile.Username = "tagban";
        FfxiServerProfileStore.Save();

        var reloaded = JsonReload();

        Assert.Contains(reloaded, p => p.Id == profile.Id && p.Username == "tagban");
    }

    [Fact]
    public void Save_ObfuscatesPasswordOnDiskButRoundTripsPlaintext()
    {
        var profile = FfxiServerProfileStore.CreateAndSave("WithPassword", "host3");
        profile.Password = "hunter2";
        FfxiServerProfileStore.Save();

        var rawJson = File.ReadAllText(FfxiServerProfileStore.FilePath);
        Assert.DoesNotContain("hunter2", rawJson);

        var reloaded = JsonReload();
        Assert.Equal("hunter2", reloaded.Single(p => p.Id == profile.Id).Password);
    }

    private static List<FfxiServerProfile> JsonReload()
    {
        var json = File.ReadAllText(FfxiServerProfileStore.FilePath);
        return System.Text.Json.JsonSerializer.Deserialize<List<FfxiServerProfile>>(json) ?? [];
    }

    [Fact]
    public void NewProfile_HasDefaultPorts()
    {
        var profile = new FfxiServerProfile();

        Assert.Equal(FfxiConstants.AuthPort, profile.AuthPort);
        Assert.Equal(FfxiConstants.DataPort, profile.DataPort);
        Assert.Equal(FfxiConstants.ViewPort, profile.ViewPort);
    }
}
