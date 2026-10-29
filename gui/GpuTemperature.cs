using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Media;
using System.Windows.Threading;

namespace PubKEYS.Pult
{
    public sealed class GpuTemperatureReading
    {
        public string Name = "", Error = "";
        public int? Celsius;
        public DateTime UpdatedAt = DateTime.Now;
    }
    public interface IGpuTemperatureSource { GpuTemperatureReading Read(); }

    public sealed class UnavailableGpuTemperatureSource : IGpuTemperatureSource
    {
        public GpuTemperatureReading Read() { return new GpuTemperatureReading { Error = "Проверка интерфейса · датчик не опрашивается" }; }
    }

    // NVML is a read-only management query here. No CUDA library/context or
    // power/clock/temperature-limit setters are loaded or called.
    public sealed class NvmlGpuTemperatureSource : IGpuTemperatureSource
    {
        [DllImport("kernel32.dll",CharSet = CharSet.Unicode,SetLastError = true)]
        static extern IntPtr LoadLibraryEx(string path,IntPtr reserved,uint flags);
        [DllImport("kernel32.dll",CharSet = CharSet.Ansi,ExactSpelling = true)]
        static extern IntPtr GetProcAddress(IntPtr library,string name);
        [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr library);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Init();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Shutdown();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetCount(out uint count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetHandle(uint index,out IntPtr handle);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetName(IntPtr handle,StringBuilder name,uint length);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetTemperature(IntPtr handle,uint sensor,out uint temperature);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr ErrorString(int result);
        static T Function<T>(IntPtr library,string name) where T : class
        {
            IntPtr address = GetProcAddress(library,name);
            if (address == IntPtr.Zero) throw new InvalidOperationException("Драйвер не предоставляет " + name + ".");
            return (T)(object)Marshal.GetDelegateForFunctionPointer(address,typeof(T));
        }
        public GpuTemperatureReading Read()
        {
            GpuTemperatureReading reading = new GpuTemperatureReading();
            IntPtr library = IntPtr.Zero; Shutdown shutdown = null; bool initialized = false;
            try
            {
                string path = Path.Combine(Environment.SystemDirectory,"nvml.dll");
                library = LoadLibraryEx(path,IntPtr.Zero,0x00000800); // LOAD_LIBRARY_SEARCH_SYSTEM32
                if (library == IntPtr.Zero) throw new InvalidOperationException("NVML недоступна · проверьте драйвер NVIDIA.");
                Init init = Function<Init>(library,"nvmlInit_v2"); shutdown = Function<Shutdown>(library,"nvmlShutdown");
                ErrorString error = Function<ErrorString>(library,"nvmlErrorString");
                Check(init(),error); initialized = true;
                uint count; Check(Function<GetCount>(library,"nvmlDeviceGetCount_v2")(out count),error);
                if (count == 0) throw new InvalidOperationException("GPU NVIDIA не обнаружена.");
                IntPtr handle; Check(Function<GetHandle>(library,"nvmlDeviceGetHandleByIndex_v2")(0,out handle),error);
                StringBuilder name = new StringBuilder(128); Check(Function<GetName>(library,"nvmlDeviceGetName")(handle,name,128),error);
                reading.Name = "GPU 0 · " + name.ToString();
                uint temperature; Check(Function<GetTemperature>(library,"nvmlDeviceGetTemperature")(handle,0,out temperature),error);
                if (temperature > 150) throw new InvalidOperationException("Датчик вернул недопустимое значение.");
                reading.Celsius = (int)temperature;
            }
            catch (Exception ex) { reading.Celsius = null; reading.Error = ex.Message; }
            finally
            {
                if (initialized && shutdown != null) try { shutdown(); } catch { }
                if (library != IntPtr.Zero) FreeLibrary(library);
            }
            reading.UpdatedAt = DateTime.Now; return reading;
        }
        static void Check(int result,ErrorString error)
        {
            if (result != 0) throw new InvalidOperationException("NVML: " + (Marshal.PtrToStringAnsi(error(result)) ?? result.ToString()) + ".");
        }
    }

    public sealed class GpuTemperatureMonitor : IDisposable
    {
        readonly IGpuTemperatureSource source;
        readonly DispatcherTimer timer;
        readonly Dispatcher dispatcher;
        int busy, disposed;
        public GpuTemperatureReading Snapshot { get; private set; }
        public bool IsPolling { get { return Interlocked.CompareExchange(ref busy,0,0) != 0; } }
        public bool IsDisposed { get { return Interlocked.CompareExchange(ref disposed,0,0) != 0; } }
        public bool IsRunning { get { return timer.IsEnabled; } }
        public event Action Updated;
        public GpuTemperatureMonitor(IGpuTemperatureSource temperatureSource,bool startPolling = true)
        {
            if (temperatureSource == null) throw new ArgumentNullException("temperatureSource"); source = temperatureSource;
            dispatcher = Dispatcher.CurrentDispatcher;
            timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) }; timer.Tick += delegate { RefreshNow(); };
            if (startPolling) Start();
        }
        public void Start() { if (!IsDisposed && !timer.IsEnabled) { timer.Start(); RefreshNow(); } }
        public static bool IsValid(GpuTemperatureReading reading)
        {
            return reading != null && reading.Celsius.HasValue && reading.Celsius.Value >= 0 && reading.Celsius.Value <= 150 && String.IsNullOrEmpty(reading.Error);
        }
        public void RefreshNow()
        {
            if (IsDisposed || Interlocked.CompareExchange(ref busy,1,0) != 0) return;
            ThreadPool.QueueUserWorkItem(delegate
            {
                try
                {
                    GpuTemperatureReading reading;
                    try { reading = source.Read() ?? new GpuTemperatureReading { Error = "Датчик не вернул данные." }; }
                    catch (Exception ex) { reading = new GpuTemperatureReading { Error = ex.Message }; }
                    if (IsDisposed || dispatcher.HasShutdownStarted) return;
                    dispatcher.BeginInvoke(new Action(delegate
                    {
                        try { if (!IsDisposed) { Snapshot = reading; if (Updated != null) Updated(); } }
                        finally { Interlocked.Exchange(ref busy,0); }
                    }));
                }
                catch (InvalidOperationException) { Interlocked.Exchange(ref busy,0); }
                finally { if (IsDisposed) Interlocked.Exchange(ref busy,0); }
            });
        }
        public void Dispose() { if (Interlocked.Exchange(ref disposed,1) == 0) timer.Stop(); }
    }

    public static class GpuTemperatureAppearance
    {
        public static string FormatTemperature(int? celsius)
        {
            return celsius.HasValue && celsius.Value >= 0 && celsius.Value <= 150 ? celsius.Value.ToString() + " °C" : "—";
        }
        public static Color ColorFor(int? celsius)
        {
            if (!celsius.HasValue || celsius.Value < 0 || celsius.Value > 150) return Color.FromRgb(104,127,133);
            Color normal = Color.FromRgb(57,125,130), burgundy = Color.FromRgb(128,0,32), red = Color.FromRgb(239,35,60);
            if (celsius.Value <= 70) return normal;
            if (celsius.Value <= 80) return Blend(normal,burgundy,(celsius.Value - 70) / 10.0);
            return Blend(burgundy,red,Math.Min(1,(celsius.Value - 80) / 20.0));
        }
        static Color Blend(Color a,Color b,double t)
        {
            return Color.FromRgb((byte)Math.Round(a.R+(b.R-a.R)*t),(byte)Math.Round(a.G+(b.G-a.G)*t),(byte)Math.Round(a.B+(b.B-a.B)*t));
        }
    }
}
