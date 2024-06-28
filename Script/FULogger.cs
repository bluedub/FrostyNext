using Frosty.Sdk.Interfaces;

namespace ManagedFrostyNext;

internal class FULogger : ILogger
{
    private static readonly string s_info = "INFO";
    private static readonly string s_warn = "WARN";
    private static readonly string s_error = "ERROR";

    public void LogInfo(string message)
    {
        Console.WriteLine($"{s_info} - {message}");
    }

    public void LogWarning(string message)
    {
        Console.WriteLine($"{s_warn} - {message}");
    }

    public void LogError(string message)
    {
        Console.WriteLine($"{s_error} - {message}");
    }

    internal static void LogErrorInternal(string message)
    {
        Console.WriteLine($"{s_error} - {message}");
    }

    public void LogProgress(double progress)
    {
    }
}