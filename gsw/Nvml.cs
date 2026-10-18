using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;

namespace GsoStatus;

sealed class Nvml : IDisposable
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Init();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int DeviceByIndex(uint index, out IntPtr device);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetName(IntPtr device, StringBuilder name, uint size);
    [StructLayout(LayoutKind.Sequential)] struct Util { public uint Gpu, Memory; }
    [StructLayout(LayoutKind.Sequential)] struct Mem { public ulong Total, Free, Used; }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetUtil(IntPtr device, out Util value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetMem(IntPtr device, out Mem value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetTemp(IntPtr device, uint sensor, out uint value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetPower(IntPtr device, out uint value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetClock(IntPtr device,uint type,out uint value);
    IntPtr library, device;
    bool initialized;
    T Function<T>(string name) where T : Delegate => Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(library, name));
    public Nvml(int index)
    {
        foreach (var path in new[] { Path.Combine(Environment.SystemDirectory,"nvml.dll"), Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),@"NVIDIA Corporation\NVSMI\nvml.dll") })
            if (NativeLibrary.TryLoad(path, out library)) break;
        if (library == IntPtr.Zero) return;
        try
        {
            initialized = Function<Init>("nvmlInit_v2")() == 0;
            if (initialized) Function<DeviceByIndex>("nvmlDeviceGetHandleByIndex_v2")((uint)index, out device);
        }
        catch { device=IntPtr.Zero; }
    }
    public JsonObject Sample()
    {
        var h = Model.Hardware(); if (device == IntPtr.Zero) return h;
        var name = new StringBuilder(160);
        if (Function<GetName>("nvmlDeviceGetName")(device,name,160)==0) h["model"]=name.ToString();
        if (Function<GetUtil>("nvmlDeviceGetUtilizationRates")(device,out var util)==0) h["load_percent"]=Model.Metric(util.Gpu);
        if (Function<GetMem>("nvmlDeviceGetMemoryInfo")(device,out var mem)==0) { h["memory_used_bytes"]=Model.Bytes(mem.Used); h["memory_total_bytes"]=Model.Bytes(mem.Total); }
        if (Function<GetTemp>("nvmlDeviceGetTemperature")(device,0,out var temp)==0) h["temperature_c"]=Model.Metric(temp);
        if (Function<GetPower>("nvmlDeviceGetPowerUsage")(device,out var power)==0) h["power_w"]=Model.Metric(power/1000.0);
        try {var clock=Function<GetClock>("nvmlDeviceGetClockInfo");if(clock(device,0,out var core)==0&&core>0)h["clock_mhz"]=Model.Metric(core);if(clock(device,2,out var memory)==0&&memory>0)h["memory_clock_mhz"]=Model.Metric(memory);}catch{}
        return h;
    }
    public void Dispose()
    {
        if (library == IntPtr.Zero) return;
        try { if (initialized) Function<Init>("nvmlShutdown")(); } catch { }
        NativeLibrary.Free(library); library=IntPtr.Zero;
    }
}
