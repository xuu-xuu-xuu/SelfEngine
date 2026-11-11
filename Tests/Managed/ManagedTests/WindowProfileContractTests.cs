using System.Text.Json;
using SelfEngine;

internal static class WindowProfileContractTests
{
    private static void Check(bool value,string text){if(!value)throw new InvalidOperationException(text);}
    public static void Run(bool window)
    {
        using var session=new EngineSession();session.SetWindowProfile(null);
        var root=Path.Combine(AppContext.BaseDirectory,"Validation","T68",Guid.NewGuid().ToString("N"));
        bool rejected=false;try{session.SetWindowProfile(root,1,720);}catch(EngineException e){rejected=e.Key=="Bridge.InvalidArgument";}
        Check(rejected&&!Directory.Exists(root),"invalid profile has no output");
        session.SetWindowProfile(root,320,240);
        session.Update+=_=>{bool busy=false;try{session.SetWindowProfile(null);}catch(EngineException e){busy=e.Key=="Bridge.SessionBusy";}Check(busy,"profile immutable in callback");};
        session.Step(.01);Check(!Directory.Exists(root),"config only has no output");
        if(!window){session.SetWindowProfile(null);return;}
        session.RunWindow("en-US",false,4);
        using var report=JsonDocument.Parse(File.ReadAllText(Path.Combine(root,"window-profile.json")));
        Check(report.RootElement.GetProperty("success").GetBoolean()&&report.RootElement.GetProperty("rendered_frames").GetInt32()==4,"profile real frames");
        foreach(var name in new[]{"cpu.csv","gpu.csv","frames.csv"})Check(File.ReadAllLines(Path.Combine(root,name)).Length>1,"profile nonempty "+name);
        string sentinel=Path.Combine(root,"sentinel.txt");File.WriteAllText(sentinel,"preserve");
        rejected=false;try{session.RunWindow("en-US",false,1);}catch(EngineException e){rejected=e.Key=="Bridge.ProfileOutputExists";}
        Check(rejected&&File.ReadAllText(sentinel)=="preserve","reusing profile cannot overwrite");
        session.SetWindowProfile(null);session.RunWindow("en-US",false,1);
        Console.WriteLine("WindowProfileContractTests passed.");
    }
}
