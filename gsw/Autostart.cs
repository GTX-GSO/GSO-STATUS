using Microsoft.Win32;

namespace GsoStatus;

// Start at sign-in through the current user's Run key: GSW runs asInvoker, so no task or elevation is needed, and the entry
// shows up (and can be switched off) on Task Manager's Startup apps page. Switching it off there leaves the Run value in
// place and marks it disabled under StartupApproved; that counts as off here, and switching it on here clears the mark.
sealed class Autostart(string runKey=Autostart.RunKey,string approvedKey=Autostart.ApprovedKey,string valueName=Autostart.ValueName)
{
    internal const string RunKey=@"Software\Microsoft\Windows\CurrentVersion\Run";
    internal const string ApprovedKey=@"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run";
    internal const string ValueName="GSO STATUS WIN";
    internal enum State {Off,On,OtherCopy}
    internal static string Command(string executable,string config)=>"\""+executable+"\" --config \""+config+"\"";
    /// <summary>On only when the entry starts this executable with this configuration and Task Manager has not disabled it.</summary>
    internal State Read(string executable,string config)
    {
        using var run=Registry.CurrentUser.OpenSubKey(runKey);
        if(run?.GetValue(valueName) is not string command)return State.Off;
        using var approved=Registry.CurrentUser.OpenSubKey(approvedKey);
        if(approved?.GetValue(valueName) is byte[] flags&&flags.Length>0&&(flags[0]&1)!=0)return State.Off;
        return string.Equals(command,Command(executable,config),StringComparison.OrdinalIgnoreCase)?State.On:State.OtherCopy;
    }
    internal void Enable(string executable,string config)
    {
        if(!Path.IsPathFullyQualified(executable)||!Path.IsPathFullyQualified(config)||executable.Contains('"')||config.Contains('"'))throw new ArgumentException("autostart paths must be full paths");
        using(var run=Registry.CurrentUser.CreateSubKey(runKey))run.SetValue(valueName,Command(executable,config),RegistryValueKind.String);
        using var approved=Registry.CurrentUser.OpenSubKey(approvedKey,true);
        approved?.DeleteValue(valueName,false);
    }
    internal void Disable()
    {
        using(var run=Registry.CurrentUser.OpenSubKey(runKey,true))run?.DeleteValue(valueName,false);
        using var approved=Registry.CurrentUser.OpenSubKey(approvedKey,true);
        approved?.DeleteValue(valueName,false);
    }
}
