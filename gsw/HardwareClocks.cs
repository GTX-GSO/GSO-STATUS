using System.Management;
using System.Runtime.InteropServices;

namespace GsoStatus;

static class HardwareClocks
{
    [StructLayout(LayoutKind.Sequential)] struct ProcessorPower { public uint Number,MaxMhz,CurrentMhz,Limit,MaxIdle,CurrentIdle; }
    [DllImport("powrprof.dll")] static extern uint CallNtPowerInformation(int level,IntPtr input,uint inputSize,[Out] ProcessorPower[] output,uint outputSize);
    static int standardDdr;
    public static double? ConfiguredDdrMhz=>MemoryRate is {} rate&&Volatile.Read(ref standardDdr)==1?rate/2:null;
    static readonly Task<double?> memoryRate=Task.Run(ReadMemoryRate);
    public static double? MemoryRate=>memoryRate.IsCompletedSuccessfully?memoryRate.Result:null;
    public static double? CpuMhz()
    {
        try {
            var values=new ProcessorPower[Environment.ProcessorCount];
            if(CallNtPowerInformation(11,IntPtr.Zero,0,values,(uint)(values.Length*Marshal.SizeOf<ProcessorPower>()))!=0)return null;
            var clocks=values.Where(v=>v.CurrentMhz is >0 and <=20000).Select(v=>(double)v.CurrentMhz).ToArray();
            return clocks.Length==values.Length?clocks.Average():null;
        }catch{return null;}
    }
    static double? ReadMemoryRate()
    {
        try {
            using var query=new ManagementObjectSearcher("SELECT ConfiguredClockSpeed,SMBIOSMemoryType FROM Win32_PhysicalMemory");
            using var rows=query.Get();var rates=new List<double>();bool ddr=true;
            foreach(ManagementObject row in rows)using(row){var type=Convert.ToUInt32(row["SMBIOSMemoryType"]);var rate=Convert.ToUInt32(row["ConfiguredClockSpeed"]);if(type is not (24 or 26 or 30 or 34 or 35)||rate is 0 or >20000)return null;if(type is not (24 or 26 or 34))ddr=false;rates.Add(rate);}
            // A mixed configuration has no single defensible transfer rate.
            if(rates.Count>0&&rates.Distinct().Count()==1){if(ddr)Volatile.Write(ref standardDdr,1);return rates[0];}return null;
        }catch{return null;}
    }
}
