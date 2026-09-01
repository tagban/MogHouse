using System;
using Avalonia.Controls;
using MogHouse.App.ViewModels;

namespace MogHouse.App.Views;

public partial class MainWindow : Window
{
    private GameViewModel? _watching;

    public MainWindow()
    {
        InitializeComponent();

        // A folder picker belongs to a window, and a view model has none - so
        // the one screen that needs to ask for a folder borrows this one's.
        DataContextChanged += (_, _) =>
        {
            if (DataContext is MainViewModel shell)
            {
                shell.StorageProvider = StorageProvider;
                shell.PropertyChanged += (_, changed) =>
                {
                    if (changed.PropertyName == nameof(MainViewModel.CurrentPage))
                    {
                        Follow(shell.CurrentPage as GameViewModel);
                    }
                };
                Follow(shell.CurrentPage as GameViewModel);
            }
        };
    }

    /// <summary>
    /// Steps aside while the world is up, and comes back when it closes.
    ///
    /// The world is a native window of its own rather than a control inside
    /// this one, so without this the player gets two windows the moment they
    /// pick a character: the game in front, and the launcher behind it. Going
    /// from the character list into the world should look like this window
    /// becoming the game.
    /// </summary>
    private void Follow(GameViewModel? game)
    {
        if (ReferenceEquals(_watching, game))
        {
            return;
        }
        if (_watching is not null)
        {
            _watching.WorldVisibilityChanged -= OnWorldVisibilityChanged;
        }
        _watching = game;
        if (_watching is not null)
        {
            _watching.WorldVisibilityChanged += OnWorldVisibilityChanged;
        }
        else
        {
            Show();
        }
    }

    private void OnWorldVisibilityChanged(bool worldIsUp)
    {
        if (worldIsUp)
        {
            Hide();
        }
        else
        {
            Show();
            Activate();
        }
    }
}
