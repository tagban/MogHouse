using MogHouse.Core.Ffxi;
using MogHouse.Core.Interop;

namespace MogHouse.Core.Screens;

/// <summary>
/// Draws the client's own screens in the renderer, and waits for answers.
///
/// <para>
/// The screens before the world - signing in, choosing a character - used to be
/// a separate toolkit's windows put in front of the game. Drawing them in the
/// renderer instead means there is one window for the whole client, and the
/// world can carry on behind a screen rather than being replaced by it, the way
/// the retail launcher has always looked.
/// </para>
///
/// <para>
/// Every method here blocks, which is the point: a sign-in reads as the
/// sequence of steps it is rather than as a graph of callbacks. That is only
/// safe away from the thread running the renderer, so <b>this must not be used
/// from the main thread</b> - the loop that draws the screen and collects the
/// typing is the very thing being waited on, and waiting on it from inside it
/// hangs the client with the screen half-drawn.
/// </para>
/// </summary>
public sealed class ScreenHost(LiveRadar world)
{
    /// <summary>
    /// How often to look for an answer. The renderer collects the typing; this
    /// only notices that a button was pressed, so a frame's worth of delay is
    /// under what anyone can see.
    /// </summary>
    private static readonly TimeSpan Poll = TimeSpan.FromMilliseconds(16);

    /// <summary>Whether the player has closed the window, which ends everything.</summary>
    public bool Closed => world.Closed;

    /// <summary>
    /// Puts a screen up and waits for a button.
    ///
    /// Returns null if the window closes first - which is the player quitting,
    /// so every caller should treat it as "stop", not as "try again".
    /// </summary>
    public NativeFormResult? Ask(string title, string message, IReadOnlyList<NativeFormRow> rows)
    {
        world.ShowForm(title, message, rows);

        while (!world.Closed)
        {
            if (world.TakeFormResult() is { } result)
            {
                return result;
            }

            Thread.Sleep(Poll);
        }

        return null;
    }

    /// <summary>
    /// Says what is happening during something slow, with nothing to press.
    ///
    /// Left up until the next screen replaces it, so a caller sets one of these
    /// before a connection attempt and never has to take it down.
    /// </summary>
    public void Busy(string title, string message) =>
        world.ShowForm(title, message, []);

    /// <summary>
    /// Tells the player something and waits for them to acknowledge it.
    /// For a failure that ends a step, where carrying on silently would leave
    /// them looking at a screen that never changes.
    /// </summary>
    public void Tell(string title, string message) =>
        Ask(title, message, [NativeFormRow.Button("OK")]);

    /// <summary>Takes whatever is showing down, so the world is unobscured.</summary>
    public void Clear() => world.HideForm();
}
