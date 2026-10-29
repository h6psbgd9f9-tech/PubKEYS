using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Security.Cryptography;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Threading;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Input;
using Microsoft.Win32;

namespace PubKEYS.Pult
{
    public static class WindowsCommandLine
    {
        // Windows CRT parsing consumes pairs of backslashes before a quote.
        // Quote every argument and double trailing slashes before its end quote.
        public static string QuoteArgument(string value)
        {
            if (value == null) throw new ArgumentNullException("value");
            StringBuilder result = new StringBuilder(); result.Append('"'); int slashes = 0;
            foreach (char c in value)
            {
                if (c == '\\') { slashes++; continue; }
                if (c == '"') { result.Append('\\',slashes * 2 + 1); result.Append('"'); }
                else { result.Append('\\',slashes); result.Append(c); }
                slashes = 0;
            }
            result.Append('\\',slashes * 2); result.Append('"'); return result.ToString();
        }
        public static string JoinArguments(params string[] args)
        {
            StringBuilder result = new StringBuilder();
            foreach (string arg in args) { if (result.Length > 0) result.Append(' '); result.Append(QuoteArgument(arg)); }
            return result.ToString();
        }
    }

    public interface IPultController : IDisposable
    {
        event Action Updated;
        event Action<string> Log;
        Dictionary<string,string> Status { get; }
        InitialSample Sample { get; }
        string SessionDirectory { get; }
        bool IsActive { get; }
        string BlockingReason { get; }
        void Start(PultSettings settings);
        int Apply(PultSettings settings);
        void Command(string action,int value);
        void Poll();
    }
    public interface IPultInitialPauseController { void Start(PultSettings settings,bool initiallyPaused); }

    public sealed class FilePultController : IPultController, IPultInitialPauseController
    {
        public event Action Updated;
        public event Action<string> Log;
        public Dictionary<string,string> Status { get; private set; }
        public InitialSample Sample { get; private set; }
        public string SessionDirectory { get; private set; }
        public bool IsActive { get; private set; }
        public string BlockingReason { get; private set; }
        readonly string appRoot, enginePath;
        readonly bool fixture;
        Process process;
        int commandRevision, settingsRevision, sampleRevision;
        string lastStatusText = "", lastPhase = "";
        bool exitLogged, stopRequested;
        public FilePultController(string root,string engine,string fixturePath)
        {
            appRoot = root; enginePath = engine; fixture = fixturePath != null;
            Status = new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
            if (fixture) { SessionDirectory = fixturePath; BlockingReason = "Проверка интерфейса: запуск двигателя отключён."; }
            else RefreshBlockingReason();
        }
        void RefreshBlockingReason()
        {
            BlockingReason = null;
            foreach (Process p in Process.GetProcesses())
            {
                try
                {
                    if (process != null && p.Id == process.Id) continue;
                    if (p.ProcessName.StartsWith("pubkeyhunt",StringComparison.OrdinalIgnoreCase))
                    {
                        BlockingReason = "Уже работает консольный поиск (PID " + p.Id.ToString(CultureInfo.InvariantCulture) + "). Завершите его в консоли, чтобы запустить новый сеанс из пульта.";
                        return;
                    }
                }
                catch (InvalidOperationException) { }
                finally { p.Dispose(); }
            }
        }
        public void Start(PultSettings settings) { Start(settings,false); }
        public void Start(PultSettings settings,bool initiallyPaused)
        {
            if (fixture) throw new InvalidOperationException(BlockingReason);
            if (IsActive) throw new InvalidOperationException("Сеанс уже запущен.");
            if (process != null && !process.HasExited) throw new InvalidOperationException("Предыдущий процесс ещё завершает работу. Дождитесь его выхода.");
            if (!Directory.Exists(appRoot)) throw new DirectoryNotFoundException("Рабочая папка пульта не найдена: " + appRoot);
            RefreshBlockingReason(); if (BlockingReason != null) throw new InvalidOperationException(BlockingReason);
            if (!File.Exists(enginePath)) throw new FileNotFoundException("Не найден двигатель для пульта: " + enginePath);
            if (!HasProtocolMarker(enginePath)) throw new InvalidOperationException("Этот двигатель не поддерживает протокол пульта. Требуется совместимая проверенная сборка; старый EXE запускаться не будет.");
            string error = settings.Validate(); if (error != null) throw new InvalidOperationException(error);
            SessionDirectory = Path.Combine(Path.GetDirectoryName(enginePath),"gui_sessions",Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(SessionDirectory);
            commandRevision = 0; settingsRevision = 1; sampleRevision = 0; Sample = null; lastStatusText = ""; exitLogged = false; stopRequested = false;
            Dictionary<string,string> values = settings.ToDictionary(); values["revision"] = "1";
            Ini.WriteAtomic(Path.Combine(SessionDirectory,"settings.ini"),values);
            if (initiallyPaused) WriteCommand("pause",0,0);
            Status = new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase); Status["phase"] = "starting";
            ProcessStartInfo start = new ProcessStartInfo(enginePath,WindowsCommandLine.JoinArguments("--pult-worker",SessionDirectory,"--pult-app-root",appRoot));
            start.WorkingDirectory = appRoot; start.UseShellExecute = false; start.CreateNoWindow = true;
            start.RedirectStandardOutput = true; start.RedirectStandardError = true;
            start.StandardOutputEncoding = Encoding.UTF8; start.StandardErrorEncoding = Encoding.UTF8;
            process = new Process(); process.StartInfo = start;
            process.OutputDataReceived += ProcessOutput; process.ErrorDataReceived += ProcessOutput;
            if (!process.Start()) throw new InvalidOperationException("Не удалось запустить двигатель.");
            process.BeginOutputReadLine(); process.BeginErrorReadLine(); IsActive = true;
            EmitLog("Запущен новый сеанс. Ожидание фактического статуса двигателя."); EmitUpdated();
        }
        void ProcessOutput(object sender,DataReceivedEventArgs e) { if (!String.IsNullOrEmpty(e.Data)) EmitLog(Redact(e.Data)); }
        static string Redact(string line)
        {
            if (Regex.IsMatch(line,@"(?i)(tg_token|tg_chat|archive_pass|signal_account|signal_to|notify_cmd)\s*=")) return "[Скрыта служебная строка с настройками доставки]";
            return line.Length > 3000 ? line.Substring(0,3000) + "…" : line;
        }
        public int Apply(PultSettings settings)
        {
            if (!IsActive) throw new InvalidOperationException("Сеанс ещё не запущен.");
            if (stopRequested) throw new InvalidOperationException("Сеанс завершает работу; новые параметры можно задать перед следующим запуском.");
            string error = settings.Validate(); if (error != null) throw new InvalidOperationException(error);
            int next = ++settingsRevision;
            Dictionary<string,string> values = settings.ToDictionary(); values["revision"] = next.ToString(CultureInfo.InvariantCulture);
            Ini.WriteAtomic(Path.Combine(SessionDirectory,"settings_" + next.ToString("D12",CultureInfo.InvariantCulture) + ".ini"),values);
            WriteCommand("apply",0,next); return next;
        }
        public void Command(string action,int value)
        {
            if (!IsActive) throw new InvalidOperationException("Сеанс не работает.");
            if (stopRequested && action != "stop") throw new InvalidOperationException("Команда завершения уже отправлена.");
            if (action != "pause" && action != "resume" && action != "stop" && action != "power") throw new ArgumentException("Неизвестная команда.");
            if (action == "power" && (value < 50 || value > 100 || value % 5 != 0)) throw new ArgumentException("Нагрузка GPU должна быть 50–100%, шаг 5%.");
            WriteCommand(action,value,0);
            if (action == "stop") stopRequested = true;
        }
        void WriteCommand(string action,int value,int settings)
        {
            int revision = ++commandRevision;
            Dictionary<string,string> d = new Dictionary<string,string>();
            d["revision"] = revision.ToString(CultureInfo.InvariantCulture); d["action"] = action;
            if (action == "power") d["value"] = value.ToString(CultureInfo.InvariantCulture);
            if (action == "apply") d["settings_revision"] = settings.ToString(CultureInfo.InvariantCulture);
            Ini.WriteAtomic(Path.Combine(SessionDirectory,"command_" + revision.ToString("D12",CultureInfo.InvariantCulture) + ".ini"),d);
        }
        public void Poll()
        {
            if (SessionDirectory == null) { RefreshBlockingReason(); EmitUpdated(); return; }
            string path = Path.Combine(SessionDirectory,"status.ini");
            try
            {
                if (File.Exists(path))
                {
                    string content = Ini.ReadTextShared(path);
                    if (content != lastStatusText)
                    {
                        Dictionary<string,string> next = Ini.Parse(content);
                        if (next.ContainsKey("phase"))
                        {
                            Status = next; lastStatusText = content;
                            string phase = Ini.Get(Status,"phase","");
                            if (phase != lastPhase) { lastPhase = phase; EmitLog("Состояние: " + PhaseTitle(phase)); }
                            if ((phase == "stopped" || phase == "completed" || phase == "error") && (process == null || process.HasExited)) IsActive = false;
                        }
                    }
                }
                int generation = Ini.Int(Status,"sample_generation",Ini.Int(Status,"actual_revision",1));
                string samplePath = Path.Combine(SessionDirectory,"sample_initial.txt");
                // The initial sample is immutable for a given worker session.
                // An applied search owns a new sample_generation published by the worker.
                if ((Sample == null || generation != sampleRevision) && File.Exists(samplePath))
                {
                    InitialSample nextSample = InitialSample.Read(samplePath);
                    if (nextSample != null && nextSample.Generation == generation) { Sample = nextSample; sampleRevision = generation; }
                }
                if (!fixture && process != null && process.HasExited && !exitLogged)
                {
                    exitLogged = true; IsActive = false;
                    string phase = Ini.Get(Status,"phase","");
                    if (phase != "stopped" && phase != "completed" && phase != "error")
                    {
                        Status["phase"] = "error"; Status["error"] = "Двигатель завершился без конечного статуса (код " + process.ExitCode.ToString(CultureInfo.InvariantCulture) + ").";
                    }
                    EmitLog("Процесс двигателя завершён; код " + process.ExitCode.ToString(CultureInfo.InvariantCulture) + ".");
                }
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            EmitUpdated();
        }
        public static string PhaseTitle(string phase)
        {
            switch (phase)
            {
                case "starting": return "Подготовка";
                case "running": return "Поиск работает";
                case "pausing": return "Ожидает паузы";
                case "paused": return "Пауза";
                case "applying": return "Применение параметров";
                case "stopping": return "Завершение пакета";
                case "stopped": return "Сеанс завершён";
                case "completed": return "Диапазон полностью проверен";
                case "error": return "Ошибка";
                default: return "Готов к запуску";
            }
        }
        public static bool HasProtocolMarker(string path)
        {
            byte[] marker = Encoding.ASCII.GetBytes("PUBKEYS_PULT_PROTOCOL=1");
            using (FileStream stream = new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite | FileShare.Delete))
            {
                int matched = 0, b;
                while ((b = stream.ReadByte()) >= 0)
                {
                    if (b == marker[matched]) { if (++matched == marker.Length) return true; }
                    else matched = b == marker[0] ? 1 : 0;
                }
            }
            return false;
        }
        void EmitUpdated() { if (Updated != null) Updated(); }
        void EmitLog(string text) { if (Log != null) Log(text); }
        public void Dispose() { if (process != null) process.Dispose(); }
    }

