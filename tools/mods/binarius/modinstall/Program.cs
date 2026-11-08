// modinstall <launch file> <game folder> [--choose "<prompt text>=<option>"]... [--report <file.json>] [--keys <underground2.txt>]
//
// What Binary / Binarius do in User Mode, without their window: read a mod's launch
// file (install.end, *.endlauncher: "[VERSN1]" + JSON naming the endscript and the
// game files it edits), load those files from the game folder, run the script, answer
// its questions, save.  The launcher's Mods tab (launcher/Mods.swift) runs it on a
// clone of the game folder; tools/mods/ul_install.sh runs it by hand.
//
// Questions: a combobox or checkbox is answered from --choose (matched against the
// question text, case-insensitive), otherwise with the script's first option / "yes".
// When the script then reports a failed lookup in the game data ("Collection named X
// does not exist") from inside the branch of an option nobody
// chose, the other options of that question are tried, and the run with the fewest
// errors is kept.  (Carbon 4K's "Default" names a texture pack the stock game doesn't
// have, its "Unlimiter" option the one it does.)  Errors outside a chosen branch
// don't cause retries.  Every question, its options, the answer and whether it was
// picked automatically go to --report, with the remaining errors.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using Endscript.Commands;
using Endscript.Core;
using Endscript.Enums;
using Endscript.Profiles;

static class Program
{
    sealed class Prompt
    {
        public string Kind { get; set; }           // "combobox" | "checkbox"
        public string Description { get; set; }
        public string[] Options { get; set; }
        public string Choice { get; set; }
        public bool Auto { get; set; }             // nobody asked for this answer
        internal int From = -1, To = -1;           // command indices of the chosen branch
    }
    sealed class Error { public string File { get; set; } public int Line { get; set; } public string Command { get; set; } public string Message { get; set; } internal int At = -1; }
    sealed class Run { public List<Prompt> Prompts = new(); public List<Error> Errors = new(); public BaseProfile Profile; }
    sealed class Report
    {
        public string Launch { get; set; }
        public bool Saved { get; set; }
        public int Attempts { get; set; }
        public string Failure { get; set; }
        public List<Prompt> Prompts { get; set; } = new();
        public List<Error> Errors { get; set; } = new();
    }

    static Launch launch;
    static BaseCommand[] commands;
    static string endscript;

    static int Main(string[] args)
    {
        // As Binarius does (Program.cs): en-US.  With the user's locale (Turkish, say)
        // ToUpper turns "i" into "İ" (other hashes) and "1.5" stops being a number.
        var culture = CultureInfo.CreateSpecificCulture("en-US");
        Thread.CurrentThread.CurrentCulture = culture;
        Thread.CurrentThread.CurrentUICulture = culture;
        CultureInfo.DefaultThreadCurrentCulture = culture;
        CultureInfo.DefaultThreadCurrentUICulture = culture;

        if (args.Length == 3 && args[0] == "--decompress")
        {   // test: unpack one file with the managed decompressor
            var o = Nikki.Utils.Interop.Decompress(File.ReadAllBytes(args[1]));
            File.WriteAllBytes(args[2], o); Console.WriteLine($"{o.Length} bytes"); return 0;
        }
        var pos = new List<string>();
        var choose = new List<(string, string)>();
        string keys = null, reportPath = null;
        for (int i = 0; i < args.Length; ++i)
        {
            if (args[i] == "--choose" && i + 1 < args.Length)
            {
                var kv = args[++i]; int eq = kv.LastIndexOf('=');
                if (eq > 0) choose.Add((kv[..eq], kv[(eq + 1)..]));
            }
            else if (args[i] == "--keys" && i + 1 < args.Length) keys = args[++i];
            else if (args[i] == "--report" && i + 1 < args.Length) reportPath = args[++i];
            else pos.Add(args[i]);
        }
        if (pos.Count != 2)
        {
            Console.Error.WriteLine("usage: modinstall <launch file> <game folder> [--choose \"prompt=option\"]... [--report file.json] [--keys file]");
            return 2;
        }
        var report = new Report { Launch = Path.GetFileName(pos[0]) };
        int rc;
        try { rc = Install(Path.GetFullPath(pos[0]), Path.GetFullPath(pos[1]), choose, keys, report); }
        catch (Exception ex) { report.Failure = ex.Message; Console.Error.WriteLine("failed: " + ex); rc = 1; }
        if (reportPath != null)
            File.WriteAllText(reportPath, JsonSerializer.Serialize(report,
                new JsonSerializerOptions { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase }));
        return rc;
    }

