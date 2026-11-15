using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using UnityEngine;

namespace AetherGraphics {
	/// <summary>
	/// Entry point without a mod loader: Aether's own version.dll (src/AetherGraphics.Loader) calls Start on a thread of its own,
	/// attached to Mono, early in the game's start. Start waits until the game's scripts are loaded, then moves to Unity's
	/// main thread (Awaitable.MainThreadAsync posts to Unity's main thread context) and starts the core there. Aether's files
	/// live in [game]\Aether (this DLL, native\, shaders\, hdrp\), its settings in [game]\Aether\config. KSL, if installed,
	/// runs on its own and does not load Aether.
	/// </summary>
	public static class AetherBootstrap {
		private static string root_;
		private static int started_;

		// (the loader's thread; returns once the start on the main thread is queued, or after giving up)
		public static void Start(string gameRoot) {
			root_ = gameRoot;
			var host = new StandaloneHost(gameRoot);
			for (int i = 0; i < 600 && !GameScriptsLoaded(); ++i) {
				Thread.Sleep(200);
			}
			Exception last = null;
			for (int i = 0; i < 600; ++i) {
				try {
					var awaiter = Awaitable.MainThreadAsync().GetAwaiter();
					awaiter.OnCompleted(OnMainThread);
					host.LogInfo("[Aether Graphics] start queued on Unity's main thread");
					return;
				} catch (Exception e) {
					last = e; // Unity's main thread context is not set yet
				}
				Thread.Sleep(200);
			}
			host.LogError("[Aether Graphics] Unity's main thread could not be reached: " + last);
		}

		private static bool GameScriptsLoaded() {
			foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies()) {
				if (assembly.GetName().Name == "Assembly-CSharp") {
					return true;
				}
			}
			return false;
		}

		private static void OnMainThread() {
			if (Interlocked.Exchange(ref started_, 1) != 0) {
				return;
			}
			var host = new StandaloneHost(root_);
			try {
				AetherCore.Create(host);
			} catch (Exception e) {
				host.LogError("[Aether Graphics] failed to start: " + e);
			}
		}
	}

	internal sealed class StandaloneHost : IAetherHost {
		private static readonly object logLock_ = new object();
		private readonly string logPath_;

		public StandaloneHost(string gameRoot) {
			GameRoot = gameRoot;
			DataDir = Path.Combine(gameRoot, "AetherGraphics");
			ConfigDir = Path.Combine(DataDir, "config");
			logPath_ = Path.Combine(DataDir, "logs", "loader.log");
		}

		public string LoaderName => "Aether Graphics loader (version.dll)";
		public string DataDir { get; }
		public string ConfigDir { get; }
		public string GameRoot { get; }
		public void LogInfo(string message) { Write("Info", message); }
		public void LogError(string message) { Write("Error", message); }
		public void SetSuppressInput(bool suppress) { }
		public IEnumerable<object> LoadedMods() { yield break; }

		// loader.managed.log (the native loader keeps loader.log open) and Unity's Player.log
		private void Write(string level, string message) {
			lock (logLock_) {
				try {
					File.AppendAllText(Path.ChangeExtension(logPath_, ".managed.log"), DateTime.Now.ToString("HH:mm:ss.fff") + " [" + level + "] " + message + Environment.NewLine);
				} catch (Exception) {
				}
			}
			if (level == "Error") {
				Debug.LogError(message);
			} else {
				Debug.Log(message);
			}
		}
	}
}
