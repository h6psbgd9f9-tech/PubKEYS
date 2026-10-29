using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace PubKEYS.Pult
{
    public sealed class PultProfileRecord
    {
        public Dictionary<string,string> Draft, AcceptedRun, EffectiveResume;
        public bool ResumeDesired, ResumePaused, DraftDirty;
        public int AcceptedRevision;
        public long ResumeGeneration;
        public string LastSessionId = "", LastPhase = "", CheckpointPolicy = "", CheckpointIdentity = "", CheckpointPath = "";
    }
    public sealed class PultProfileLoadResult
    {
        public PultProfileRecord Record;
        public bool Exists, IsValid;
        public string Error;
    }
    public sealed class PultResumeExport
    {
        public Dictionary<string,string> Settings;
        public int ActualRevision;
        public long Generation;
        public string Policy, Identity, CheckpointPath;
    }
    public sealed class PultAutoResumeCoordinator
    {
        public bool Attempted { get; private set; }
        public string BlockingReason { get; private set; }
        public bool TryOnce(Action start,string blockingReason)
        {
            if (Attempted) return false;
            Attempted = true; BlockingReason = blockingReason;
            if (!String.IsNullOrEmpty(blockingReason)) return false;
            start(); return true;
        }
    }
    public sealed class PultProfileStore
    {
        public static readonly string[] WorkerKeys = { "mode","pubkey","found","range_start","range_end","gpu_power","zone_nibbles","wave_nibbles","hybrid_nibbles","chunk","mask_hex","mask_froz","det_hex","digit_hours","want_nk","debug","zone_debug","hybrid_debug","tg_progress","signal_progress","progress","check","wave_start","threads" };
        public static readonly string[] DraftKeys = { "mode","pubkey","found","range_start","range_end","gpu_power","zone_nibbles","wave_nibbles","hybrid_nibbles","chunk","mask_hex","mask_froz","mask_enabled","det_hex","digit_hours","want_nk","debug","tg_progress","signal_progress","progress","check","wave_start","threads" };
        readonly string root;
        string lastSaved;
        public string FilePath { get; private set; }
        public PultProfileStore(string appRoot) { root = Path.GetFullPath(appRoot); FilePath = Path.Combine(root,"pult_profile.ini"); }
        public PultProfileLoadResult Load()
        {
            PultProfileLoadResult result = new PultProfileLoadResult { Exists = File.Exists(FilePath) };
            if (!result.Exists) return result;
            try
            {
                Dictionary<string,string> values = ReadLiteral(FilePath);
                ValidateProfileKeys(values);
                string checksum = Ini.Get(values,"profile_checksum","");
                if (!Regex.IsMatch(checksum,"^[0-9a-fA-F]{64}$") || !String.Equals(checksum,Checksum(values),StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Контрольная сумма профиля не совпадает.");
                if (Ini.Get(values,"profile_version","") != "1") throw new InvalidDataException("Неподдерживаемая версия профиля.");
                PultProfileRecord p = new PultProfileRecord();
                p.ResumeDesired = Flag(values,"resume_desired"); p.ResumePaused = Flag(values,"resume_paused"); p.DraftDirty = Flag(values,"draft_dirty");
                if (Flag(values,"has_draft"))
                {
                    p.Draft = new Dictionary<string,string>();
                    foreach (string key in DraftKeys)
                    {
                        string encoded; if (!values.TryGetValue("draft."+key,out encoded)) throw new InvalidDataException("Неполный черновик профиля.");
                        string value = Encoding.UTF8.GetString(Convert.FromBase64String(encoded));
                        if (value.Length > 4096 || value.IndexOf('\0') >= 0) throw new InvalidDataException("Недопустимый размер поля профиля.");
                        p.Draft[key] = value;
                    }
                    if (Array.IndexOf(PultSettings.Modes,p.Draft["mode"]) < 0) throw new InvalidDataException("Некорректный режим профиля.");
                }
                if (Flag(values,"has_accepted"))
                {
                    p.AcceptedRun = TakeSettings(values,"accepted."); ValidateSettings(p.AcceptedRun);
                    p.AcceptedRevision = Ini.Int(values,"accepted_revision",0);
                    if (p.AcceptedRevision < 1) throw new InvalidDataException("Нет подтверждённой ревизии профиля.");
                }
                if (Flag(values,"has_effective")) { p.EffectiveResume = TakeSettings(values,"effective."); ValidateSettings(p.EffectiveResume); }
                p.LastSessionId = Ini.Get(values,"last_session_id","");
                if (p.LastSessionId.Length > 0 && !Regex.IsMatch(p.LastSessionId,"^[0-9a-fA-F]{32}$")) throw new InvalidDataException("Некорректный идентификатор сеанса.");
                p.LastPhase = Ini.Get(values,"last_phase",""); p.CheckpointPolicy = Ini.Get(values,"checkpoint_policy","");
                p.CheckpointIdentity = Ini.Get(values,"checkpoint_identity",""); p.CheckpointPath = Ini.Get(values,"checkpoint_path","");
                Int64.TryParse(Ini.Get(values,"resume_generation","0"),NumberStyles.None,CultureInfo.InvariantCulture,out p.ResumeGeneration);
                if (p.ResumeDesired && (p.AcceptedRun == null || p.EffectiveResume == null)) throw new InvalidDataException("Нет подтверждённых параметров продолжения.");
                if (p.LastPhase == "completed" || p.LastPhase == "error") p.ResumeDesired = false;
                result.Record = p; result.IsValid = true;
            }
            catch (Exception ex) { result.Error = "Профиль не загружен: " + ex.Message; }
            return result;
        }
        public void Save(PultProfileRecord p)
        {
            Dictionary<string,string> values = new Dictionary<string,string>();
            values["profile_version"] = "1"; values["resume_desired"] = p.ResumeDesired && p.AcceptedRun != null && p.EffectiveResume != null && p.LastPhase != "completed" && p.LastPhase != "error" ? "1" : "0";
            values["resume_paused"] = p.ResumePaused ? "1" : "0"; values["draft_dirty"] = p.DraftDirty ? "1" : "0";
            values["has_draft"] = p.Draft != null ? "1" : "0"; values["has_accepted"] = p.AcceptedRun != null ? "1" : "0"; values["has_effective"] = p.EffectiveResume != null ? "1" : "0";
            if (p.Draft != null) foreach (string key in DraftKeys)
            {
                string value; if (!p.Draft.TryGetValue(key,out value) || value.Length > 4096) throw new InvalidDataException("Неполный или слишком длинный черновик.");
                values["draft."+key] = Convert.ToBase64String(Encoding.UTF8.GetBytes(value));
            }
            if (p.AcceptedRun != null) { ValidateSettings(p.AcceptedRun); AddSettings(values,"accepted.",p.AcceptedRun); }
            if (p.EffectiveResume != null) { ValidateSettings(p.EffectiveResume); AddSettings(values,"effective.",p.EffectiveResume); }
            values["accepted_revision"] = p.AcceptedRevision.ToString(CultureInfo.InvariantCulture);
            values["resume_generation"] = p.ResumeGeneration.ToString(CultureInfo.InvariantCulture);
            values["last_session_id"] = p.LastSessionId; values["last_phase"] = p.LastPhase;
            values["checkpoint_policy"] = p.CheckpointPolicy; values["checkpoint_identity"] = p.CheckpointIdentity; values["checkpoint_path"] = p.CheckpointPath;
            values["profile_checksum"] = Checksum(values);
            StringBuilder text = new StringBuilder();
            foreach (KeyValuePair<string,string> entry in values) { if (entry.Value.IndexOfAny(new char[] { '\r','\n','\0' }) >= 0) throw new InvalidDataException("Перевод строки в параметре профиля."); text.Append(entry.Key).Append('=').Append(entry.Value).Append('\n'); }
            string body = text.ToString(); if (body == lastSaved && File.Exists(FilePath)) return;
            WritePrivateAtomic(FilePath,body); lastSaved = body;
        }
        public static Dictionary<string,string> DraftFromSettings(PultSettings settings)
        {
            Dictionary<string,string> worker = settings.ToDictionary(), draft = new Dictionary<string,string>();
            foreach (string key in DraftKeys) if (worker.ContainsKey(key)) draft[key] = worker[key];
            draft["mask_enabled"] = settings.UseMask ? "1" : "0"; draft["mask_hex"] = settings.MaskHex;
            draft["mask_froz"] = settings.Freeze.ToString(CultureInfo.InvariantCulture); draft["det_hex"] = settings.DetHex.ToString(CultureInfo.InvariantCulture);
            return draft;
        }
        public static bool CanUseLegacyResume(Dictionary<string,string> settings)
        {
            PultSettings s = PultSettings.FromDictionary(settings); return s.DetHex < 0 || s.DigitHours == 0;
        }
        public static PultProfileRecord CreateFromAcceptedSession(string appRoot,string enginePath,string sessionId,bool resumeDesired)
        {
            string directory = SessionPath(enginePath,sessionId);
            Dictionary<string,string> status = ReadLiteral(Path.Combine(directory,"status.ini"));
            int revision = Ini.Int(status,"actual_revision",0); string phase = Ini.Get(status,"phase","");
            if (revision < 1 || (phase != "running" && phase != "paused" && phase != "completed" && phase != "stopped")) throw new InvalidDataException("Сеанс ещё не подтвердил параметры.");
            string file = revision == 1 ? "settings.ini" : "settings_" + revision.ToString("D12",CultureInfo.InvariantCulture) + ".ini";
            Dictionary<string,string> source = ReadLiteral(Path.Combine(directory,file));
            if (Ini.Int(source,"revision",0) != revision) throw new InvalidDataException("Ревизия снимка не совпадает со статусом.");
            Dictionary<string,string> accepted = TakeSettings(source,""); ValidateSettings(accepted);
            PultProfileRecord p = new PultProfileRecord { AcceptedRun = accepted, AcceptedRevision = revision, LastSessionId = sessionId, LastPhase = phase, ResumePaused = phase == "paused", Draft = DraftFromSettings(PultSettings.FromDictionary(accepted)) };
            PultResumeExport export; string error;
            if (TryReadResumeExport(enginePath,sessionId,status,out export,out error))
            {
                p.EffectiveResume = export.Settings; p.ResumeGeneration = export.Generation; p.CheckpointPolicy = export.Policy; p.CheckpointIdentity = export.Identity; p.CheckpointPath = export.CheckpointPath;
            }
            else if (!status.ContainsKey("resume_generation") && CanUseLegacyResume(accepted)) { p.EffectiveResume = new Dictionary<string,string>(accepted); p.CheckpointPolicy = "legacy"; }
            p.ResumeDesired = resumeDesired && p.EffectiveResume != null && (phase == "running" || phase == "paused");
            return p;
        }
        public static Dictionary<string,string> ReadSessionStatus(string enginePath,string sessionId) { return ReadLiteral(Path.Combine(SessionPath(enginePath,sessionId),"status.ini")); }
        public static bool TryReadResumeExport(string enginePath,string sessionId,Dictionary<string,string> status,out PultResumeExport export,out string error)
        {
            export = null; error = null;
            try
            {
                long generation; if (!Int64.TryParse(Ini.Get(status,"resume_generation","0"),out generation) || generation < 1) return false;
                Dictionary<string,string> values = ReadLiteral(Path.Combine(SessionPath(enginePath,sessionId),"resume_settings.ini"));
                HashSet<string> allowed = new HashSet<string>(WorkerKeys,StringComparer.Ordinal);
                foreach (string key in new string[] { "revision","actual_revision","resume_generation","checkpoint_policy","checkpoint_identity","checkpoint_path" }) allowed.Add(key);
                foreach (string key in values.Keys) if (!allowed.Contains(key)) throw new InvalidDataException("Неизвестное поле снимка продолжения.");
                long fileGeneration; Int64.TryParse(Ini.Get(values,"resume_generation","0"),out fileGeneration);
                int actual = Ini.Int(status,"actual_revision",0);
                if (actual < 1 || Ini.Int(values,"actual_revision",0) != actual || Ini.Int(values,"revision",0) != actual || fileGeneration != generation) return false;
                Dictionary<string,string> settings = TakeSettings(values,""); ValidateSettings(settings);
                string policy = Ini.Get(values,"checkpoint_policy","");
                if (policy != "exact" && policy != "sampling") throw new InvalidDataException("Некорректная политика чекпоинта.");
                string identity = Ini.Get(values,"checkpoint_identity",""), checkpoint = Ini.Get(values,"checkpoint_path","");
                if (policy == "exact" && (!Regex.IsMatch(identity,"^[0-9a-fA-F]{64}$") || checkpoint != Path.GetFileName(checkpoint) || checkpoint.Length == 0)) throw new InvalidDataException("Неполная идентичность чекпоинта.");
                export = new PultResumeExport { Settings = settings, ActualRevision = actual, Generation = generation, Policy = policy, Identity = identity, CheckpointPath = checkpoint }; return true;
            }
            catch (Exception ex) { error = ex.Message; return false; }
        }
        static string SessionPath(string enginePath,string id)
        {
            if (!Regex.IsMatch(id ?? "","^[0-9a-fA-F]{32}$")) throw new InvalidDataException("Некорректный идентификатор сеанса.");
            string parent = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(enginePath)),"gui_sessions"), directory = Path.Combine(parent,id);
            if (!Directory.Exists(directory)) throw new DirectoryNotFoundException("Папка предыдущего сеанса отсутствует.");
            if ((File.GetAttributes(parent) & FileAttributes.ReparsePoint) != 0 || (File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("Недопустимая папка сеанса.");
            return directory;
        }
        static bool Flag(Dictionary<string,string> values,string key)
        {
            string value; if (!values.TryGetValue(key,out value) || (value != "0" && value != "1")) throw new InvalidDataException("Некорректный флаг профиля: " + key);
            return value == "1";
        }
        static Dictionary<string,string> TakeSettings(Dictionary<string,string> values,string prefix)
        {
            Dictionary<string,string> settings = new Dictionary<string,string>();
            foreach (string key in WorkerKeys) { string value; if (!values.TryGetValue(prefix+key,out value)) throw new InvalidDataException("Неполный снимок параметров: " + key); settings[key] = value; }
            return settings;
        }
        static void ValidateSettings(Dictionary<string,string> settings)
        {
            foreach (string key in WorkerKeys) if (!settings.ContainsKey(key)) throw new InvalidDataException("Неполный снимок параметров.");
            if (Array.IndexOf(PultSettings.Modes,settings["mode"]) < 0) throw new InvalidDataException("Некорректный режим снимка.");
            Integer(settings,"range_start",0,255); Integer(settings,"range_end",1,256); Integer(settings,"gpu_power",50,100);
            Integer(settings,"zone_nibbles",3,15); Integer(settings,"wave_nibbles",3,10); Integer(settings,"hybrid_nibbles",3,15);
            Integer(settings,"chunk",10,40); Integer(settings,"mask_froz",0,64); Integer(settings,"threads",1,1024);
            Integer(settings,"progress",1,Int32.MaxValue); Integer(settings,"check",1,Int32.MaxValue);
            foreach (string key in new string[] { "want_nk","debug","zone_debug","hybrid_debug","tg_progress","signal_progress" }) if (settings[key] != "0" && settings[key] != "1") throw new InvalidDataException("Некорректный флаг снимка.");
            string det = settings["det_hex"]; int digit;
            if (det != "-1" && !Regex.IsMatch(det,"^[0-9a-fA-F]$") && (!Int32.TryParse(det,NumberStyles.None,CultureInfo.InvariantCulture,out digit) || digit < 0 || digit > 15)) throw new InvalidDataException("Некорректная ведущая цифра снимка.");
            double hours; string rawHours = settings["digit_hours"];
            if (rawHours != rawHours.Trim() || !Double.TryParse(rawHours,NumberStyles.Float,CultureInfo.InvariantCulture,out hours) || Double.IsNaN(hours) || Double.IsInfinity(hours) || hours < 0 || hours > 8760) throw new InvalidDataException("Некорректное время смены цифры.");
            string mask = settings["mask_hex"], wave = settings["wave_start"];
            int frozen = Int32.Parse(settings["mask_froz"],CultureInfo.InvariantCulture);
            if ((mask.Length == 0 && frozen != 0) || (mask.Length > 0 && (!Regex.IsMatch(mask,"^[0-9a-fA-F]{64}$") || frozen < 1 || settings["mode"] != "seq"))) throw new InvalidDataException("Некорректная HEX-маска снимка.");
            if (wave.Length > 0 && !Regex.IsMatch(wave,"^[0-9a-fA-F]{64}$")) throw new InvalidDataException("Некорректная точка волны.");
            foreach (string key in new string[] { "pubkey","found" }) if (String.IsNullOrWhiteSpace(settings[key]) || settings[key].Length > 4096 || settings[key].IndexOfAny(new char[] { '\r','\n','\0' }) >= 0) throw new InvalidDataException("Некорректный путь снимка.");
            string error = PultSettings.FromDictionary(settings).Validate(); if (error != null) throw new InvalidDataException(error);
        }
        static void Integer(Dictionary<string,string> settings,string key,int minimum,int maximum)
        {
            int value; if (!Int32.TryParse(settings[key],NumberStyles.None,CultureInfo.InvariantCulture,out value) || value < minimum || value > maximum) throw new InvalidDataException("Некорректное число снимка: " + key);
        }
        static void ValidateProfileKeys(Dictionary<string,string> values)
        {
            HashSet<string> allowed = new HashSet<string>(StringComparer.Ordinal);
            foreach (string key in new string[] { "profile_version","profile_checksum","resume_desired","resume_paused","draft_dirty","has_draft","has_accepted","has_effective","accepted_revision","resume_generation","last_session_id","last_phase","checkpoint_policy","checkpoint_identity","checkpoint_path" }) allowed.Add(key);
            foreach (string key in DraftKeys) allowed.Add("draft."+key);
            foreach (string key in WorkerKeys) { allowed.Add("accepted."+key); allowed.Add("effective."+key); }
            foreach (string key in values.Keys) if (!allowed.Contains(key)) throw new InvalidDataException("Неизвестное поле профиля.");
        }
        public static string Checksum(Dictionary<string,string> values)
        {
            List<string> keys = new List<string>(values.Keys); keys.Sort(StringComparer.Ordinal);
            StringBuilder canonical = new StringBuilder(); foreach (string key in keys) if (key != "profile_checksum") canonical.Append(key).Append('=').Append(values[key]).Append('\n');
            using (SHA256 hash = SHA256.Create()) return BitConverter.ToString(hash.ComputeHash(Encoding.UTF8.GetBytes(canonical.ToString()))).Replace("-","").ToLowerInvariant();
        }
        static void AddSettings(Dictionary<string,string> values,string prefix,Dictionary<string,string> settings) { foreach (string key in WorkerKeys) values[prefix+key] = settings[key]; }
        public static Dictionary<string,string> ReadLiteral(string path)
        {
            if (!File.Exists(path)) throw new FileNotFoundException("Профиль или снимок отсутствует.",path);
            if (new FileInfo(path).Length > 262144 || (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("Профиль или снимок недоступен.");
            Dictionary<string,string> result = new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
            foreach (string raw in Ini.ReadTextShared(path).Split(new char[] { '\r','\n' },StringSplitOptions.RemoveEmptyEntries))
            {
                if (raw.TrimStart().StartsWith("#") || raw.TrimStart().StartsWith(";")) continue;
                int equals = raw.IndexOf('='); if (equals < 1) throw new InvalidDataException("Неверная строка профиля.");
                string key = raw.Substring(0,equals).Trim(); if (result.ContainsKey(key)) throw new InvalidDataException("Повтор ключа профиля.");
                result[key] = raw.Substring(equals+1);
            }
            return result;
        }
        static void WritePrivateAtomic(string path,string body)
        {
            string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            FileSecurity security = new FileSecurity(); security.SetAccessRuleProtection(true,false);
            SecurityIdentifier user = WindowsIdentity.GetCurrent().User; security.SetOwner(user);
            security.AddAccessRule(new FileSystemAccessRule(user,FileSystemRights.FullControl,AccessControlType.Allow));
            security.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.LocalSystemSid,null),FileSystemRights.FullControl,AccessControlType.Allow));
            try
            {
                if (File.Exists(path)) { if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("Ссылка вместо файла профиля."); File.SetAccessControl(path,security); }
                using (FileStream stream = new FileStream(temporary,FileMode.CreateNew,FileAccess.Write,FileShare.None,4096,FileOptions.WriteThrough))
                {
                    File.SetAccessControl(temporary,security); byte[] bytes = new UTF8Encoding(false).GetBytes(body); stream.Write(bytes,0,bytes.Length); stream.Flush(true);
                }
                if (File.Exists(path)) File.Replace(temporary,path,null); else File.Move(temporary,path);
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }
    }
}