    public sealed class PultWindow
    {
        readonly Window window;
        readonly string root;
        readonly IPultController controller;
        readonly DispatcherTimer pollTimer, powerTimer;
        readonly GpuTemperatureMonitor temperatureMonitor;
        readonly MinuteMetricsCollector minuteMetrics;
        readonly SessionElapsedClock sessionClock;
        readonly string enginePath;
        readonly bool persistenceEnabled, injectedController;
        readonly PultProfileStore profileStore;
        readonly PultAutoResumeCoordinator autoResume = new PultAutoResumeCoordinator();
        readonly DispatcherTimer profileTimer;
        PultProfileRecord profile;
        bool profileLoaded, currentRunConfirmed, resumeAfterWindowClose, runStarted;
        string lastProfileError = "";
        readonly bool fixtureMode;
        readonly string[] radioNames = { "ModeSeq", "ModeKxe", "ModeRandom", "ModeZone", "ModeWave", "ModeHybrid" };
        PultSettings settings;
        string mode, waveStart;
        int zoneN, waveN, hybridN, pendingRevision;
        bool loading = true, closingRequested, dirty, stopQueued, applyWasPaused, updatingLeading;
        int? pendingPower;
        int? confirmedPower;
        string powerSession = "";
        decimal lastFound;
        string foundSession = "";
        Dictionary<string,string> lastSentSnapshot, acceptedSnapshot;
        readonly Dictionary<int,Dictionary<string,string>> sentSnapshots = new Dictionary<int,Dictionary<string,string>>();
        int lastNoopRevision, lastRejectedRevision;
        string lastCurrent = "", lastError = "";
        string inactiveDraftNotice = "";
        public Window Window { get { return window; } }
        public GpuTemperatureMonitor TemperatureMonitor { get { return temperatureMonitor; } }
        public MinuteMetricsCollector MinuteMetrics { get { return minuteMetrics; } }
        public SessionElapsedClock SessionClock { get { return sessionClock; } }
        public bool HasUnsubmittedSettings { get { return dirty; } }
        public bool IsApplyPending { get { return pendingRevision > 1 && controller.IsActive; } }
        public bool ApplyAlertActive { get; private set; }
        public bool FoundAlertActive { get; private set; }
        T C<T>(string name) where T : class { return (T)window.FindName(name); }
        TextBox T(string name) { return C<TextBox>(name); }
        TextBlock L(string name) { return C<TextBlock>(name); }
        bool Check(string name) { return C<CheckBox>(name).IsChecked == true; }
        public PultWindow(string appRoot,string engine,string fixture,IGpuTemperatureSource gpuSource = null,IMonotonicClock metricsClock = null,IPultController suppliedController = null)
        {
            root = appRoot;
            enginePath = engine; persistenceEnabled = fixture == null; injectedController = suppliedController != null;
            profileStore = persistenceEnabled ? new PultProfileStore(root) : null;
            PultProfileLoadResult loaded = persistenceEnabled ? profileStore.Load() : new PultProfileLoadResult();
            profileLoaded = loaded.IsValid; profile = loaded.IsValid ? loaded.Record : new PultProfileRecord();
            fixtureMode = fixture != null;
            IMonotonicClock uiClock = metricsClock ?? new StopwatchMonotonicClock();
            minuteMetrics = new MinuteMetricsCollector(uiClock); sessionClock = new SessionElapsedClock(uiClock);
            temperatureMonitor = new GpuTemperatureMonitor(gpuSource ?? (fixture == null ? (IGpuTemperatureSource)new NvmlGpuTemperatureSource() : new UnavailableGpuTemperatureSource()),false);
            using (Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("PubKEYS.Pult.xaml")) window = (Window)XamlReader.Load(stream);
            settings = PultSettings.Read(Path.Combine(root,"config.ini")); waveStart = settings.WaveStart;
            if (profileLoaded && profile.AcceptedRun != null) settings = PultSettings.FromDictionary(profile.AcceptedRun);
            if (!profileLoaded) { settings.WantNk = true; settings.TgProgress = true; settings.SignalProgress = true; }
            mode = settings.Mode; zoneN = settings.ZoneNibbles; waveN = settings.WaveNibbles; hybridN = settings.HybridNibbles;
            controller = suppliedController ?? new FilePultController(root,engine,fixture);
            controller.Updated += Refresh;
            controller.Log += delegate(string line) { window.Dispatcher.BeginInvoke(new Action(delegate { AppendLog(line); })); };
            powerTimer = new DispatcherTimer(); powerTimer.Interval = TimeSpan.FromMilliseconds(450);
            powerTimer.Tick += delegate { powerTimer.Stop(); if (controller.IsActive) Safe(delegate { controller.Command("power",(int)C<Slider>("PowerSlider").Value); }); };
            profileTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(600) };
            profileTimer.Tick += delegate { profileTimer.Stop(); SaveProfileQuiet(); };
            SetFields(settings);
            foreach (string name in radioNames)
            {
                C<RadioButton>(name).Checked += ModeChanged;
                if ((string)C<RadioButton>(name).Tag == mode) C<RadioButton>(name).IsChecked = true;
            }
            C<Slider>("PowerSlider").ValueChanged += delegate
            {
                L("PowerLabel").Text = ((int)C<Slider>("PowerSlider").Value).ToString(CultureInfo.InvariantCulture) + "%";
                if (!loading)
                {
                    if (controller.IsActive) { pendingPower = (int)C<Slider>("PowerSlider").Value; powerTimer.Stop(); powerTimer.Start(); L("PowerActual").Text = "Ожидание подтверждения нагрузки…"; }
                    else MarkDirty();
                    QueueProfileSave();
                }
            };
            C<ComboBox>("BlockN").SelectionChanged += delegate { if (!loading) { RememberN(); MarkDirty(); } };
            C<ComboBox>("LeadingHex").SelectionChanged += delegate { if (!loading && !updatingLeading) { UpdateDependencies(); MarkDirty(); } };
            foreach (string name in new string[] { "RangeStart","RangeEnd","MaskHex","Freeze","ChunkBits","DigitHours","PubkeyPath","FoundPath","ProgressSeconds","CheckSeconds","Threads","WaveStart" })
                T(name).TextChanged += delegate { if (!loading) { UpdateSummaries(); MarkDirty(); } };
            foreach (string name in new string[] { "UseMask","WantNk","DebugChunks","TgProgress","SignalProgress" })
            { C<CheckBox>(name).Checked += AnyCheckChanged; C<CheckBox>(name).Unchecked += AnyCheckChanged; }
            C<Button>("BrowseDb").Click += delegate { OpenFileDialog dialog = new OpenFileDialog(); dialog.Title = "Выберите базу публичных ключей"; dialog.Filter = "Текстовая база (*.txt)|*.txt|Все файлы (*.*)|*.*"; if (dialog.ShowDialog(window) == true) T("PubkeyPath").Text = dialog.FileName; };
            C<Button>("BrowseFound").Click += delegate { SaveFileDialog dialog = new SaveFileDialog(); dialog.Title = "Выберите файл результатов"; dialog.Filter = "Текстовый файл (*.txt)|*.txt|Все файлы (*.*)|*.*"; dialog.FileName = "Found.txt"; dialog.OverwritePrompt = false; if (dialog.ShowDialog(window) == true) T("FoundPath").Text = dialog.FileName; };
            C<Button>("StartButton").Click += delegate { Start(); };
            C<Button>("ApplyButton").Click += delegate { Apply(); };
            C<Button>("PauseButton").Click += delegate { Safe(delegate { controller.Command(Ini.Get(controller.Status,"phase","") == "paused" ? "resume" : "pause",0); }); };
            C<Button>("StopButton").Click += delegate { Stop(); };
            C<Button>("OpenJournal").Click += delegate { OpenJournal(); };
            C<Border>("FoundCard").MouseLeftButtonUp += delegate(object sender,MouseButtonEventArgs e) { AcknowledgeFoundAlert(); e.Handled = true; };
            C<Border>("FoundCard").KeyDown += delegate(object sender,KeyEventArgs e) { if (e.Key == Key.Enter || e.Key == Key.Space) { AcknowledgeFoundAlert(); e.Handled = true; } };
            temperatureMonitor.Updated += UpdateHeaderTemperature;
            window.Loaded += delegate { if (fixture == null) temperatureMonitor.Start(); AttemptAutoResume(); };
            window.Closed += delegate { profileTimer.Stop(); SaveProfileQuiet(); temperatureMonitor.Dispose(); Blink(C<Button>("ApplyButton"),false); Blink(C<Border>("FoundCard"),false); ApplyAlertActive = FoundAlertActive = false; };
            window.Closing += Closing;
            pollTimer = new DispatcherTimer(); pollTimer.Interval = TimeSpan.FromMilliseconds(700); pollTimer.Tick += delegate { controller.Poll(); }; pollTimer.Start();
            loading = false; UpdateMode(); UpdateDependencies(); UpdateSummaries();
            if (profileLoaded && profile.Draft != null) RestoreDraft(profile.Draft);
            if (profileLoaded && profile.AcceptedRun != null)
            {
                PultSettings baseline = ResumeUiBaseline();
                if (!profile.DraftDirty && profile.EffectiveResume != null) AdoptEffectiveFields();
                lastSentSnapshot = acceptedSnapshot = baseline.NormalizedSnapshot(root); sentSnapshots[1] = acceptedSnapshot;
            }
            else try { lastSentSnapshot = acceptedSnapshot = (profileLoaded && profile.Draft != null ? ReadFields() : settings).NormalizedSnapshot(root); if (profile.DraftDirty) lastSentSnapshot = null; sentSnapshots[1] = acceptedSnapshot; } catch (Exception) { }
            Refresh();
            UpdateHeaderTemperature();
            if (fixture != null) L("FooterMessage").Text = "Проверка интерфейса — двигатель не запущен.";
            AppendLog("Пульт открыт. Рабочие данные появятся после запуска сеанса."); controller.Poll();
            if (loaded.Exists && !loaded.IsValid) { AppendLog(loaded.Error); L("ValidationMessage").Text = loaded.Error; L("ValidationMessage").Visibility = Visibility.Visible; }
        }
        void UpdateHeaderTemperature()
        {
            GpuTemperatureReading reading = temperatureMonitor.Snapshot;
            bool valid = GpuTemperatureMonitor.IsValid(reading);
            string text = "GPU " + (valid ? GpuTemperatureAppearance.FormatTemperature(reading.Celsius) : "—");
            L("HeaderGpuTemperature").Text = text;
            L("HeaderGpuTemperature").Foreground = new SolidColorBrush(GpuTemperatureAppearance.ColorFor(valid ? reading.Celsius : null));
            L("HeaderGpuTemperature").ToolTip = reading == null ? "Температура GPU · ожидание датчика" :
                (String.IsNullOrWhiteSpace(reading.Name) ? "GPU NVIDIA" : reading.Name) + " · " +
                (GpuTemperatureMonitor.IsValid(reading) ? "обновлено " + reading.UpdatedAt.ToString("HH:mm:ss") + " · каждые 2 с" : String.IsNullOrEmpty(reading.Error) ? "нет данных" : reading.Error);
            window.Title = "PubKEYS · " + text + " — Пульт управления" + (fixtureMode ? " · ПРОВЕРКА ИНТЕРФЕЙСА" : "");
        }
        static readonly string[,] DraftTextFields = { {"range_start","RangeStart"},{"range_end","RangeEnd"},{"mask_hex","MaskHex"},{"mask_froz","Freeze"},{"chunk","ChunkBits"},{"digit_hours","DigitHours"},{"pubkey","PubkeyPath"},{"found","FoundPath"},{"progress","ProgressSeconds"},{"check","CheckSeconds"},{"threads","Threads"},{"wave_start","WaveStart"} };
        static readonly string[,] DraftChecks = { {"mask_enabled","UseMask"},{"want_nk","WantNk"},{"debug","DebugChunks"},{"tg_progress","TgProgress"},{"signal_progress","SignalProgress"} };
        Dictionary<string,string> CaptureDraft()
        {
            RememberN(); Dictionary<string,string> result = new Dictionary<string,string>();
            result["mode"] = mode;
            for (int i=0;i<DraftTextFields.GetLength(0);i++) result[DraftTextFields[i,0]] = T(DraftTextFields[i,1]).Text;
            for (int i=0;i<DraftChecks.GetLength(0);i++) result[DraftChecks[i,0]] = Check(DraftChecks[i,1]) ? "1" : "0";
            result["zone_nibbles"] = zoneN.ToString(); result["wave_nibbles"] = waveN.ToString(); result["hybrid_nibbles"] = hybridN.ToString();
            result["det_hex"] = (C<ComboBox>("LeadingHex").SelectedIndex-1).ToString(); result["gpu_power"] = ((int)C<Slider>("PowerSlider").Value).ToString(); return result;
        }
        void RestoreDraft(Dictionary<string,string> draft)
        {
            bool prior = loading; loading = true;
            try
            {
                mode = draft["mode"]; zoneN = Ini.Int(draft,"zone_nibbles",zoneN); waveN = Ini.Int(draft,"wave_nibbles",waveN); hybridN = Ini.Int(draft,"hybrid_nibbles",hybridN);
                for (int i=0;i<DraftTextFields.GetLength(0);i++) T(DraftTextFields[i,1]).Text = draft[DraftTextFields[i,0]];
                for (int i=0;i<DraftChecks.GetLength(0);i++) C<CheckBox>(DraftChecks[i,1]).IsChecked = draft[DraftChecks[i,0]] == "1";
                C<Slider>("PowerSlider").Value = Ini.Int(draft,"gpu_power",100); C<ComboBox>("LeadingHex").SelectedIndex = Ini.Int(draft,"det_hex",-1)+1;
                foreach (string name in radioNames) C<RadioButton>(name).IsChecked = (string)C<RadioButton>(name).Tag == mode;
                UpdateMode(); UpdateDependencies(); UpdateSummaries();
            }
            finally { loading = prior; }
        }
        PultSettings ResumeUiBaseline()
        {
            PultSettings result = PultSettings.FromDictionary(profile.AcceptedRun);
            if (profile.EffectiveResume == null) return result;
            PultSettings effective = PultSettings.FromDictionary(profile.EffectiveResume);
            result.DetHex = effective.DetHex; if (result.Mode == "wave") result.WaveStart = effective.WaveStart;
            if (result.Mode == "seq" || result.Mode == "kxe") result.ChunkBits = effective.ChunkBits;
            if (result.Mode == "zone") result.ZoneNibbles = effective.ZoneNibbles;
            if (result.Mode == "wave") result.WaveNibbles = effective.WaveNibbles;
            return result;
        }
        void AdoptEffectiveFields()
        {
            PultSettings effective = ResumeUiBaseline(); bool prior = loading; loading = true;
            try
            {
                if (!(mode == "seq" && Check("UseMask"))) C<ComboBox>("LeadingHex").SelectedIndex = effective.DetHex+1;
                if (mode == "wave") { T("WaveStart").Text = effective.WaveStart; waveN = effective.WaveNibbles; }
                if (mode == "zone") zoneN = effective.ZoneNibbles;
                if (mode == "seq" || mode == "kxe") T("ChunkBits").Text = effective.ChunkBits.ToString();
                UpdateMode(); UpdateDependencies(); UpdateSummaries();
            }
            finally { loading = prior; }
        }
        void QueueProfileSave() { if (persistenceEnabled && !loading) { profileTimer.Stop(); profileTimer.Start(); } }
        void SaveProfile()
        {
            if (!persistenceEnabled || loading) return;
            RecomputeDirty(); profile.Draft = CaptureDraft();
            profile.DraftDirty = dirty || (runStarted && (!currentRunConfirmed || pendingRevision > profile.AcceptedRevision));
            profileStore.Save(profile);
        }
        void SaveProfileQuiet()
        {
            try { SaveProfile(); lastProfileError = ""; }
            catch (Exception ex) { if (lastProfileError != ex.Message) { lastProfileError = ex.Message; AppendLog("Профиль не сохранён: " + ex.Message); L("ValidationMessage").Text = "Профиль не сохранён: " + ex.Message; L("ValidationMessage").Visibility = Visibility.Visible; } }
        }
        public void AttemptAutoResume()
        {
            if (!persistenceEnabled || !profileLoaded || autoResume.Attempted || !profile.ResumeDesired || profile.AcceptedRun == null || profile.EffectiveResume == null) return;
            Safe(delegate
            {
                bool started = autoResume.TryOnce(delegate
                {
                    bool paused = profile.ResumePaused;
                    profile.ResumeDesired = false; SaveProfile();
                    if (profile.LastSessionId.Length > 0) try
                    {
                        Dictionary<string,string> latest = PultProfileStore.ReadSessionStatus(enginePath,profile.LastSessionId);
                        string phase = Ini.Get(latest,"phase","");
                        if (phase == "completed" || phase == "error") { profile.ResumeDesired = false; profile.LastPhase = phase; SaveProfile(); AppendLog("Автопродолжение отключено: предыдущий сеанс завершён."); return; }
                        if (Ini.Int(latest,"actual_revision",0) != profile.AcceptedRevision) throw new InvalidDataException("Автопродолжение отключено: новые параметры предыдущего сеанса не подтверждены профилем. Проверьте их и нажмите «Запустить».");
                        PultResumeExport export; string error;
                        bool validExport = PultProfileStore.TryReadResumeExport(enginePath,profile.LastSessionId,latest,out export,out error);
                        if (latest.ContainsKey("resume_generation") && !validExport) throw new InvalidDataException("Автопродолжение отключено: снимок продолжения не совпадает со статусом. Проверьте параметры и нажмите «Запустить».");
                        if (!latest.ContainsKey("resume_generation") && !PultProfileStore.CanUseLegacyResume(profile.AcceptedRun)) throw new InvalidDataException("Автопродолжение отключено: текущая часть обхода не подтверждена.");
                        if (validExport)
                        {
                            profile.EffectiveResume = export.Settings; profile.ResumeGeneration = export.Generation; profile.CheckpointPolicy = export.Policy; profile.CheckpointIdentity = export.Identity; profile.CheckpointPath = export.CheckpointPath;
                            if (!profile.DraftDirty) AdoptEffectiveFields();
                            lastSentSnapshot = acceptedSnapshot = ResumeUiBaseline().NormalizedSnapshot(root);
                        }
                    }
                    catch (FileNotFoundException) { }
                    catch (DirectoryNotFoundException) { }
                    AppendLog("Параметры восстановлены; проверяется чекпоинт."); L("FooterMessage").Text = "Продолжение сохранённого сеанса…";
                    BeginRun(PultSettings.FromDictionary(profile.EffectiveResume),ResumeUiBaseline().NormalizedSnapshot(root),paused);
                },controller.BlockingReason);
                if (!started && !String.IsNullOrEmpty(autoResume.BlockingReason)) { L("FooterMessage").Text = "Автопродолжение не запущено: " + autoResume.BlockingReason; AppendLog(L("FooterMessage").Text); }
            });
        }
        void UpdateProfileFromStatus(Dictionary<string,string> status)
        {
            if (!persistenceEnabled) return;
            string phase = Ini.Get(status,"phase",""); int actual = Ini.Int(status,"actual_revision",0);
            bool ready = phase == "running" || phase == "paused" || phase == "completed";
            Dictionary<string,string> accepted;
            if (runStarted && ready && sentSnapshots.TryGetValue(actual,out accepted))
            {
                currentRunConfirmed = true; profile.AcceptedRun = new Dictionary<string,string>(accepted); profile.AcceptedRevision = actual;
                profile.LastSessionId = Path.GetFileName((controller.SessionDirectory ?? "").TrimEnd('\\','/'));
                if (!Regex.IsMatch(profile.LastSessionId,"^[0-9a-fA-F]{32}$")) profile.LastSessionId = "";
                profile.LastPhase = phase; profile.ResumePaused = phase == "paused";
                PultResumeExport export; string error;
                if (profile.LastSessionId.Length > 0 && PultProfileStore.TryReadResumeExport(enginePath,profile.LastSessionId,status,out export,out error))
                {
                    profile.EffectiveResume = export.Settings; profile.ResumeGeneration = export.Generation; profile.CheckpointPolicy = export.Policy; profile.CheckpointIdentity = export.Identity; profile.CheckpointPath = export.CheckpointPath;
                }
                else if (!status.ContainsKey("resume_generation") && PultProfileStore.CanUseLegacyResume(accepted)) { profile.EffectiveResume = new Dictionary<string,string>(accepted); profile.CheckpointPolicy = "legacy"; }
                else profile.EffectiveResume = null;
                profile.ResumeDesired = profile.EffectiveResume != null && phase != "completed" && (!stopQueued || resumeAfterWindowClose);
            }
            if (phase == "completed" || phase == "error") { profile.ResumeDesired = false; profile.LastPhase = phase; }
            else if (phase == "stopped") { profile.ResumeDesired = resumeAfterWindowClose && profile.EffectiveResume != null; profile.LastPhase = phase; }
            SaveProfileQuiet();
        }
        void SetFields(PultSettings s)
        {
            T("RangeStart").Text = s.RangeStart.ToString(); T("RangeEnd").Text = s.RangeEnd.ToString();
            T("MaskHex").Text = s.MaskHex; T("Freeze").Text = s.Freeze.ToString(); T("ChunkBits").Text = s.ChunkBits.ToString();
            T("DigitHours").Text = s.DigitHours.ToString("0.########",CultureInfo.CurrentCulture);
            T("PubkeyPath").Text = s.PubkeyPath; T("FoundPath").Text = s.FoundPath;
            T("ProgressSeconds").Text = s.ProgressSeconds.ToString(); T("CheckSeconds").Text = s.CheckSeconds.ToString();
            T("Threads").Text = s.Threads.ToString(); T("WaveStart").Text = s.WaveStart;
            C<CheckBox>("UseMask").IsChecked = s.UseMask; C<CheckBox>("WantNk").IsChecked = s.WantNk; C<CheckBox>("DebugChunks").IsChecked = s.Debug;
            C<CheckBox>("TgProgress").IsChecked = s.TgProgress; C<CheckBox>("SignalProgress").IsChecked = s.SignalProgress;
            C<Slider>("PowerSlider").Value = s.Power; L("PowerLabel").Text = s.Power.ToString() + "%";
            C<ComboBox>("LeadingHex").Items.Add(new ComboBoxItem { Content = "Все", Tag = -1, Foreground = Brushes.Black }); for (int i = 0; i < 16; i++) C<ComboBox>("LeadingHex").Items.Add(new ComboBoxItem { Content = i.ToString("x"), Tag = i });
            C<ComboBox>("LeadingHex").SelectedIndex = s.DetHex + 1;
        }
        void ModeChanged(object sender,RoutedEventArgs e)
        {
            if (loading) return;
            RememberN(); mode = (string)((RadioButton)sender).Tag; UpdateMode(); UpdateDependencies(); MarkDirty();
        }
        void RememberN()
        {
            object item = C<ComboBox>("BlockN").SelectedItem; if (item == null) return;
            int n = (int)item; if (mode == "zone") zoneN = n; else if (mode == "wave") waveN = n; else if (mode == "hybrid") hybridN = n;
        }
        void UpdateMode()
        {
            bool wasLoading = loading; loading = true;
            bool hasN = mode == "zone" || mode == "wave" || mode == "hybrid";
            C<StackPanel>("MaskPanel").Visibility = mode == "seq" ? Visibility.Visible : Visibility.Collapsed;
            C<StackPanel>("WaveStartPanel").Visibility = mode == "wave" ? Visibility.Visible : Visibility.Collapsed;
            C<Grid>("NPanel").Visibility = hasN ? Visibility.Visible : Visibility.Collapsed;
            C<Grid>("ChunkPanel").Visibility = hasN ? Visibility.Collapsed : Visibility.Visible;
            ComboBox combo = C<ComboBox>("BlockN"); combo.Items.Clear();
            int upper = mode == "wave" ? 10 : 15; for (int i = 3; i <= upper; i++) combo.Items.Add(i);
            combo.SelectedItem = mode == "wave" ? waveN : mode == "zone" ? zoneN : hybridN;
            L("NLabel").Text = mode == "zone" ? "Размер зоны N" : mode == "wave" ? "Размер волны N" : "Младшая часть B · N";
            L("NHint").Text = "16^N ключей · N = 3–" + upper.ToString();
            L("ModeNote").Text = mode == "random" || mode == "hybrid" ? "Выборочный режим. Полное покрытие диапазона не гарантируется." : mode == "seq" && Check("UseMask") ? "HEX-маска: меняется верхняя часть, младший хвост остаётся неизменным." : "Прогресс и счётчики поступают от существующего поискового двигателя.";
            loading = wasLoading;
        }
        void AnyCheckChanged(object sender,RoutedEventArgs e) { if (!loading) { UpdateDependencies(); UpdateSummaries(); MarkDirty(); } }
        void UpdateDependencies()
        {
            bool mask = mode == "seq" && Check("UseMask"); C<StackPanel>("MaskFields").IsEnabled = mask;
            C<StackPanel>("LeadingPanel").IsEnabled = !mask; T("DigitHours").IsEnabled = C<ComboBox>("LeadingHex").SelectedIndex > 0;
            UpdateSummaries();
        }
        void UpdateSummaries()
        {
            L("RangeSummary").Text = "Ключи в [2^" + T("RangeStart").Text + ", 2^" + T("RangeEnd").Text + ").";
            int freeze;
            if (Int32.TryParse(T("Freeze").Text,out freeze) && freeze >= 1 && freeze <= 64)
                L("MaskSummary").Text = "Справа неизменны " + freeze.ToString() + " из 64 нибблов. Слева меняются " + (64-freeze).ToString() + ". Шаг между кандидатами: 16^" + freeze.ToString() + ".";
            else L("MaskSummary").Text = "Введите от 1 до 64: отсчёт младших нибблов справа.";
            UpdateLeadingDigits();
        }
        void UpdateLeadingDigits()
        {
            ComboBox combo = C<ComboBox>("LeadingHex"); if (combo.Items.Count != 17 || updatingLeading) return;
            int start=0,end=0; bool valid = Int32.TryParse(T("RangeStart").Text,out start) && Int32.TryParse(T("RangeEnd").Text,out end);
            updatingLeading = true;
            try
            {
                for (int digit=0;digit<16;digit++)
                {
                    ComboBoxItem item = (ComboBoxItem)combo.Items[digit+1]; bool allowed = valid && PultSettings.IsLeadingDigitAllowed(start,end,digit);
                    item.IsEnabled = allowed; item.Foreground = allowed ? Brushes.Black : new SolidColorBrush(Color.FromRgb(160,173,177));
                    item.ToolTip = allowed ? null : "Цифра недоступна для выбранного диапазона.";
                }
                if (combo.SelectedIndex > 0 && !((ComboBoxItem)combo.SelectedItem).IsEnabled) combo.SelectedIndex = 0;
            }
            finally { updatingLeading = false; }
        }
        void MarkDirty()
        {
            inactiveDraftNotice = "";
            RecomputeDirty();
            L("ApplyNote").Text = dirty ? controller.IsActive ? "Изменения ещё не отправлены. Применение дождётся окончания текущего GPU-пакета." : "Изменения применятся перед запуском." : IsApplyPending ? "Настройки уже отправлены. Ожидание подтверждения." : "Настройки не изменились.";
            UpdateApplyAlert(); QueueProfileSave();
        }
        void RecomputeDirty()
        {
            try { dirty = !SnapshotsEqual(ReadFields().NormalizedSnapshot(root),lastSentSnapshot,controller.IsActive); }
            catch (Exception) { dirty = true; }
        }
        void SynchronizeConfirmedPower(Dictionary<string,string> status)
        {
            string session = controller.SessionDirectory ?? Ini.Get(status,"pid","");
            if (session != powerSession) { powerSession = session; confirmedPower = null; }
            int power = Ini.Int(status,"power",-1);
            if (power < 50 || power > 100) return;
            string phase = Ini.Get(status,"phase","");
            bool finished = !controller.IsActive || phase == "completed" || phase == "stopped" || phase == "error";
            bool unacknowledged = finished && pendingPower.HasValue && pendingPower.Value != power;
            if (confirmedPower.HasValue && confirmedPower.Value == power && !unacknowledged) return;
            confirmedPower = power;
            string value = power.ToString(CultureInfo.InvariantCulture);
            if (lastSentSnapshot != null) lastSentSnapshot["gpu_power"] = value;
            if (acceptedSnapshot != null) acceptedSnapshot["gpu_power"] = value;
            foreach (Dictionary<string,string> snapshot in sentSnapshots.Values) snapshot["gpu_power"] = value;
        }
        static bool SnapshotsEqual(Dictionary<string,string> a,Dictionary<string,string> b,bool ignorePower)
        {
            if (a == null || b == null || a.Count != b.Count) return false;
            foreach (KeyValuePair<string,string> entry in a)
            {
                if (ignorePower && entry.Key == "gpu_power") continue;
                string value; if (!b.TryGetValue(entry.Key,out value) || value != entry.Value) return false;
            }
            return true;
        }
        static int ReadInt(TextBox control) { int n; if (!Int32.TryParse(control.Text,NumberStyles.Integer,CultureInfo.InvariantCulture,out n)) throw new FormatException("Введите целое число в поле «" + System.Windows.Automation.AutomationProperties.GetName(control) + "»."); return n; }
        PultSettings ReadFields()
        {
            RememberN(); PultSettings s = new PultSettings(); s.Mode = mode;
            s.RangeStart = ReadInt(T("RangeStart")); s.RangeEnd = ReadInt(T("RangeEnd"));
            s.MaskHex = T("MaskHex").Text; s.Freeze = Check("UseMask") && mode == "seq" ? ReadInt(T("Freeze")) : 40; s.UseMask = Check("UseMask");
            s.ZoneNibbles = zoneN; s.WaveNibbles = waveN; s.HybridNibbles = hybridN; s.ChunkBits = ReadInt(T("ChunkBits"));
            s.Power = (int)C<Slider>("PowerSlider").Value; s.DetHex = C<ComboBox>("LeadingHex").SelectedIndex - 1;
            double hours; if (!Double.TryParse(T("DigitHours").Text.Replace(',','.'),NumberStyles.Float,CultureInfo.InvariantCulture,out hours)) throw new FormatException("Введите число часов от 0 до 8760."); s.DigitHours = hours;
            s.PubkeyPath = T("PubkeyPath").Text.Trim(); s.FoundPath = T("FoundPath").Text.Trim();
            s.WantNk = Check("WantNk"); s.Debug = Check("DebugChunks"); s.TgProgress = Check("TgProgress"); s.SignalProgress = Check("SignalProgress");
            s.ProgressSeconds = ReadInt(T("ProgressSeconds")); s.CheckSeconds = ReadInt(T("CheckSeconds")); s.WaveStart = T("WaveStart").Text.Trim(); s.Threads = ReadInt(T("Threads"));
            string error = s.Validate(); if (error != null) throw new FormatException(error); return s;
        }
        void Safe(Action action)
        {
            try { action(); L("ValidationMessage").Visibility = Visibility.Collapsed; }
            catch (Exception ex) { L("ValidationMessage").Text = ex.Message; L("ValidationMessage").Visibility = Visibility.Visible; AppendLog(ex.Message); }
        }
        void Start()
        {
            Safe(delegate { settings = ReadFields(); BeginRun(settings,settings.NormalizedSnapshot(root),false); });
        }
        void BeginRun(PultSettings run,Dictionary<string,string> baseline,bool paused)
        {
            string database = Path.IsPathRooted(run.PubkeyPath) ? run.PubkeyPath : Path.Combine(root,run.PubkeyPath);
            if (!injectedController && !File.Exists(database)) throw new FileNotFoundException("Файл базы не найден: " + database);
            if (persistenceEnabled) { profile.ResumeDesired = false; SaveProfile(); }
            Dictionary<string,string> previousLast = lastSentSnapshot, previousAccepted = acceptedSnapshot;
            Dictionary<int,Dictionary<string,string>> previousHistory = new Dictionary<int,Dictionary<string,string>>(sentSnapshots);
            currentRunConfirmed = false; resumeAfterWindowClose = false; runStarted = true;
            pendingRevision = 1; stopQueued = false; applyWasPaused = false; pendingPower = null; inactiveDraftNotice = "";
            lastSentSnapshot = acceptedSnapshot = baseline; sentSnapshots.Clear(); sentSnapshots[1] = baseline; lastNoopRevision = lastRejectedRevision = 0;
            try
            {
                if (paused)
                {
                    IPultInitialPauseController initialPause = controller as IPultInitialPauseController;
                    if (initialPause == null) throw new InvalidOperationException("Контроллер не поддерживает безопасное восстановление паузы.");
                    initialPause.Start(run,true);
                }
                else controller.Start(run);
            }
            catch
            {
                runStarted = false; pendingRevision = 0; lastSentSnapshot = previousLast; acceptedSnapshot = previousAccepted;
                sentSnapshots.Clear(); foreach (KeyValuePair<int,Dictionary<string,string>> entry in previousHistory) sentSnapshots[entry.Key] = entry.Value;
                UpdateApplyAlert(); SaveProfileQuiet(); throw;
            }
            ResetSamples(); minuteMetrics.Reset(); sessionClock.Start(controller.SessionDirectory);
            L("ApplyNote").Text = "Параметры переданы двигателю. Ожидание подтверждения."; Refresh();
        }
        void Apply()
        {
            Safe(delegate
            {
                settings = ReadFields();
                Dictionary<string,string> snapshot = settings.NormalizedSnapshot(root);
                if (SnapshotsEqual(snapshot,lastSentSnapshot,controller.IsActive))
                {
                    dirty = false;
                    L("ApplyNote").Text = IsApplyPending ? "Настройки уже отправлены. Ожидание подтверждения." : controller.IsActive ? "Настройки не изменились." : "Настройки не изменились. Для нового поиска нажмите «Запустить».";
                    UpdateApplyAlert(); return;
                }
                if (controller.IsActive)
                {
                    bool wasPaused = Ini.Get(controller.Status,"phase","") == "paused";
                    pendingRevision = controller.Apply(settings); dirty = false; applyWasPaused = wasPaused;
                    inactiveDraftNotice = "";
                    lastSentSnapshot = snapshot; sentSnapshots[pendingRevision] = snapshot;
                    L("ApplyNote").Text = "Ожидает завершения текущего GPU-пакета. Новые параметры: версия " + pendingRevision.ToString() + ".";
                    AppendLog("Отправлены параметры прохода; версия " + pendingRevision.ToString() + ".");
                }
                else { lastSentSnapshot = acceptedSnapshot = snapshot; dirty = false; inactiveDraftNotice = ""; L("ApplyNote").Text = "Параметры проверены. Нажмите «Запустить» для нового поиска."; }
                UpdateApplyAlert(); SaveProfileQuiet();
            });
        }
        void Stop()
        {
            Safe(delegate
            {
                resumeAfterWindowClose = false; stopQueued = true; profile.ResumeDesired = false; SaveProfileQuiet();
                try { controller.Command("stop",0); } catch { stopQueued = false; throw; }
                powerTimer.Stop(); L("FooterMessage").Text = "Команда завершения отправлена. Ожидание сохранения состояния."; Refresh();
            });
        }
        void ResetSamples()
        {
            T("InitialPrivate").Text = "Ожидание начального образца…";
            foreach (string name in new string[] { "InitialX","InitialY","InitialPubC","InitialPubU","CurrentPrivate" }) T(name).Text = "—";
            lastCurrent = ""; L("CurrentStamp").Text = "каждые 30 с";
        }
        void Refresh()
        {
            Dictionary<string,string> d = controller.Status; string phase = Ini.Get(d,"phase","");
            sessionClock.Observe(controller.SessionDirectory ?? Ini.Get(d,"pid",""),phase,Ini.Get(d,"elapsed_seconds",""));
            L("SessionElapsed").Text = sessionClock.Text;
            L("SessionElapsed").ToolTip = "Длительность текущего сеанса · " + sessionClock.Text;
            SynchronizeConfirmedPower(d);
            RecomputeDirty();
            string reason = controller.BlockingReason;
            bool applyPreparing = phase == "applying" || (phase == "starting" && pendingRevision > 1);
            string preparingMessage = applyWasPaused ? "Применяются настройки… пауза сохранится." : "Применяются настройки… поиск продолжится автоматически.";
            C<Button>("StartButton").IsEnabled = !controller.IsActive && reason == null;
            bool finalPhase = phase == "stopped" || phase == "completed" || phase == "error";
            C<Button>("PauseButton").IsEnabled = controller.IsActive && !stopQueued && (phase == "running" || phase == "paused");
            C<Button>("PauseButton").Content = phase == "paused" ? "Продолжить" : "Пауза";
            C<Button>("StopButton").IsEnabled = controller.IsActive && !stopQueued && !finalPhase && phase != "stopping";
            C<Button>("ApplyButton").IsEnabled = reason == null && (!controller.IsActive || (!stopQueued && (phase == "running" || phase == "paused")));
            L("StateLabel").Text = reason != null ? "Двигатель не подключён" : applyPreparing ? "Применяются настройки…" : FilePultController.PhaseTitle(phase);
            if (reason != null) L("FooterMessage").Text = reason;
            else if (phase == "completed") L("FooterMessage").Text = "Выбранный интервал проверен. Для нового поиска нажмите «Запустить»." + (controller.IsActive ? " Ожидание завершения обработки итогов." : "");
            else if (applyPreparing)
            {
                string detail = Ini.Get(d,"detail","");
                L("FooterMessage").Text = preparingMessage + (detail.Length > 0 ? " " + detail : "");
            }
            else if (phase.Length > 0) L("FooterMessage").Text = FilePultController.PhaseTitle(phase) + (controller.IsActive ? " · команды применяются между пакетами" : "");
            if (applyPreparing && pendingRevision > 0 && !dirty) L("ApplyNote").Text = preparingMessage;
            minuteMetrics.Observe(controller.SessionDirectory ?? Ini.Get(d,"pid",""),Ini.Int(d,"actual_revision",0),phase,Ini.Get(d,"count",""));
            ShowMinuteMetrics(); L("FoundValue").Text = Metric(Ini.Get(d,"found",""));
            string actualPower = Ini.Get(d,"power","");
            if (pendingPower.HasValue && (Ini.Int(d,"power",-1) == pendingPower.Value || !controller.IsActive)) pendingPower = null;
            L("PowerActual").Text = pendingPower.HasValue ? "Выбрано: " + pendingPower.Value.ToString() + "% · ожидает подтверждения" + (actualPower.Length > 0 ? " (сейчас " + actualPower + "%)" : "") : actualPower.Length > 0 ? "Фактически: " + actualPower + "%" : "Фактически: —";
            string actualRevision = Ini.Get(d,"actual_revision","");
            L("ActualSettings").Text = actualRevision.Length > 0 ? (phase == "starting" || phase == "applying" ? "Подготовка: " : "Применено: ") + Ini.Get(d,"mode","") + " · версия " + actualRevision : "";
            bool readyPhase = phase == "running" || phase == "paused" || phase == "completed";
            int actualNumber = Ini.Int(d,"actual_revision",0);
            Dictionary<string,string> accepted;
            if (readyPhase && sentSnapshots.TryGetValue(actualNumber,out accepted)) acceptedSnapshot = accepted;
            int noop = Ini.Int(d,"noop_revision",0), rejected = Ini.Int(d,"rejected_revision",0);
            if (noop > lastNoopRevision && sentSnapshots.TryGetValue(noop,out accepted))
            {
                lastNoopRevision = noop; acceptedSnapshot = accepted;
                if (actualNumber > 0 && actualNumber < noop) sentSnapshots[actualNumber] = accepted;
                if (readyPhase && actualNumber > noop && sentSnapshots.TryGetValue(actualNumber,out accepted)) acceptedSnapshot = accepted;
                if (pendingRevision == noop)
                {
                    pendingRevision = 0; RecomputeDirty();
                    if (!dirty) L("ApplyNote").Text = "Настройки не изменились. Перезапуск не требуется.";
                }
            }
            if (rejected > lastRejectedRevision)
            {
                lastRejectedRevision = rejected;
                if (pendingRevision == rejected)
                {
                    pendingRevision = 0; lastSentSnapshot = acceptedSnapshot; RecomputeDirty();
                    L("ApplyNote").Text = "Настройки отклонены. Исправьте параметры и отправьте снова.";
                }
            }
            if (pendingRevision > 0 && readyPhase && Ini.Int(d,"actual_revision",0) >= pendingRevision)
            {
                if (!dirty) L("ApplyNote").Text = "Параметры применены. Двигатель подтвердил версию " + actualRevision + ".";
                pendingRevision = 0;
            }
            if (!controller.IsActive && finalPhase && pendingRevision > actualNumber)
            {
                pendingRevision = 0; lastSentSnapshot = acceptedSnapshot; RecomputeDirty();
                inactiveDraftNotice = "Поиск завершился до применения. Параметры доступны для нового запуска через «Запустить».";
                L("ApplyNote").Text = inactiveDraftNotice;
            }
            UpdateApplyAlert(); UpdateFoundAlert(d); UpdateProfileFromStatus(d);
            if (controller.Sample != null)
            {
                T("InitialPrivate").Text = controller.Sample.PrivateKey; T("InitialX").Text = controller.Sample.X; T("InitialY").Text = controller.Sample.Y;
                T("InitialPubC").Text = controller.Sample.PubC; T("InitialPubU").Text = controller.Sample.PubU;
            }
            string current = Ini.Get(d,"current_sample_hex","");
            if (Regex.IsMatch(current,"^[0-9a-fA-F]{64}$") && current != lastCurrent)
            { lastCurrent = current; T("CurrentPrivate").Text = current; L("CurrentStamp").Text = DateTime.Now.ToString("HH:mm:ss") + " · 30 с"; }
            string error = Ini.Get(d,"error","");
            if (error.Length > 0 && error != lastError) { lastError = error; L("ValidationMessage").Text = error; L("ValidationMessage").Visibility = Visibility.Visible; AppendLog(error); }
            if (closingRequested && !controller.IsActive) { pollTimer.Stop(); controller.Dispose(); window.Close(); }
        }
        void UpdateApplyAlert()
        {
            RecomputeDirty();
            string phase = Ini.Get(controller.Status,"phase","");
            bool alert = dirty;
            Button button = C<Button>("ApplyButton");
            if (ApplyAlertActive != alert) { ApplyAlertActive = alert; Blink(button,alert); }
            if (alert)
            {
                button.Background = new SolidColorBrush(Color.FromRgb(188,48,62)); button.Foreground = Brushes.White;
                button.Content = controller.IsActive ? "Применить изменения" : "Подготовить новый запуск";
                button.ToolTip = controller.IsActive ? "Есть изменения, которые ещё не отправлены двигателю." : "Проверяет параметры в этом окне. Новый поиск запускается кнопкой «Запустить».";
                if (!controller.IsActive) L("ApplyNote").Text = inactiveDraftNotice.Length > 0 ? inactiveDraftNotice : "Черновик сохранён в окне. Для нового поиска нажмите «Запустить».";
            }
            else if (IsApplyPending)
            {
                button.Background = new SolidColorBrush(Color.FromRgb(255,242,214)); button.Foreground = new SolidColorBrush(Color.FromRgb(139,91,24));
                button.Content = "Ожидает применения…"; button.ToolTip = "Настройки отправлены. Ожидание подтверждения двигателя.";
            }
            else
            {
                button.ClearValue(Control.BackgroundProperty); button.ClearValue(Control.ForegroundProperty);
                button.Content = controller.IsActive ? "Применить параметры" : "Подготовить новый запуск";
                button.ToolTip = controller.IsActive ? null : "Проверяет параметры в этом окне. Новый поиск запускается кнопкой «Запустить».";
            }
            button.IsEnabled = controller.BlockingReason == null && dirty && (!controller.IsActive || (!stopQueued && (phase == "running" || phase == "paused")));
        }
        void UpdateFoundAlert(Dictionary<string,string> status)
        {
            string session = controller.SessionDirectory ?? Ini.Get(status,"pid","");
            if (session != foundSession)
            {
                foundSession = session; lastFound = 0; SetFoundBlink(false); L("FoundValue").Foreground = new SolidColorBrush(Color.FromRgb(19,125,130));
            }
            decimal count;
            if (!Decimal.TryParse(Ini.Get(status,"found",""),NumberStyles.None,CultureInfo.InvariantCulture,out count)) return;
            if (count == 0) SetFoundBlink(false);
            else if (count > lastFound) SetFoundBlink(true);
            lastFound = count;
            L("FoundValue").Foreground = count > 0 ? new SolidColorBrush(Color.FromRgb(183,28,44)) : new SolidColorBrush(Color.FromRgb(19,125,130));
            L("FoundValue").ToolTip = RussianMetricFormatter.FullCount(count.ToString("0",CultureInfo.InvariantCulture)) + " реальных совпадений";
        }
        void SetFoundBlink(bool active)
        {
            if (FoundAlertActive != active) { FoundAlertActive = active; Blink(C<Border>("FoundCard"),active); }
            C<Border>("FoundCard").Background = active ? new SolidColorBrush(Color.FromRgb(255,236,238)) : Brushes.White;
            C<Border>("FoundCard").BorderBrush = active ? new SolidColorBrush(Color.FromRgb(213,62,79)) : new SolidColorBrush(Color.FromRgb(218,229,232));
        }
        public void AcknowledgeFoundAlert() { SetFoundBlink(false); }
        static void Blink(UIElement element,bool active)
        {
            if (active) element.BeginAnimation(UIElement.OpacityProperty,new DoubleAnimation(1,0.38,TimeSpan.FromMilliseconds(500)) { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever });
            else { element.BeginAnimation(UIElement.OpacityProperty,null); element.ClearValue(UIElement.OpacityProperty); }
        }
        void ShowMinuteMetrics()
        {
            string rateUnit = "", countUnit = "";
            L("RateValue").Text = minuteMetrics.DisplayRate.HasValue ? RussianMetricFormatter.Compact(minuteMetrics.DisplayRate.Value,out rateUnit) : "—";
            L("RateUnit").Text = (rateUnit.Length > 0 ? rateUnit + " " : "") + "ключей/с";
            double count;
            L("CountValue").Text = Double.TryParse(minuteMetrics.DisplayCount,NumberStyles.Float,CultureInfo.InvariantCulture,out count) ? RussianMetricFormatter.Compact(count,out countUnit) : "—";
            L("CountUnit").Text = (countUnit.Length > 0 ? countUnit + " " : "") + "ключей";
            string seconds = RussianMetricFormatter.Seconds(minuteMetrics.DisplayWindowSeconds);
            L("RateHint").Text = minuteMetrics.DisplayRate.HasValue ? (minuteMetrics.IsFinal && minuteMetrics.IsPartial ? "итог за " : "средняя за ") + seconds + " с" : minuteMetrics.IsFinal ? "итог · окно не измерено" : "средняя за 60 с";
            L("CountHint").Text = minuteMetrics.IsFinal ? "итог текущего запуска" : "за текущий запуск · 60 с";
            L("RateValue").ToolTip = minuteMetrics.DisplayRate.HasValue ? RussianMetricFormatter.Full(minuteMetrics.DisplayRate.Value) + " ключей/с · окно " + seconds + " с" : "Ожидание минутного интервала для расчёта средней скорости.";
            L("CountValue").ToolTip = RussianMetricFormatter.FullCount(minuteMetrics.DisplayCount) + " ключей за текущий запуск; прогресс чекпоинта учитывает движок.";
        }
        static string Metric(string value)
        {
            if (value.Length == 0) return "—";
            double n; if (!Double.TryParse(value,NumberStyles.Float,CultureInfo.InvariantCulture,out n) || Double.IsNaN(n) || Double.IsInfinity(n) || n < 0) return "—";
            if (n >= 1000000000000) return (n/1000000000000).ToString("0.##",CultureInfo.CurrentCulture) + "T";
            if (n >= 1000000000) return (n/1000000000).ToString("0.##",CultureInfo.CurrentCulture) + "G";
            if (n >= 1000000) return (n/1000000).ToString("0.##",CultureInfo.CurrentCulture) + "M";
            if (n >= 1000) return (n/1000).ToString("0.##",CultureInfo.CurrentCulture) + "k";
            return n.ToString("0.##",CultureInfo.CurrentCulture);
        }
        void AppendLog(string line)
        {
            TextBox journal = T("Journal"); journal.AppendText(DateTime.Now.ToString("HH:mm:ss") + "  " + line + Environment.NewLine);
            if (journal.Text.Length > 70000) journal.Text = journal.Text.Substring(journal.Text.Length-50000);
            journal.ScrollToEnd();
        }
        void OpenJournal()
        {
            Safe(delegate
            {
                string folder = controller.SessionDirectory;
                if (String.IsNullOrEmpty(folder)) throw new InvalidOperationException("Журнал файла появится после запуска сеанса.");
                string path = Path.Combine(folder,"journal_ui.txt"); File.WriteAllText(path,T("Journal").Text,new UTF8Encoding(false));
                Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
            });
        }
        void Closing(object sender,System.ComponentModel.CancelEventArgs e)
        {
            if (!controller.IsActive) { pollTimer.Stop(); controller.Dispose(); return; }
            e.Cancel = true;
            if (closingRequested) return;
            MessageBoxResult choice = MessageBox.Show(window,"Завершить текущий поиск и закрыть пульт после сохранения состояния?","Закрытие пульта",MessageBoxButton.YesNo,MessageBoxImage.Question);
            if (choice == MessageBoxResult.Yes)
            {
                closingRequested = true;
                resumeAfterWindowClose = currentRunConfirmed && profile.AcceptedRun != null && profile.EffectiveResume != null;
                profile.ResumeDesired = resumeAfterWindowClose; SaveProfileQuiet(); stopQueued = true;
                try { controller.Command("stop",0); powerTimer.Stop(); L("FooterMessage").Text = "Ожидание безопасного завершения поиска перед закрытием…"; }
                catch (Exception ex) { closingRequested = false; L("ValidationMessage").Text = ex.Message; L("ValidationMessage").Visibility = Visibility.Visible; }
            }
        }
    }

    public static class Program
    {
        static Mutex instanceMutex;
        [STAThread]
        public static int Main(string[] args)
        {
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            string root = baseDir, engine = Path.Combine(baseDir,"pubkeyhunt_cuda_verified.exe"), fixture = null;
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--app-root" && i+1 < args.Length) root = Path.GetFullPath(args[++i]);
                else if (args[i] == "--engine" && i+1 < args.Length) engine = Path.GetFullPath(args[++i]);
                else if (args[i] == "--fixture" && i+1 < args.Length) fixture = Path.GetFullPath(args[++i]);
            }
            try
            {
                if (fixture == null)
                {
                    string identity;
                    using (SHA256 sha = SHA256.Create()) identity = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(Path.GetFullPath(engine).ToLowerInvariant()))).Replace("-","");
                    bool created; instanceMutex = new Mutex(true,"Local\\PubKEYS-Pult-" + identity,out created);
                    if (!created) { MessageBox.Show("Пульт для этого двигателя уже открыт.","PubKEYS",MessageBoxButton.OK,MessageBoxImage.Information); instanceMutex.Dispose(); return 0; }
                }
                Application app = new Application(); app.ShutdownMode = ShutdownMode.OnMainWindowClose;
                PultWindow pult = new PultWindow(root,engine,fixture); app.Run(pult.Window);
                if (instanceMutex != null) { instanceMutex.ReleaseMutex(); instanceMutex.Dispose(); } return 0;
            }
            catch (Exception ex) { MessageBox.Show("Не удалось открыть пульт:\n" + ex.Message,"PubKEYS",MessageBoxButton.OK,MessageBoxImage.Error); return 1; }
        }
    }
}
