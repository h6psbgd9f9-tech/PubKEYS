using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace PubKEYS.Pult
{
    public sealed class PultSettings
    {
        public string Mode = "random", PubkeyPath = "pubkey.txt", FoundPath = "RESULT/Found.txt", MaskHex = "", WaveStart = "";
        public int RangeStart = 0, RangeEnd = 256, ZoneNibbles = 10, WaveNibbles = 4, HybridNibbles = 10, ChunkBits = 22;
        public int Power = 100, Freeze = 40, DetHex = -1, ProgressSeconds = 36000, CheckSeconds = 36000, Threads = 1;
        public bool UseMask, WantNk = true, Debug, TgProgress, SignalProgress;
        public double DigitHours = 2;
        public static readonly string[] Modes = { "seq", "kxe", "random", "zone", "wave", "hybrid" };

        public static PultSettings Read(string path)
        {
            return FromDictionary(Ini.Read(path));
        }
        public static PultSettings FromDictionary(Dictionary<string,string> d)
        {
            PultSettings s = new PultSettings();
            s.Mode = Ini.Get(d,"mode",s.Mode).ToLowerInvariant();
            string[] legacy = { "", "random", "seq", "zone", "kxe", "wave", "hybrid" };
            int legacyMode; if (Int32.TryParse(s.Mode,out legacyMode) && legacyMode >= 1 && legacyMode <= 6) s.Mode = legacy[legacyMode];
            if (Array.IndexOf(Modes,s.Mode) < 0) s.Mode = "random";
            s.PubkeyPath = Ini.Get(d,"pubkey",s.PubkeyPath); s.FoundPath = Ini.Get(d,"found",s.FoundPath);
            s.MaskHex = Ini.Get(d,"mask_hex",""); s.Freeze = Ini.Int(d,"mask_froz",40); s.UseMask = s.MaskHex.Length > 0;
            if (!s.UseMask) s.Freeze = 40;
            s.RangeStart = Ini.Int(d,"range_start",s.RangeStart); s.RangeEnd = Ini.Int(d,"range_end",s.RangeEnd);
            s.ZoneNibbles = Ini.Int(d,"zone_nibbles",s.ZoneNibbles); s.WaveNibbles = Ini.Int(d,"wave_nibbles",s.WaveNibbles);
            s.HybridNibbles = Ini.Int(d,"hybrid_nibbles",s.HybridNibbles); s.ChunkBits = Ini.Int(d,"chunk",s.ChunkBits);
            s.Power = Ini.Int(d,"gpu_power",100); if (s.Power == 0) s.Power = 100;
            s.DetHex = ParseDetHex(Ini.Get(d,"det_hex","-1")); s.DigitHours = Ini.Double(d,"digit_hours",2);
            s.WantNk = Ini.Int(d,"want_nk",1) != 0; s.Debug = Ini.Int(d,"debug",0) != 0;
            s.TgProgress = Ini.Int(d,"tg_progress",0) != 0; s.SignalProgress = Ini.Int(d,"signal_progress",0) != 0;
            s.ProgressSeconds = Ini.Int(d,"progress",36000); s.CheckSeconds = Ini.Int(d,"check",36000);
            s.WaveStart = Ini.Get(d,"wave_start","");
            s.Threads = Ini.Int(d,"threads",1);
            return s;
        }
        public static int ParseDetHex(string value)
        {
            if (value == "-1" || value == "Все") return -1;
            if (Regex.IsMatch(value,"^[a-fA-F]$")) return Convert.ToInt32(value,16);
            int n; return Int32.TryParse(value,out n) && n >= 0 && n <= 15 ? n : -1;
        }
        public string Validate()
        {
            if (Array.IndexOf(Modes,Mode) < 0) return "Выберите один из шести режимов.";
            if (RangeStart < 0 || RangeStart > 255 || RangeEnd <= RangeStart || RangeEnd > 256) return "Диапазон: начальный бит 0–255; конечный больше начального и не выше 256.";
            if (Power < 50 || Power > 100 || Power % 5 != 0) return "Нагрузка GPU: 50–100% с шагом 5%.";
            if (ZoneNibbles < 3 || ZoneNibbles > 15 || HybridNibbles < 3 || HybridNibbles > 15 || WaveNibbles < 3 || WaveNibbles > 10) return "Размер N: зона и гибрид 3–15; волна 3–10.";
            if (ChunkBits < 10 || ChunkBits > 40) return "Размер участка 2^N: N должен быть от 10 до 40.";
            if (Threads < 1 || Threads > 1024) return "Число потоков CPU: от 1 до 1024.";
            if (DetHex < -1 || DetHex > 15) return "Ведущая HEX-цифра: «Все» или 0–f.";
            if (!(Mode == "seq" && UseMask) && DetHex >= 0 && !IsLeadingDigitAllowed(RangeStart,RangeEnd,DetHex)) return "Выбранная HEX-цифра не встречается в этом диапазоне.";
            if (Double.IsNaN(DigitHours) || Double.IsInfinity(DigitHours) || DigitHours < 0 || DigitHours > 8760) return "Время смены цифры: от 0 до 8760 часов.";
            if (ProgressSeconds < 1 || CheckSeconds < 1) return "Интервалы уведомлений должны быть целым числом секунд, не меньше 1.";
            if (String.IsNullOrWhiteSpace(PubkeyPath) || String.IsNullOrWhiteSpace(FoundPath)) return "Выберите файл базы и файл результатов.";
            if (HasNewline(PubkeyPath) || HasNewline(FoundPath)) return "Путь к файлу не может содержать перевод строки.";
            if (!String.IsNullOrEmpty(WaveStart) && !Regex.IsMatch(WaveStart,"^[0-9a-fA-F]{64}$")) return "Сохранённый wave_start должен быть пустым или содержать ровно 64 знака HEX.";
            if (Mode == "seq" && UseMask)
            {
                string hex = NormalizeHex(MaskHex);
                if (!Regex.IsMatch(hex,"^[0-9a-fA-F]{64}$")) return "Образец должен содержать ровно 64 знака HEX (пробелы допускаются).";
                if (Freeze < 1 || Freeze > 64) return "Число неизменных младших нибблов: от 1 до 64, счёт справа.";
            }
            return null;
        }
        public static bool IsLeadingDigitAllowed(int start,int end,int digit)
        {
            if (start < 0 || start >= end || end > 256 || digit < 0 || digit > 15) return false;
            int shift = ((end-1)/4)*4;
            int minimum = start >= shift ? 1 << (start-shift) : 0;
            int maximum = (1 << (end-shift))-1;
            return digit >= minimum && digit <= maximum;
        }
        static bool HasNewline(string value) { return value.IndexOfAny(new char[] { '\r','\n','\0' }) >= 0; }
        public static string NormalizeHex(string value) { return Regex.Replace(value ?? "",@"\s",""); }
        public Dictionary<string,string> ToDictionary()
        {
            Dictionary<string,string> d = new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
            d["mode"] = Mode; d["pubkey"] = PubkeyPath; d["found"] = FoundPath;
            d["range_start"] = I(RangeStart); d["range_end"] = I(RangeEnd); d["gpu_power"] = I(Power);
            d["zone_nibbles"] = I(ZoneNibbles); d["wave_nibbles"] = I(WaveNibbles); d["hybrid_nibbles"] = I(HybridNibbles); d["chunk"] = I(ChunkBits);
            bool mask = Mode == "seq" && UseMask;
            d["mask_hex"] = mask ? NormalizeHex(MaskHex) : ""; d["mask_froz"] = mask ? I(Freeze) : "0";
            d["det_hex"] = mask ? "-1" : DetHex < 0 ? "-1" : DetHex.ToString("x",CultureInfo.InvariantCulture);
            d["digit_hours"] = DigitHours.ToString("0.########",CultureInfo.InvariantCulture);
            d["want_nk"] = WantNk ? "1" : "0"; d["debug"] = Debug ? "1" : "0";
            d["zone_debug"] = Debug ? "1" : "0"; d["hybrid_debug"] = Debug ? "1" : "0";
            d["tg_progress"] = TgProgress ? "1" : "0"; d["signal_progress"] = SignalProgress ? "1" : "0";
            d["progress"] = I(ProgressSeconds); d["check"] = I(CheckSeconds); d["wave_start"] = WaveStart; d["threads"] = I(Threads);
            return d;
        }
        public Dictionary<string,string> NormalizedSnapshot(string appRoot)
        {
            Dictionary<string,string> values = ToDictionary();
            values["mask_hex"] = NormalizeHex(values["mask_hex"]).ToLowerInvariant();
            values["wave_start"] = NormalizeHex(values["wave_start"]).ToLowerInvariant();
            foreach (string key in new string[] { "pubkey", "found" })
            {
                string path = values[key];
                values[key] = Path.GetFullPath(Path.IsPathRooted(path) ? path : Path.Combine(appRoot,path)).ToUpperInvariant();
            }
            return values;
        }
        static string I(int n) { return n.ToString(CultureInfo.InvariantCulture); }
    }

    public static class Ini
    {
        public static Dictionary<string,string> Read(string path)
        {
            return Parse(ReadTextShared(path));
        }
        public static string ReadTextShared(string path)
        {
            if (!File.Exists(path)) return "";
            using (FileStream stream = new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite | FileShare.Delete))
            using (StreamReader reader = new StreamReader(stream,Encoding.UTF8,true)) return reader.ReadToEnd();
        }
        public static Dictionary<string,string> Parse(string text)
        {
            Dictionary<string,string> d = new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
            foreach (string line in text.Split(new char[] { '\r','\n' },StringSplitOptions.RemoveEmptyEntries))
            {
                string t = line.Trim(); if (t.Length == 0 || t[0] == '#' || t[0] == ';') continue;
                int p = t.IndexOf('='); if (p < 1) continue;
                string v = t.Substring(p+1).Trim();
                for (int i = 0; i < v.Length; i++) if (v[i] == '#' && (i == 0 || Char.IsWhiteSpace(v[i-1]))) { v = v.Substring(0,i).TrimEnd(); break; }
                d[t.Substring(0,p).Trim()] = v;
            }
            return d;
        }
        public static string Get(Dictionary<string,string> d,string key,string fallback) { string value; return d.TryGetValue(key,out value) ? value : fallback; }
        public static int Int(Dictionary<string,string> d,string key,int fallback) { int n; return Int32.TryParse(Get(d,key,""),NumberStyles.Integer,CultureInfo.InvariantCulture,out n) ? n : fallback; }
        public static double Double(Dictionary<string,string> d,string key,double fallback) { double n; return System.Double.TryParse(Get(d,key,"").Replace(',','.'),NumberStyles.Float,CultureInfo.InvariantCulture,out n) ? n : fallback; }
        public static void WriteAtomic(string path,Dictionary<string,string> d)
        {
            StringBuilder text = new StringBuilder(); foreach (KeyValuePair<string,string> kv in d) text.Append(kv.Key).Append('=').Append(kv.Value).Append('\n');
            string temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            File.WriteAllText(temp,text.ToString(),new UTF8Encoding(false));
            if (File.Exists(path)) File.Replace(temp,path,null); else File.Move(temp,path);
        }
    }

    public sealed class InitialSample
    {
        public string PrivateKey, X, Y, PubC, PubU;
        public int Generation;
        public static InitialSample Read(string path)
        {
            Dictionary<string,string> d = Ini.Read(path);
            InitialSample s = new InitialSample();
            s.Generation = Ini.Int(d,"sample_generation",1);
            s.PrivateKey = Value(d,"private_key","private","sample_hex","hex"); s.X = Value(d,"x"); s.Y = Value(d,"y");
            s.PubC = Value(d,"pubc","pub_c","pub_compressed"); s.PubU = Value(d,"pubu","pub_u","pub_uncompressed");
            if (!IsHex(s.PrivateKey,64) || !IsHex(s.X,64) || !IsHex(s.Y,64) || !IsHex(s.PubC,66) || !IsHex(s.PubU,130)) return null;
            return s;
        }
        static string Value(Dictionary<string,string> d,params string[] keys) { foreach (string key in keys) { string v; if (d.TryGetValue(key,out v)) return v; } return ""; }
        static bool IsHex(string s,int length) { return s != null && s.Length == length && Regex.IsMatch(s,"^[0-9a-fA-F]+$"); }
    }
}
