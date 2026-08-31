using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using MogHouse.Core.Ffxi;

namespace MogHouse.App.ViewModels;

/// <summary>
/// Making a character, with the character standing there while you make them.
///
/// The preview is the real renderer on zone 49 - a flat lit plane and nothing
/// else, which is what that zone is for - so what you see being built is what
/// will exist. Every change reopens it, because a look is chosen when the
/// window opens rather than swapped inside it.
/// </summary>
public partial class CreateCharacterViewModel : ViewModelBase, IDisposable
{
    private readonly MainViewModel _shell;
    private readonly byte[] _sessionHash;
    private readonly string _host;

    /// <summary>The stage. Flat, empty, and not a place anyone can go.</summary>
    private const int PreviewZone = 49;

    public CreateCharacterViewModel(MainViewModel shell, byte[] sessionHash, string host)
    {
        _shell = shell;
        _sessionHash = sessionHash;
        _host = host;

        Races = new ObservableCollection<NamedValue>(new[]
        {
            new NamedValue("Hume, male", (byte)FfxiRaceId.HumeMale),
            new NamedValue("Hume, female", (byte)FfxiRaceId.HumeFemale),
            new NamedValue("Elvaan, male", (byte)FfxiRaceId.ElvaanMale),
            new NamedValue("Elvaan, female", (byte)FfxiRaceId.ElvaanFemale),
            new NamedValue("Tarutaru, male", (byte)FfxiRaceId.TarutaruMale),
            new NamedValue("Tarutaru, female", (byte)FfxiRaceId.TarutaruFemale),
            new NamedValue("Mithra", (byte)FfxiRaceId.Mithra),
            new NamedValue("Galka", (byte)FfxiRaceId.Galka),
        });

        Jobs = new ObservableCollection<NamedValue>(new[]
        {
            new NamedValue("Warrior", (byte)FfxiStartingJob.Warrior),
            new NamedValue("Monk", (byte)FfxiStartingJob.Monk),
            new NamedValue("White Mage", (byte)FfxiStartingJob.WhiteMage),
            new NamedValue("Black Mage", (byte)FfxiStartingJob.BlackMage),
            new NamedValue("Red Mage", (byte)FfxiStartingJob.RedMage),
            new NamedValue("Thief", (byte)FfxiStartingJob.Thief),
        });

        Nations = new ObservableCollection<NamedValue>(new[]
        {
            new NamedValue("San d'Oria", (byte)FfxiNation.SanDOria),
            new NamedValue("Bastok", (byte)FfxiNation.Bastok),
            new NamedValue("Windurst", (byte)FfxiNation.Windurst),
        });

        Sizes = new ObservableCollection<NamedValue>(new[]
        {
            new NamedValue("Small", (byte)FfxiBodySize.Small),
            new NamedValue("Medium", (byte)FfxiBodySize.Medium),
            new NamedValue("Large", (byte)FfxiBodySize.Large),
        });

        SelectedRace = Races[0];
        SelectedJob = Jobs[0];
        SelectedNation = Nations[0];
        SelectedSize = Sizes[1];

        ShowPreview();
    }

    /// <summary>A thing with a name for the list and a number for the wire.</summary>
    public sealed record NamedValue(string Name, byte Value);

    public ObservableCollection<NamedValue> Races { get; }
    public ObservableCollection<NamedValue> Jobs { get; }
    public ObservableCollection<NamedValue> Nations { get; }
    public ObservableCollection<NamedValue> Sizes { get; }

    [ObservableProperty]
    public partial string Name { get; set; } = "";

    [ObservableProperty]
    public partial NamedValue? SelectedRace { get; set; }

    [ObservableProperty]
    public partial NamedValue? SelectedJob { get; set; }

    [ObservableProperty]
    public partial NamedValue? SelectedNation { get; set; }

    [ObservableProperty]
    public partial NamedValue? SelectedSize { get; set; }

    /// <summary>Face and hair colour together, which is how the game stores it.</summary>
    [ObservableProperty]
    public partial int Face { get; set; }

    [ObservableProperty]
    public partial string? Error { get; set; }

    [ObservableProperty]
    public partial bool IsBusy { get; set; }

    private LiveRadar? _preview;

    partial void OnSelectedRaceChanged(NamedValue? value) => ShowPreview();

    partial void OnFaceChanged(int value) => ShowPreview();

    /// <summary>
    /// Stands the character on the empty zone so they can be looked at. The
    /// window is opened again for each change rather than altered in place -
    /// a look is settled when a character is built, not after.
    /// </summary>
    private void ShowPreview()
    {
        if (SelectedRace is null)
        {
            return;
        }

        _preview?.Dispose();

        byte face = (byte)Math.Clamp(Face, 0, 15);
        _preview = LiveRadar.Open(PreviewZone, 0, 0, 0, 0, Name.Length > 0 ? Name : "?",
                                  FfxiAppearance.LookString(SelectedRace.Value, face));

        if (_preview is null)
        {
            Error = "Could not open the preview window.";
        }
    }

    [RelayCommand]
    private void Back()
    {
        Dispose();
        _shell.Navigate(new LoginViewModel(_shell));
    }

    [RelayCommand]
    private async Task CreateAsync()
    {
        Error = null;

        // What can be known without asking is checked here, because the server
        // answers every bad name with the same code and explains none of them.
        if (FfxiCharacterCreation.WhyNameIsInvalid(Name) is { } wrong)
        {
            Error = wrong;
            return;
        }

        if (SelectedRace is null || SelectedJob is null || SelectedNation is null || SelectedSize is null)
        {
            Error = "Choose a race, a job, a nation and a size.";
            return;
        }

        IsBusy = true;
        try
        {
            var wanted = new FfxiNewCharacter(Name, (FfxiRaceId)SelectedRace.Value, (byte)Math.Clamp(Face, 0, 15),
                                              (FfxiBodySize)SelectedSize.Value, (FfxiStartingJob)SelectedJob.Value,
                                              (FfxiNation)SelectedNation.Value);

            string? refused = await _shell.Session.CreateCharacterAsync(wanted, _sessionHash);
            if (refused is not null)
            {
                Error = refused;
                return;
            }

            Dispose();
            _shell.Status = $"Created {Name}.";
            _shell.Navigate(new LoginViewModel(_shell));
        }
        catch (Exception ex)
        {
            Error = ex.Message;
        }
        finally
        {
            IsBusy = false;
        }
    }

    public void Dispose()
    {
        _preview?.Dispose();
        _preview = null;
    }
}
