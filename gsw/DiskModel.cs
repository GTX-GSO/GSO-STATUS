using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using System.Text;

namespace GsoStatus;

static class DiskModel
{
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFile(string name,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle handle,uint code,byte[]? input,uint inputSize,byte[] output,uint outputSize,out uint returned,IntPtr overlap);
    public static string? Read(string mount)
    {
        try
        {
            var letter=Path.GetPathRoot(mount)?.TrimEnd('\\');if(letter is null||letter.Length!=2)return null;
            using var volume=CreateFile(@"\\.\"+letter,0,3,IntPtr.Zero,3,0,IntPtr.Zero);
            if(volume.IsInvalid)return null;
            var extents=new byte[1024];
            if(!DeviceIoControl(volume,0x00560000,null,0,extents,(uint)extents.Length,out var size,IntPtr.Zero)||size<32||BitConverter.ToUInt32(extents,0)!=1)return null;
            var disk=BitConverter.ToUInt32(extents,8);
            using var physical=CreateFile(@"\\.\PhysicalDrive"+disk,0,3,IntPtr.Zero,3,0,IntPtr.Zero);
            if(physical.IsInvalid)return null;
            var data=new byte[4096];
            if(!DeviceIoControl(physical,0x002D1400,new byte[12],12,data,(uint)data.Length,out size,IntPtr.Zero)||size<36)return null;
            string Part(int offset){var index=BitConverter.ToUInt32(data,offset);if(index==0||index>=size)return "";int end=(int)index;while(end<size&&data[end]!=0)end++;return Encoding.ASCII.GetString(data,(int)index,end-(int)index).Trim();}
            var name=(Part(12)+" "+Part(16)).Trim();return name.Length==0?null:name[..Math.Min(name.Length,160)];
        }
        catch { return null; }
    }
}
