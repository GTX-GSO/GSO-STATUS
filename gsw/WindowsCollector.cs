using System.Runtime.InteropServices;
using Microsoft.Win32;
using System.Text.Json.Nodes;

namespace GsoStatus;

sealed class WindowsCollector : IDisposable
{
    [StructLayout(LayoutKind.Sequential)] struct Memory
    {
        public uint Length, Load;
        public ulong TotalPhysical, AvailablePhysical, TotalPage, AvailablePage, TotalVirtual, AvailableVirtual, Extended;
    }
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetSystemTimes(out ulong idle, out ulong kernel, out ulong user);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GlobalMemoryStatusEx(ref Memory status);
    ulong idle, kernel, user;
    bool primed;
    readonly string? cpuName;
    readonly Nvml gpu;
    readonly CpuSensorClient cpuSensors=new();
    readonly string[] drives;
    readonly Dictionary<string,(DateTimeOffset at,string? name)> diskModels=new();
    readonly DiskPerf diskPerf=new();
    public WindowsCollector(int gpuIndex, string[] drives)
    {
        this.drives = drives;
        using var key = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0");
        cpuName = (key?.GetValue("ProcessorNameString") as string)?.Trim();
        gpu = new Nvml(gpuIndex);
    }
    public JsonObject Sample()
    {
        var cpu = Model.Hardware(cpuName);
        cpu["clock_mhz"]=Model.Metric(HardwareClocks.CpuMhz(),"cpu_clock_unavailable");
        cpu["memory_rate_mts"]=Model.Metric(HardwareClocks.MemoryRate,"ram_configured_rate_unavailable");
        var sensors=cpuSensors.Sample();
        if(sensors.Clock.HasValue)cpu["clock_mhz"]=Model.Metric(sensors.Clock);
        var ramClock=sensors.MemoryClock??HardwareClocks.ConfiguredDdrMhz;
        cpu["memory_clock_mhz"]=Model.Metric(ramClock,"ram_clock_sensor_unavailable");
        cpu["memory_clock_source"]=ramClock.HasValue?(sensors.MemoryClock.HasValue?"sensor":"configured_ddr"):null;
        cpu["temperature_c"]=Model.Metric(sensors.Temperature,sensors.Error??"cpu_temperature_sensor_unavailable");
        cpu["power_w"]=Model.Metric(sensors.Power,sensors.Error??"cpu_package_power_unavailable");
        if (GetSystemTimes(out var i, out var k, out var u))
        {
            if (primed && k >= kernel && u >= user && i >= idle)
            {
                var total = (k-kernel)+(u-user); var inactive = i-idle;
                cpu["load_percent"] = total > 0 && inactive <= total ? Model.Metric(100.0*(total-inactive)/total) : Model.Missing("cpu_interval_empty");
            }
            else cpu["load_percent"] = Model.Missing("cpu_warming_up");
            idle=i; kernel=k; user=u; primed=true;
        }
        else cpu["load_percent"] = Model.Missing("cpu_api_failed");
        var memory = new Memory { Length = (uint)Marshal.SizeOf<Memory>() };
        if (GlobalMemoryStatusEx(ref memory))
        {
            cpu["memory_used_bytes"] = Model.Bytes(memory.TotalPhysical-memory.AvailablePhysical);
            cpu["memory_total_bytes"] = Model.Bytes(memory.TotalPhysical);
        }
        var disks = new JsonArray();
        foreach (var path in drives.Take(3))
        {
            if(!diskModels.TryGetValue(path,out var cached)||DateTimeOffset.UtcNow-cached.at>TimeSpan.FromSeconds(60)) {cached=(DateTimeOffset.UtcNow,DiskModel.Read(path));diskModels[path]=cached;}
            var perf = diskPerf.Sample(path);
            var disk = new JsonObject { ["id"] = path, ["model"] = cached.name, ["total_bytes"] = Model.Missing("disk_unavailable"), ["free_bytes"] = Model.Missing("disk_unavailable"),
                ["busy_percent"] = perf.Busy, ["read_ms"] = perf.Read, ["write_ms"] = perf.Write,
                ["response_ms"] = perf.Response, ["read_bytes_per_second"] = perf.ReadRate, ["write_bytes_per_second"] = perf.WriteRate };
            try { var drive = new DriveInfo(path); if (drive.IsReady) { disk["total_bytes"] = Model.Bytes((ulong)drive.TotalSize); disk["free_bytes"] = Model.Bytes((ulong)drive.TotalFreeSpace); } }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException) { }
            disks.Add(disk);
        }
        JsonObject graphics;
        try { graphics = gpu.Sample(); }
        catch { graphics = Model.Hardware(); }
        return new JsonObject { ["source"] = Model.Source("ok"), ["cpu"] = cpu, ["gpu"] = graphics, ["disks"] = disks };
    }
    public void Dispose(){cpuSensors.Dispose();gpu.Dispose();diskPerf.Dispose();}
}
