using System.Threading.Tasks;
using BSDDisplayControl.Services.Interfaces;

namespace BSDisplayControl.ViewModels;

public partial class MainWindowViewModel : ViewModelBase
{
    private IDisplayService _displayService;



    public string Greeting { get; } = "Welcome to Avalonia!";

    private string _commandOutput;
    public string CommandOutput
    {
        get => _commandOutput;
        set => SetProperty(ref _commandOutput, value); // Assumes ViewModelBase implements SetProperty
    }

    public MainWindowViewModel(
        IDisplayService displayService
    )
    {
        _displayService = displayService;
        LoadCommandOutputAsync();
    }

    public async void LoadCommandOutputAsync()
    {
        CommandOutput = await _displayService.GetDisplayInfo();
    }
}
