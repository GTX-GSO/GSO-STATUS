using System.ComponentModel;
using System.Runtime.InteropServices;

namespace GsoStatus;

// A hidden window owns the notification icon and receives Explorer restart broadcasts.
sealed class NativeTray
{
    const uint Callback=0x8001;
    readonly CancellationTokenSource stop;
    readonly Task task;
    readonly Func<string> status;
    readonly Action open;
    readonly WindowProc procedure;
    readonly string className="GSOStatusTray."+Guid.NewGuid().ToString("N");
    IntPtr window;
    bool iconAdded;
    uint explorerRestart;
    NativeTray(CancellationTokenSource stop,Task task,Func<string> status,Action open)
    {this.stop=stop;this.task=task;this.status=status;this.open=open;procedure=Dispatch;}
    public static void Run(CancellationTokenSource stop,Task task,Func<string> status,Action open)
        =>new NativeTray(stop,task,status,open).Loop();
    public static void ShowError(string message)=>MessageBoxW(IntPtr.Zero,message,"GSO STATUS WIN",0x10);
    void Loop()
    {
        var instance=GetModuleHandleW(null);
        var wc=new WindowClass {Procedure=Marshal.GetFunctionPointerForDelegate(procedure),Instance=instance,Name=className};
        if(RegisterClassW(ref wc)==0)throw new Win32Exception();
        try
        {
            explorerRestart=RegisterWindowMessageW("TaskbarCreated");
            window=CreateWindowExW(0,className,"GSO STATUS WIN",0,0,0,0,0,IntPtr.Zero,IntPtr.Zero,instance,IntPtr.Zero);
            if(window==IntPtr.Zero)throw new Win32Exception();
            UpdateIcon();
            if(SetTimer(window,1,1000,IntPtr.Zero)==0)throw new Win32Exception();
            int result;
            while((result=GetMessageW(out var message,IntPtr.Zero,0,0))>0){TranslateMessage(ref message);DispatchMessageW(ref message);}
            if(result<0)throw new Win32Exception();
        }
        finally
        {
            stop.Cancel();
            if(window!=IntPtr.Zero)DestroyWindow(window);
            UnregisterClassW(className,instance);
            GC.KeepAlive(procedure);
        }
    }
    void UpdateIcon()
    {
        var tip="GSW · "+status();
        var data=Data();data.Flags=7;data.Callback=Callback;data.Icon=LoadIconW(IntPtr.Zero,(IntPtr)32512);data.Tip=tip[..Math.Min(127,tip.Length)];
        iconAdded=Shell_NotifyIconW(iconAdded?1u:0u,ref data);
    }
    IconData Data()=>new(){Size=(uint)Marshal.SizeOf<IconData>(),Window=window,Id=1,Tip="",Info="",Title=""};
    IntPtr Dispatch(IntPtr hwnd,uint message,UIntPtr wParam,IntPtr lParam)
    {
        try
        {
            if(message==explorerRestart&&explorerRestart!=0){iconAdded=false;UpdateIcon();return IntPtr.Zero;}
            switch(message)
            {
                case 0x113:
                    if(task.IsCompleted||stop.IsCancellationRequested)DestroyWindow(hwnd);else UpdateIcon();
                    return IntPtr.Zero;
                case Callback:
                    if(lParam.ToInt64() is 0x205 or 0x203)ShowMenu(hwnd);
                    return IntPtr.Zero;
                case 0x10:stop.Cancel();DestroyWindow(hwnd);return IntPtr.Zero;
                case 2:
                    stop.Cancel();KillTimer(hwnd,1);var data=Data();Shell_NotifyIconW(2,ref data);iconAdded=false;window=IntPtr.Zero;PostQuitMessage(0);return IntPtr.Zero;
            }
        }
        catch {stop.Cancel();PostMessageW(hwnd,0x10,UIntPtr.Zero,IntPtr.Zero);}
        return DefWindowProcW(hwnd,message,wParam,lParam);
    }
    void ShowMenu(IntPtr hwnd)
    {
        var menu=CreatePopupMenu();if(menu==IntPtr.Zero)return;
        try
        {
            AppendMenuW(menu,1,100,status());AppendMenuW(menu,0,101,"打开配置");AppendMenuW(menu,0,102,"退出");
            GetCursorPos(out var point);SetForegroundWindow(hwnd);
            var command=TrackPopupMenuEx(menu,0x102,point.X,point.Y,hwnd,IntPtr.Zero);
            PostMessageW(hwnd,0,UIntPtr.Zero,IntPtr.Zero);
            if(command==101)open();else if(command==102)PostMessageW(hwnd,0x10,UIntPtr.Zero,IntPtr.Zero);
        }
        finally {DestroyMenu(menu);}
    }
    delegate IntPtr WindowProc(IntPtr hwnd,uint message,UIntPtr wParam,IntPtr lParam);
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct WindowClass
    {public uint Style;public IntPtr Procedure;public int ClassExtra,WindowExtra;public IntPtr Instance,Icon,Cursor,Background;public string? Menu;public string Name;}
    [StructLayout(LayoutKind.Sequential)] struct Point {public int X,Y;}
    [StructLayout(LayoutKind.Sequential)] struct Message {public IntPtr Window;public uint Id;public UIntPtr WParam;public IntPtr LParam;public uint Time;public Point Point;public uint Private;}
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct IconData
    {
        public uint Size;public IntPtr Window;public uint Id,Flags,Callback;public IntPtr Icon;
        [MarshalAs(UnmanagedType.ByValTStr,SizeConst=128)]public string Tip;
        public uint State,StateMask;
        [MarshalAs(UnmanagedType.ByValTStr,SizeConst=256)]public string Info;
        public uint Timeout;
        [MarshalAs(UnmanagedType.ByValTStr,SizeConst=64)]public string Title;
        public uint InfoFlags;public Guid Guid;public IntPtr BalloonIcon;
    }
    [DllImport("kernel32",CharSet=CharSet.Unicode)]static extern IntPtr GetModuleHandleW(string? name);
    [DllImport("user32",CharSet=CharSet.Unicode,SetLastError=true)]static extern ushort RegisterClassW(ref WindowClass wc);
    [DllImport("user32",CharSet=CharSet.Unicode)]static extern bool UnregisterClassW(string name,IntPtr instance);
    [DllImport("user32",CharSet=CharSet.Unicode,SetLastError=true)]static extern IntPtr CreateWindowExW(uint ex,string cls,string title,uint style,int x,int y,int width,int height,IntPtr parent,IntPtr menu,IntPtr instance,IntPtr param);
    [DllImport("user32")]static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32",SetLastError=true)]static extern int GetMessageW(out Message message,IntPtr hwnd,uint min,uint max);
    [DllImport("user32")]static extern bool TranslateMessage(ref Message message);
    [DllImport("user32")]static extern IntPtr DispatchMessageW(ref Message message);
    [DllImport("user32")]static extern IntPtr DefWindowProcW(IntPtr hwnd,uint message,UIntPtr wp,IntPtr lp);
    [DllImport("user32")]static extern void PostQuitMessage(int code);
    [DllImport("user32")]static extern bool PostMessageW(IntPtr hwnd,uint message,UIntPtr wp,IntPtr lp);
    [DllImport("user32",SetLastError=true)]static extern UIntPtr SetTimer(IntPtr hwnd,UIntPtr id,uint interval,IntPtr callback);
    [DllImport("user32")]static extern bool KillTimer(IntPtr hwnd,UIntPtr id);
    [DllImport("user32",CharSet=CharSet.Unicode)]static extern uint RegisterWindowMessageW(string name);
    [DllImport("user32",CharSet=CharSet.Unicode)]static extern IntPtr LoadIconW(IntPtr instance,IntPtr name);
    [DllImport("shell32",CharSet=CharSet.Unicode)]static extern bool Shell_NotifyIconW(uint operation,ref IconData data);
    [DllImport("user32")]static extern IntPtr CreatePopupMenu();
    [DllImport("user32",CharSet=CharSet.Unicode)]static extern bool AppendMenuW(IntPtr menu,uint flags,UIntPtr id,string text);
    [DllImport("user32")]static extern bool DestroyMenu(IntPtr menu);
    [DllImport("user32")]static extern bool GetCursorPos(out Point point);
    [DllImport("user32")]static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32")]static extern uint TrackPopupMenuEx(IntPtr menu,uint flags,int x,int y,IntPtr hwnd,IntPtr parameters);
    [DllImport("user32",CharSet=CharSet.Unicode)]static extern int MessageBoxW(IntPtr hwnd,string text,string title,uint flags);
}
