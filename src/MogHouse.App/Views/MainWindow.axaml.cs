using Avalonia.Controls;
using MogHouse.App.ViewModels;

namespace MogHouse.App.Views;

public partial class MainWindow : Window
{
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
            }
        };
    }
}
