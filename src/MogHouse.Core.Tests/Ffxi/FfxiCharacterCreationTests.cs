using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiCharacterCreationTests
{
    [Theory]
    [InlineData("Testy")]
    [InlineData("Duo")]                 // the shortest a name may be
    [InlineData("Abcdefghijklmno")]     // and the longest, at fifteen
    public void AllowsOrdinaryNames(string name)
    {
        Assert.Null(FfxiCharacterCreation.WhyNameIsInvalid(name));
    }

    [Theory]
    [InlineData("Do")]
    [InlineData("Abcdefghijklmnop")]     // sixteen
    public void RejectsNamesOfTheWrongLength(string name)
    {
        Assert.Equal("A name is between 3 and 15 characters.",
                     FfxiCharacterCreation.WhyNameIsInvalid(name));
    }

    // These are the rules for *making* a name, and nothing more. A character
    // that already has a space in its name - which a GM command on a private
    // server will happily produce - logs in perfectly well on the retail
    // client, tells to it aside, so nothing on the way into the world should
    // consult this and decide such a character is broken.
    [Theory]
    [InlineData("Donald Trump")]
    [InlineData("Testy2")]
    [InlineData("O'Brien")]
    public void RejectsNamesThatAreNotLetters(string name)
    {
        Assert.Equal("A name is letters only - no spaces, digits or punctuation.",
                     FfxiCharacterCreation.WhyNameIsInvalid(name));
    }
}
