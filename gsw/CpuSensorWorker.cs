using System.IO.MemoryMappedFiles;
using System.Security.Principal;
using LibreHardwareMonitor.Hardware;

namespace GsoStatus;

static class CpuSensorWorker
{
    internal static int Run()
    {
        if(!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))return 2;
        using var mutex=new Mutex(true,CpuSensorClient.MappingName+".Worker",out var owner);if(!owner)return 0;
        using var stop=new EventWaitHandle(false,EventResetMode.ManualReset,CpuSensorClient.StopName);stop.Reset();
        using var map=MemoryMappedFile.CreateOrOpen(CpuSensorClient.MappingName,64,MemoryMappedFileAccess.ReadWrite);
        using var view=map.CreateViewAccessor(0,64,MemoryMappedFileAccess.ReadWrite);
        using var clockMap=MemoryMappedFile.CreateOrOpen(CpuSensorClient.MappingName+".Clocks",64,MemoryMappedFileAccess.ReadWrite);
        using var clockView=clockMap.CreateViewAccessor(0,64,MemoryMappedFileAccess.ReadWrite);
        var computer=new Computer{IsCpuEnabled=true,IsGpuEnabled=false,IsMotherboardEnabled=false,IsMemoryEnabled=false,IsStorageEnabled=false,IsNetworkEnabled=false,IsControllerEnabled=false,IsPowerMonitorEnabled=false};
        try{
            computer.Open();long started=Environment.TickCount64,sequence=0;
            view.Write(0,CpuSensorClient.Signature);view.Write(4,1u);
            clockView.Write(0,CpuSensorClient.Signature);clockView.Write(4,1u);
            do{
                long now=Environment.TickCount64,heartbeat=view.ReadInt64(48);
                if(now-started>60000&&(heartbeat<=0||now-heartbeat>60000))break;
                double? temperature=null,power=null,clock=null,memoryClock=null;
                try{
                    foreach(var hardware in computer.Hardware.Where(h=>h.HardwareType==HardwareType.Cpu)){
                        hardware.Update();
                        temperature=Pick(hardware.Sensors,SensorType.Temperature,"Core (Tctl/Tdie)","CPU Package","Package","Core (Tdie)");
                        power=Pick(hardware.Sensors,SensorType.Power,"Package","CPU Package");
                        var clocks=hardware.Sensors.Where(s=>s.SensorType==SensorType.Clock&&s.Name.StartsWith("Core #",StringComparison.OrdinalIgnoreCase)&&s.Value is >0).Select(s=>(double)s.Value!.Value).ToArray();
                        if(clocks.Length>0)clock=clocks.Average();memoryClock=Pick(hardware.Sensors,SensorType.Clock,"Memory","Memory Clock","DRAM");break;
                    }
                }catch{ /* Missing readings remain null; no estimates or control writes. */ }
                view.Write(8,++sequence);Thread.MemoryBarrier();
                view.Write(16,Environment.TickCount64);view.Write(24,temperature??double.NaN);view.Write(32,power??double.NaN);
                view.Write(40,(temperature.HasValue?1u:0u)|(power.HasValue?2u:0u));Thread.MemoryBarrier();view.Write(8,++sequence);
                clockView.Write(8,sequence-1);Thread.MemoryBarrier();clockView.Write(16,Environment.TickCount64);clockView.Write(24,clock??double.NaN);clockView.Write(32,memoryClock??double.NaN);clockView.Write(40,(clock.HasValue?1u:0u)|(memoryClock.HasValue?2u:0u));Thread.MemoryBarrier();clockView.Write(8,sequence);
            }while(!stop.WaitOne(1000));
            return 0;
        }catch{return 3;}
        finally{view.Write(16,0L);clockView.Write(16,0L);computer.Close();}
    }
    static double? Pick(IEnumerable<ISensor> sensors,SensorType type,params string[] names)
    {
        foreach(var name in names){var sensor=sensors.FirstOrDefault(s=>s.SensorType==type&&s.Name.Equals(name,StringComparison.OrdinalIgnoreCase));if(sensor?.Value is float value&&float.IsFinite(value))return value;}return null;
    }
}