    static int Install(string launchPath, string gameDir, List<(string, string)> choose, string keys, Report report)
    {
        Endscript.Version.Value = new System.Version(2, 10, 4);
        var tmp = Path.Combine(Path.GetTempPath(), "modinstall-keys"); Directory.CreateDirectory(tmp);
        string Touch(string n) { var p = Path.Combine(tmp, n); if (!File.Exists(p)) File.WriteAllText(p, ""); return p; }
        Underground2Profile.MainHashList = keys ?? Touch("underground2.txt");
        Underground2Profile.CustomHashList = Touch("user_underground2.txt");
        Nikki.Core.Map.CustomAttribFile = Touch("CustomAttributes.txt");

        Launch.Deserialize(launchPath, out launch);
        if (launch.UsageID != eUsage.User) throw new Exception($"the launch file is for {launch.Usage} mode, not User");
        if (launch.GameID != Nikki.Core.GameINT.Underground2) throw new Exception($"the mod is for {launch.Game}, not Need for Speed Underground 2");
        launch.Directory = gameDir;
        launch.ThisDir = Path.GetDirectoryName(launchPath);
        launch.CheckEndscript();
        launch.CheckFiles();
        launch.LoadLinks();

        endscript = Path.Combine(launch.ThisDir, launch.Endscript);
        var parser = new EndScriptParser(endscript);
        try { commands = parser.Read(); }
        catch (Exception ex) { throw new Exception($"{Path.GetFileName(parser.CurrentFile)} line {parser.CurrentIndex}: {ex.Message}"); }
        Console.WriteLine($"{commands.Length} commands");

        // answers the tool picked after a failed run, by question text
        var forced = new Dictionary<string, string>();
        var run = Attempt(choose, forced);
        int attempts = 1;
        var tried = new HashSet<string>();
        // Only a lookup in the game's data that fails ("Collection named X does not
        // exist") says an option doesn't fit this game; other errors (a file of the mod
        // missing, say) don't make another option the right one.
        static int Misfits(Run r) => r.Errors.Count(e => e.Message.StartsWith("Collection named "));
        while (Misfits(run) > 0 && attempts < 12)
        {
            // a question answered automatically whose chosen branch holds such a lookup
            var q = run.Prompts.FirstOrDefault(p => p.Auto && p.Kind == "combobox" && !tried.Contains(p.Description)
                                               && run.Errors.Any(e => e.At >= p.From && e.At < p.To && e.Message.StartsWith("Collection named ")));
            if (q == null) break;
            tried.Add(q.Description);
            foreach (var o in q.Options.Where(o => o != q.Choice))
            {
                var f2 = new Dictionary<string, string>(forced) { [q.Description] = o };
                var r2 = Attempt(choose, f2); attempts++;
                Console.WriteLine($"retry: {q.Description} -> {o}: {Misfits(r2)} failed lookups (was {Misfits(run)}), {r2.Errors.Count} errors");
                if (Misfits(r2) < Misfits(run) && r2.Errors.Count <= run.Errors.Count) { forced = f2; run = r2; break; }
            }
        }
        report.Attempts = attempts;
        report.Prompts = run.Prompts;
        report.Errors = run.Errors;
        foreach (var p in run.Prompts) Console.WriteLine($"{p.Kind}: {p.Description} -> {p.Choice}{(p.Auto ? "" : " (chosen)")}");
        Console.WriteLine($"{run.Errors.Count} errors after {attempts} run(s)");
        foreach (var e in run.Errors) Console.WriteLine($"  error: {e.File}:{e.Line} [{e.Command}] {e.Message}");

        var sw = System.Diagnostics.Stopwatch.StartNew();
        var saveErr = run.Profile.Save();
        foreach (var e in saveErr) Console.Error.WriteLine("save: " + e);
        Console.WriteLine($"saved in {sw.Elapsed.TotalSeconds:F1} s");
        if (saveErr.Length > 0) { report.Failure = "save: " + string.Join("; ", saveErr); return 1; }
        report.Saved = true;
        return 0;
    }

    /// One run of the script on freshly loaded files; nothing is saved.
    static Run Attempt(List<(string, string)> choose, Dictionary<string, string> forced)
    {
        var run = new Run();
        run.Profile = BaseProfile.NewProfile(launch.GameID, launch.Directory);
        var loadErr = run.Profile.Load(launch);
        if (loadErr.Length > 0) throw new Exception("load: " + string.Join("; ", loadErr));

        string Pick(string desc, string[] options, out bool auto)
        {
            auto = false;
            foreach (var (k, v) in choose)
                if (desc.Contains(k, StringComparison.OrdinalIgnoreCase))
                {
                    var o = options.FirstOrDefault(x => x.Equals(v, StringComparison.OrdinalIgnoreCase))
                         ?? options.FirstOrDefault(x => x.StartsWith(v, StringComparison.OrdinalIgnoreCase));
                    if (o != null) return o;
                    throw new Exception($"\"{desc}\" has no option \"{v}\" (options: {string.Join(", ", options)})");
                }
            auto = true;
            return forced.TryGetValue(desc, out var f) ? f : null;
        }

        var manager = new EndScriptManager(run.Profile, commands, endscript);
        manager.CommandChase();
        while (!manager.ProcessScript())
        {
            var command = manager.CurrentCommand;
            if (command is InfoboxCommand info) Console.WriteLine("info: " + info.Description);
            else if (command is CheckboxCommand cb)
            {
                var p = Pick(cb.Description, new[] { "yes", "no" }, out bool auto) ?? "yes";
                cb.Choice = p == "yes" ? 1 : 0;
                run.Prompts.Add(new Prompt { Kind = "checkbox", Description = cb.Description, Options = new[] { "yes", "no" }, Choice = p, Auto = auto });
            }
            else if (command is ComboboxCommand co)
            {
                var names = co.Options.Select(o => o.Name).ToArray();
                var p = Pick(co.Description, names, out bool auto) ?? names[0];
                co.Choice = Array.IndexOf(names, p);
                // the chosen branch: from its option to the next option (or the end)
                int from = co.Options[co.Choice].Start;
                int to = co.Choice + 1 < co.Options.Length ? co.Options[co.Choice + 1].Start : co.LastCommand;
                run.Prompts.Add(new Prompt { Kind = "combobox", Description = co.Description, Options = names, Choice = p, Auto = auto, From = from, To = to });
            }
        }
        foreach (var e in manager.Errors)
        {
            // the failing command's place in the command list (errors carry file and line)
            int at = Array.FindIndex(commands, c => c.Filename == e.Filename && c.Index == e.Index);
            run.Errors.Add(new Error { File = Path.GetFileName(e.Filename), Line = e.Index, Command = e.Line, Message = e.Error, At = at });
        }
        return run;
    }
}
