using System;
using System.IO;
using System.Runtime.InteropServices;

namespace RoundedTB
{
    /// <summary>
    /// Loads RoundedTB.Tap.dll into Explorer and drives it with window messages. The DLL rounds the
    /// taskbar's button highlights and, separately, appends the live charge rate to the battery icon's hover name.
    /// See Tap.cpp for what happens inside Explorer.
    /// </summary>
    internal sealed class HighlightStyler : IDisposable
    {
        private const int WH_CALLWNDPROC = 4;

        private readonly uint applyMessage = RegisterWindowMessageW("RoundedTB.Tap.Apply");
        private readonly uint batteryMessage = RegisterWindowMessageW("RoundedTB.Tap.Battery");
        private IntPtr module;
        private IntPtr hook;
        private IntPtr hookedTaskbar;

        /// <summary>Why the styler can't run, or null when it can.</summary>
        public string Unavailable { get; }

        public HighlightStyler()
        {
            // A hook DLL has to match Explorer's architecture, which is the OS's.
            if (RuntimeInformation.ProcessArchitecture != RuntimeInformation.OSArchitecture)
            {
                Unavailable = "Needs the build of RoundedTB that matches your PC's architecture.";
                return;
            }
            string path = Path.Combine(AppContext.BaseDirectory, "RoundedTB.Tap.dll");
            module = LoadLibraryW(path);
            if (module == IntPtr.Zero)
            {
                Unavailable = "RoundedTB.Tap.dll is missing.";
            }
        }

        /// <summary>Applies <paramref name="radius"/> (DIPs) to every button highlight; below zero restores Explorer's own.</summary>
        public void Apply(double radius)
        {
            if (!EnsureHooked())
            {
                return;
            }
            SendMessageW(hookedTaskbar, applyMessage, new IntPtr((long)Math.Round(radius * 100)), IntPtr.Zero);
        }

        /// <summary>Appends the charge rate to the battery hover name, or gives that name back to Explorer.</summary>
        public void ApplyBatteryWattage(bool enabled)
        {
            if (!EnsureHooked())
            {
                return;
            }
            SendMessageW(hookedTaskbar, batteryMessage, new IntPtr(enabled ? 1 : 0), IntPtr.Zero);
        }

        private bool EnsureHooked()
        {
            if (Unavailable != null)
            {
                return false;
            }
            IntPtr taskbar = FindWindowW("Shell_TrayWnd", null);
            if (taskbar == IntPtr.Zero)
            {
                return false;
            }
            // A new taskbar window means Explorer restarted, and the DLL went with it.
            if (taskbar != hookedTaskbar)
            {
                Unhook();
                uint thread = GetWindowThreadProcessId(taskbar, out _);
                hook = SetWindowsHookExW(WH_CALLWNDPROC, GetProcAddress(module, "HookProc"), module, thread);
                hookedTaskbar = hook != IntPtr.Zero ? taskbar : IntPtr.Zero;
            }
            return hookedTaskbar != IntPtr.Zero;
        }

        private void Unhook()
        {
            if (hook != IntPtr.Zero)
            {
                UnhookWindowsHookEx(hook);
                hook = IntPtr.Zero;
            }
            hookedTaskbar = IntPtr.Zero;
        }

        public void Dispose() => Unhook();

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr SetWindowsHookExW(int id, IntPtr proc, IntPtr module, uint threadId);

        [DllImport("user32.dll")]
        private static extern bool UnhookWindowsHookEx(IntPtr hook);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr FindWindowW(string className, string windowName);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern uint RegisterWindowMessageW(string name);

        [DllImport("user32.dll")]
        private static extern IntPtr SendMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    }
}
