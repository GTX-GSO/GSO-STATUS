using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Security.Principal;

namespace GsoStatus;

readonly record struct CpuSensorReading(double? Temperature,double? Power,string? Error,double? Clock=null,double? MemoryClock=null,long SampledTick=0);
sealed class CpuSensorFilter
{
    // Ryzen's hardware.Update can exceed 3s, especially while building. Retain
    // actual source timestamps; rereading a frame never extends its lifetime.
    internal const long MaxAgeMs=10000;
    double? temperature,power;long temperatureAt,powerAt;
    public CpuSensorReading Apply(CpuSensorReading input,long now){
        if(input.SampledTick>0&&now>=input.SampledTick&&now-input.SampledTick<=MaxAgeMs){
            if(input.Temperature is double t&&double.IsFinite(t)&&t>=-100&&t<=200){temperature=t;temperatureAt=input.SampledTick;}
            if(input.Power is double p&&double.IsFinite(p)&&p>=0&&p<=2000){power=p;powerAt=input.SampledTick;}
        }
        bool freshTemperature=temperatureAt>0&&now>=temperatureAt&&now-temperatureAt<=MaxAgeMs;
        bool freshPower=powerAt>0&&now>=powerAt&&now-powerAt<=MaxAgeMs;
        return input with{Temperature=freshTemperature?temperature:null,Power=freshPower?power:null,Error=freshTemperature&&freshPower?null:input.Error??"cpu_sensor_invalid_sample"};
    }
}

// A fixed shared-memory frame keeps privileged hardware I/O out of the normal collector.
sealed class CpuSensorClient : IDisposable
{
    internal const uint Signature=0x47534350;
    internal const string TaskName="GSO STATUS CPU Sensor";
    internal static string UserSid=>WindowsIdentity.GetCurrent().User!.Value;
    internal static string MappingName=>@"Local\GSOStatus.CpuSensor."+UserSid;
    internal static string StopName=>@"Local\GSOStatus.CpuSensor.Stop."+UserSid;
    MemoryMappedFile? map;
    MemoryMappedViewAccessor? view;
    MemoryMappedFile? clockMap;
    MemoryMappedViewAccessor? clockView;
    long lastLaunch=-30000;
    readonly CpuSensorFilter filter=new();
    public CpuSensorReading Sample()=>filter.Apply(Read(),Environment.TickCount64);
    CpuSensorReading Read()
    {
        try {
            if(view is null){map=MemoryMappedFile.CreateOrOpen(MappingName,64,MemoryMappedFileAccess.ReadWrite);view=map.CreateViewAccessor(0,64,MemoryMappedFileAccess.ReadWrite);}
            long now=Environment.TickCount64;view.Write(48,now);
            if(view.ReadUInt32(0)!=Signature||view.ReadUInt32(4)!=1){StartWorker();return new(null,null,"cpu_sensor_initializing");}
            long before=view.ReadInt64(8);Thread.MemoryBarrier();
            long sampled=view.ReadInt64(16);double temperature=view.ReadDouble(24),power=view.ReadDouble(32);uint flags=view.ReadUInt32(40);
            Thread.MemoryBarrier();long after=view.ReadInt64(8);
            var reading=Validate(before,after,now,sampled,flags,temperature,power);
            if(reading.Error is null)try{
                if(clockView is null){clockMap=MemoryMappedFile.OpenExisting(MappingName+".Clocks",MemoryMappedFileRights.Read);clockView=clockMap.CreateViewAccessor(0,64,MemoryMappedFileAccess.Read);}
                if(clockView.ReadUInt32(0)==Signature&&clockView.ReadUInt32(4)==1){long first=clockView.ReadInt64(8);Thread.MemoryBarrier();long stamp=clockView.ReadInt64(16);double core=clockView.ReadDouble(24),memory=clockView.ReadDouble(32);uint clockFlags=clockView.ReadUInt32(40);Thread.MemoryBarrier();long last=clockView.ReadInt64(8);
                    if(first>0&&first==last&&(first&1)==0&&stamp>0&&now>=stamp&&now-stamp<=CpuSensorFilter.MaxAgeMs)reading=reading with{Clock=(clockFlags&1)!=0&&double.IsFinite(core)&&core>0&&core<=20000?core:null,MemoryClock=(clockFlags&2)!=0&&double.IsFinite(memory)&&memory>0&&memory<=20000?memory:null};
                }
            }catch(Exception ex)when(ex is IOException or UnauthorizedAccessException){}
            if(reading.Error=="cpu_sensor_stale")StartWorker();
            return reading;
        }catch(FileNotFoundException){StartWorker();return new(null,null,"cpu_sensor_worker_unavailable");}
        catch(UnauthorizedAccessException){return new(null,null,"cpu_sensor_access_denied");}
        catch(IOException){Dispose();StartWorker();return new(null,null,"cpu_sensor_worker_unavailable");}
    }
    internal static CpuSensorReading Validate(long before,long after,long now,long sampled,uint flags,double temperature,double power)
    {
        if(before<=0||before!=after||(before&1)!=0)return new(null,null,"cpu_sensor_update_in_progress");
        if(sampled<=0||now<sampled||now-sampled>CpuSensorFilter.MaxAgeMs)return new(null,null,"cpu_sensor_stale");
        return new((flags&1)!=0&&double.IsFinite(temperature)&&temperature>=-100&&temperature<=200?temperature:null,
            (flags&2)!=0&&double.IsFinite(power)&&power>=0&&power<=2000?power:null,null,SampledTick:sampled);
    }
    void StartWorker()
    {
        long now=Environment.TickCount64;if(now-lastLaunch<30000)return;lastLaunch=now;
        try{using var process=Process.Start(new ProcessStartInfo(Path.Combine(Environment.SystemDirectory,"schtasks.exe")){ArgumentList={"/Run","/TN",TaskName},UseShellExecute=false,CreateNoWindow=true,WindowStyle=ProcessWindowStyle.Hidden});}catch{}
    }
    internal static void StopWorker(){try{using var stop=EventWaitHandle.OpenExisting(StopName);stop.Set();}catch(WaitHandleCannotBeOpenedException){}}
    public void Dispose(){clockView?.Dispose();clockView=null;clockMap?.Dispose();clockMap=null;view?.Dispose();view=null;map?.Dispose();map=null;}
}
