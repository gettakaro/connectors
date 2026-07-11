namespace Takaro.Valheim.Companion;

internal sealed class CompanionClientBridge : IDisposable
{
    private readonly Action<string> log;
    private bool initialized;
    private bool disposed;

    public CompanionClientBridge(Action<string>? log = null)
    {
        this.log = log ?? (_ => { });
    }

    public void Initialize()
    {
        if (disposed || initialized)
        {
            return;
        }

        initialized = true;
        log($"Takaro Valheim Companion initialized for protocol {TakaroCompanionBuildVersion.ProtocolVersion}.");
    }

    public void Update()
    {
        if (!initialized || disposed)
        {
            return;
        }
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        initialized = false;
    }
}
