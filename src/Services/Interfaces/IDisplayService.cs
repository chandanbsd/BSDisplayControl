namespace BSDDisplayControl.Services.Interfaces
{
    using Avalonia.Controls;
    using System;
    using System.Threading.Tasks;

    /// <summary>
    /// Interface for display services.
    /// </summary>
    public interface IDisplayService
    {
        public Task<string> GetDisplayInfo();
    }
}