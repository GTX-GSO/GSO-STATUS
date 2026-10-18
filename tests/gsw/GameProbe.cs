using GsoStatus;
using System.Diagnostics;
using System.Text.Json;

static class GameProbe
{
    internal static async Task Run()
    {
        var columns=new[]{"Application","ProcessID","TimeInQPC","MsRenderPresentLatency"};
        bool Parse(string row)=>PresentMon.TryFrame(columns,row,123,"game.exe",10000000,1000000,out _);
        if(!Parse("game.exe,123,9999000,0.25")||Parse("game.exe,124,9999000,0.25")||Parse("other.exe,123,9999000,0.25")||Parse("game.exe,123,6000000,0.25")||Parse("game.exe,123,10000001,0.25")||Parse("game.exe,123,9999000,0")||Parse("game.exe,123,9999000,NaN")||Parse("game.exe,123,9999000,NA"))throw new Exception("frame validation failed");
        using var fixture=Process.Start(new ProcessStartInfo(Path.GetFullPath("work/GSO-PresentFixture.exe")){UseShellExecute=false,CreateNoWindow=true})!;
        using var capture=new PresentMon(Path.GetFullPath("tools/presentmon/PresentMon-2.6.0-x64.exe"));
        int valid=0;double minimum=double.MaxValue,maximum=0;string? missing=null;var clock=Stopwatch.StartNew();
        string? icon=GameIcon.Read((uint)fixture.Id);var iconBytes=icon is null?Array.Empty<byte>():Convert.FromBase64String(icon);bool iconOk=icon?.Length==3072&&iconBytes.Length==2304&&Enumerable.Range(0,576).Any(i=>iconBytes[i*4+3]>0);
        try{
            while(clock.Elapsed.TotalSeconds<7&&!fixture.HasExited){
                var sample=capture.Sample((uint)fixture.Id,"GSO-PresentFixture.exe",true);
                if(sample["value"] is not null){double latency=sample["value"]!.GetValue<double>();valid++;minimum=Math.Min(minimum,latency);maximum=Math.Max(maximum,latency);}else missing=sample["reason"]?.GetValue<string>();
                await Task.Delay(100);
            }
            capture.Sample(0,null,false);
            await fixture.WaitForExitAsync();
            var report=new{scope="Production PresentMon adapter and native executable icon on controlled D3D11 fixture; no game and no raw capture retained",passed=valid>0&&iconOk,valid_samples=valid,min_ms=valid>0?(double?)minimum:null,max_ms=valid>0?(double?)maximum:null,icon_valid=iconOk,last_missing_reason=missing,actual_game_verified=false};
            string json=JsonSerializer.Serialize(report,new JsonSerializerOptions{WriteIndented=true});File.WriteAllText("artifacts/verification/game-adapter-0.1.10.json",json+"\n");Console.WriteLine(json);
            if(!report.passed)throw new Exception("production adapter probe failed");
        }finally{if(!fixture.HasExited){fixture.Kill();await fixture.WaitForExitAsync();}}
    }
}
