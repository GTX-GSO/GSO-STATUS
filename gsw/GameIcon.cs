using System.Diagnostics;
using System.Runtime.InteropServices;

namespace GsoStatus;

// A single cached 24x24 straight-alpha BGRA icon, matching LVGL ARGB8888.
static class GameIcon
{
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct FileInfo {public IntPtr Icon;public int Index;public uint Attributes;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=260)]public string Display;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=80)]public string Type;}
    [StructLayout(LayoutKind.Sequential)] struct BitmapInfo {public uint Size;public int Width,Height;public ushort Planes,Bits;public uint Compression,ImageSize;public int X,Y;public uint Used,Important;}
    [DllImport("shell32.dll",CharSet=CharSet.Unicode)]static extern IntPtr SHGetFileInfo(string path,uint attributes,out FileInfo info,uint size,uint flags);
    [DllImport("user32.dll")]static extern bool DestroyIcon(IntPtr icon);
    [DllImport("user32.dll")]static extern bool DrawIconEx(IntPtr dc,int x,int y,IntPtr icon,int width,int height,uint step,IntPtr brush,uint flags);
    [DllImport("gdi32.dll")]static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")]static extern bool DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll")]static extern IntPtr CreateDIBSection(IntPtr dc,ref BitmapInfo info,uint usage,out IntPtr bits,IntPtr section,uint offset);
    [DllImport("gdi32.dll")]static extern IntPtr SelectObject(IntPtr dc,IntPtr obj);
    [DllImport("gdi32.dll")]static extern bool DeleteObject(IntPtr obj);
    internal static string? Read(uint pid)
    {
        IntPtr icon=IntPtr.Zero,dc=IntPtr.Zero,bitmap=IntPtr.Zero,previous=IntPtr.Zero;
        try{
            using var process=Process.GetProcessById((int)pid);string? path=process.MainModule?.FileName;if(path is null)return null;
            if(SHGetFileInfo(path,0,out var file,(uint)Marshal.SizeOf<FileInfo>(),0x100)==IntPtr.Zero)return null;icon=file.Icon;
            dc=CreateCompatibleDC(IntPtr.Zero);if(dc==IntPtr.Zero)return null;
            var info=new BitmapInfo{Size=40,Width=24,Height=-24,Planes=1,Bits=32};bitmap=CreateDIBSection(dc,ref info,0,out var bits,IntPtr.Zero,0);if(bitmap==IntPtr.Zero)return null;previous=SelectObject(dc,bitmap);
            var black=new byte[2304];Marshal.Copy(black,0,bits,black.Length);if(!DrawIconEx(dc,0,0,icon,24,24,0,IntPtr.Zero,3))return null;Marshal.Copy(bits,black,0,black.Length);
            var white=Enumerable.Repeat((byte)255,2304).ToArray();Marshal.Copy(white,0,bits,white.Length);DrawIconEx(dc,0,0,icon,24,24,0,IntPtr.Zero,3);Marshal.Copy(bits,white,0,white.Length);
            for(int i=0;i<black.Length;i+=4){int alpha=255-Math.Clamp((int)white[i]-black[i],0,255);for(int k=0;k<3;k++)black[i+k]=(byte)(alpha==0?0:Math.Min(255,black[i+k]*255/alpha));black[i+3]=(byte)alpha;}
            return Convert.ToBase64String(black);
        }catch{return null;}
        finally{if(previous!=IntPtr.Zero)SelectObject(dc,previous);if(bitmap!=IntPtr.Zero)DeleteObject(bitmap);if(dc!=IntPtr.Zero)DeleteDC(dc);if(icon!=IntPtr.Zero)DestroyIcon(icon);}
    }
}
