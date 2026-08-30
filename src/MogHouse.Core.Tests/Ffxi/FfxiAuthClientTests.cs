using System.Text;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiAuthClientTests
{
    [Fact]
    public void ParseResponse_SuccessReply_DecodesAccountAndSessionHash()
    {
        string json = """{"result":1,"account_id":42,"session_hash":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]}""";

        FfxiLoginResponse response = FfxiAuthClient.ParseResponse(Encoding.UTF8.GetBytes(json));

        Assert.Equal(FfxiLoginResult.Success, response.Result);
        Assert.Equal(42u, response.AccountId);
        Assert.Equal(Enumerable.Range(1, 16).Select(i => (byte)i), response.SessionHash);
        Assert.Null(response.ErrorMessage);
    }

    [Fact]
    public void ParseResponse_FailureReply_HasNoAccountOrSessionHash()
    {
        string json = """{"result":0}""";

        FfxiLoginResponse response = FfxiAuthClient.ParseResponse(Encoding.UTF8.GetBytes(json));

        Assert.Equal(FfxiLoginResult.Fail, response.Result);
        Assert.Null(response.AccountId);
        Assert.Null(response.SessionHash);
    }

    [Fact]
    public void ParseResponse_VersionRejection_SurfacesErrorMessageOnly()
    {
        string json = """{"error_message":"Your xiloader is too old."}""";

        FfxiLoginResponse response = FfxiAuthClient.ParseResponse(Encoding.UTF8.GetBytes(json));

        Assert.Equal("Your xiloader is too old.", response.ErrorMessage);
        Assert.Null(response.AccountId);
    }

    [Fact]
    public void ParseResponse_TrustToken_IsSurfacedWhenPresent()
    {
        string json = """{"result":1,"account_id":1,"session_hash":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"trust_token":"abc123"}""";

        FfxiLoginResponse response = FfxiAuthClient.ParseResponse(Encoding.UTF8.GetBytes(json));

        Assert.Equal("abc123", response.TrustToken);
    }
}
