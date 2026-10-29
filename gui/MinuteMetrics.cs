using System;
using System.Diagnostics;
using System.Globalization;

namespace PubKEYS.Pult
{
    public interface IMonotonicClock { double Seconds { get; } }
    public sealed class StopwatchMonotonicClock : IMonotonicClock
    {
        readonly Stopwatch watch = Stopwatch.StartNew();
        public double Seconds { get { return watch.Elapsed.TotalSeconds; } }
    }

    // UI sampling only: the worker's counter/search cadence is unchanged.
    public sealed class MinuteMetricsCollector
    {
        readonly IMonotonicClock clock;
        string workerId;
        int revision;
        bool hasBaseline, finalized;
        decimal baselineCount, lastCount;
        double baselineSeconds, lastSeconds;
        public string DisplayCount { get; private set; }
        public double? DisplayRate { get; private set; }
        public double DisplayWindowSeconds { get; private set; }
        public bool IsPartial { get; private set; }
        public bool IsFinal { get; private set; }
        public MinuteMetricsCollector(IMonotonicClock monotonicClock)
        {
            if (monotonicClock == null) throw new ArgumentNullException("monotonicClock");
            clock = monotonicClock; DisplayCount = "—";
        }
        public void Reset()
        {
            workerId = null; revision = 0; hasBaseline = finalized = false;
            DisplayCount = "0"; DisplayRate = null; DisplayWindowSeconds = 0; IsPartial = IsFinal = false;
        }
        public void Observe(string worker,int actualRevision,string phase,string cumulativeCount)
        {
            if (String.IsNullOrEmpty(worker) || actualRevision < 1) return;
            double now = clock.Seconds;
            if (Double.IsNaN(now) || Double.IsInfinity(now) || now < 0) return;
            bool newWorker = workerId != worker;
            if (newWorker) { Reset(); workerId = worker; revision = actualRevision; }
            bool final = phase == "stopped" || phase == "completed" || phase == "error";
            bool ready = phase == "running" || phase == "paused" || phase == "pausing" || phase == "stopping";
            if (!ready && !final) return; // Do not average mode/database preparation time.
            decimal count;
            if (!Decimal.TryParse(cumulativeCount,NumberStyles.None,CultureInfo.InvariantCulture,out count) || count < 0) return;
            if (revision != actualRevision)
            {
                revision = actualRevision; hasBaseline = finalized = false;
                DisplayRate = null; DisplayWindowSeconds = 0; IsPartial = IsFinal = false;
                // Count remains the last minute's cumulative snapshot until the
                // next minute; the new rate window starts with the new mode.
            }
            if (hasBaseline && now < lastSeconds) return;
            if (finalized)
            {
                if (final) return;
                hasBaseline = finalized = false; DisplayRate = null; DisplayWindowSeconds = 0; IsPartial = IsFinal = false;
            }
            if (!hasBaseline || count < lastCount)
            {
                if (hasBaseline && count < lastCount) { DisplayCount = "0"; DisplayRate = null; DisplayWindowSeconds = 0; IsPartial = IsFinal = false; }
                baselineCount = lastCount = count; baselineSeconds = lastSeconds = now; hasBaseline = true;
                if (!final) return;
                DisplayCount = count.ToString("0",CultureInfo.InvariantCulture);
                DisplayRate = null; DisplayWindowSeconds = 0; IsPartial = IsFinal = finalized = true; return;
            }
            lastCount = count; lastSeconds = now;
            double elapsed = now - baselineSeconds;
            if (elapsed < 60 && !final) return;
            DisplayCount = count.ToString("0",CultureInfo.InvariantCulture);
            if (elapsed > 0)
            {
                double rate = (double)(count - baselineCount) / elapsed;
                DisplayRate = Double.IsNaN(rate) || Double.IsInfinity(rate) ? (double?)null : rate;
                DisplayWindowSeconds = elapsed; IsPartial = elapsed < 60;
            }
            else if (!DisplayRate.HasValue) { DisplayWindowSeconds = 0; IsPartial = final; }
            IsFinal = final; finalized = final;
            baselineCount = count; baselineSeconds = now;
        }
    }

    public static class RussianMetricFormatter
    {
        static readonly NumberFormatInfo format = new NumberFormatInfo { NumberGroupSeparator = " ", NumberDecimalSeparator = ",", NumberGroupSizes = new int[] { 3 } };
        public static string Compact(double value,out string unit)
        {
            if (Double.IsNaN(value) || Double.IsInfinity(value) || value < 0) { unit = ""; return "—"; }
            double scale;
            if (value >= 1e12) { scale = 1e12; unit = "трлн"; }
            else if (value >= 1e9) { scale = 1e9; unit = "млрд"; }
            else if (value >= 1e6) { scale = 1e6; unit = "млн"; }
            else if (value >= 1e3) { scale = 1e3; unit = "тыс."; }
            else { scale = 1; unit = ""; }
            return (value / scale).ToString(scale == 1 ? "0.##" : "0.00",format);
        }
        public static string Full(double value) { return Double.IsNaN(value) || Double.IsInfinity(value) || value < 0 ? "—" : value.ToString("N0",format); }
        public static string FullCount(string value)
        {
            decimal count; return Decimal.TryParse(value,NumberStyles.None,CultureInfo.InvariantCulture,out count) ? count.ToString("N0",format) : "—";
        }
        public static string Seconds(double value) { return value.ToString("0.#",format); }
    }
}
