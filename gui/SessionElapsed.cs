using System;
using System.Globalization;

namespace PubKEYS.Pult
{
    public sealed class SessionElapsedClock
    {
        readonly IMonotonicClock clock;
        string workerId;
        ulong seconds;
        double startedAt;
        bool started, hasAnchor, hasValue, frozen;
        public string Text { get { return Format(seconds); } }
        public bool IsFinal { get { return frozen; } }
        public SessionElapsedClock(IMonotonicClock monotonicClock)
        {
            if (monotonicClock == null) throw new ArgumentNullException("monotonicClock");
            clock = monotonicClock;
        }
        public void Reset()
        {
            workerId = null; seconds = 0; started = hasAnchor = hasValue = frozen = false;
        }
        public void Start(string worker)
        {
            Reset(); workerId = worker; started = true; startedAt = clock.Seconds;
            hasAnchor = !Double.IsNaN(startedAt) && !Double.IsInfinity(startedAt) && startedAt >= 0;
        }
        public void Observe(string worker,string phase,string serverElapsed)
        {
            if (String.IsNullOrEmpty(worker)) return;
            if (workerId != worker) { Reset(); workerId = worker; }
            if (frozen) return;
            ulong serverSeconds;
            bool valid = UInt64.TryParse(serverElapsed,NumberStyles.None,CultureInfo.InvariantCulture,out serverSeconds);
            bool terminal = phase == "completed" || phase == "stopped" || phase == "error";
            if (terminal)
            {
                if (valid) { seconds = serverSeconds; hasValue = true; }
                if (hasValue || started) frozen = true;
                return;
            }
            if (valid) { seconds = Math.Max(seconds,serverSeconds); hasValue = true; }
            if (started && hasAnchor)
            {
                double elapsed = clock.Seconds - startedAt;
                if (!Double.IsNaN(elapsed) && !Double.IsInfinity(elapsed) && elapsed >= 0)
                {
                    ulong projected = elapsed >= UInt64.MaxValue ? UInt64.MaxValue : (ulong)Math.Floor(elapsed);
                    seconds = Math.Max(seconds,projected); hasValue = true;
                }
            }
        }
        public static bool TryFormat(string value,out string formatted)
        {
            ulong parsed; formatted = null;
            if (!UInt64.TryParse(value,NumberStyles.None,CultureInfo.InvariantCulture,out parsed)) return false;
            formatted = Format(parsed); return true;
        }
        static string Format(ulong value)
        {
            return (value / 3600).ToString("D2",CultureInfo.InvariantCulture) + ":" + ((value / 60) % 60).ToString("D2",CultureInfo.InvariantCulture) + ":" + (value % 60).ToString("D2",CultureInfo.InvariantCulture);
        }
    }
}
