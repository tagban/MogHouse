using MogHouse.Core.Crypto;

namespace MogHouse.Core.Tests.Crypto;

public class PasswordObfuscatorTests
{
    [Theory]
    [InlineData("hunter2")]
    [InlineData("")]
    [InlineData("p@ss w0rd with spaces!")]
    public void Wrap_ThenUnwrap_RoundTrips(string password)
    {
        var wrapped = PasswordObfuscator.Wrap(password);

        Assert.Equal(password, PasswordObfuscator.Unwrap(wrapped));
    }

    [Fact]
    public void Wrap_NonEmptyPassword_IsBracketedAndNotPlaintext()
    {
        var wrapped = PasswordObfuscator.Wrap("hunter2");

        Assert.StartsWith("[", wrapped);
        Assert.EndsWith("]", wrapped);
        Assert.DoesNotContain("hunter2", wrapped);
    }

    [Fact]
    public void Unwrap_PlaintextWithoutBrackets_PassesThroughUnchanged()
    {
        // A password typed directly into the profile JSON by hand should work immediately.
        Assert.Equal("mynewpassword", PasswordObfuscator.Unwrap("mynewpassword"));
    }
}
