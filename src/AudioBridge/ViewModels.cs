namespace AudioBridge;

using System.ComponentModel;
using System.Runtime.CompilerServices;

internal sealed class AudioDeviceView
{
    public required int Index { get; init; }
    public required string Id { get; init; }
    public required string Name { get; init; }
}

internal sealed class RecentTargetView
{
    public required string Path { get; init; }
    public required DateTimeOffset LastOpenedUtc { get; init; }

    public override string ToString()
    {
        return Path;
    }
}

internal sealed class AudioPidView : INotifyPropertyChanged
{
    private string format = string.Empty;
    private string lastPcm = string.Empty;
    private bool isSelected;

    public event PropertyChangedEventHandler? PropertyChanged;

    public required uint Pid { get; init; }

    public required string Format
    {
        get => format;
        init => format = value;
    }

    public required string LastPcm
    {
        get => lastPcm;
        init => lastPcm = value;
    }

    public required bool IsSelected
    {
        get => isSelected;
        init => isSelected = value;
    }

    public string SelectedText => IsSelected ? "Selected" : "";

    public void Update(string nextFormat, string nextLastPcm, bool nextIsSelected)
    {
        SetProperty(ref format, nextFormat, nameof(Format));
        SetProperty(ref lastPcm, nextLastPcm, nameof(LastPcm));
        if (SetProperty(ref isSelected, nextIsSelected, nameof(IsSelected)))
        {
            OnPropertyChanged(nameof(SelectedText));
        }
    }

    private bool SetProperty<T>(ref T field, T value, string propertyName)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }

        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
