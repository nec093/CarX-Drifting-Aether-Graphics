using System;
using System.Collections.Generic;
using System.IO;
using Aether;
using UnityEngine;

namespace AetherGraphics {
	/// <summary>
	/// Entry point under Aether Loader (an Aether mod: mod.json beside this DLL). Its files live in the mod's folder
	/// AetherLoader\mods\AetherGraphics (this DLL, native\, shaders\, hdrp\; logs, captures and benchmarks are written
	/// there too), its settings in AetherLoader\config\nec093.aether.graphics. The core runs on the object the loader gives
	/// the mod; it shuts itself down when the game quits.
	/// </summary>
	public sealed class AetherGraphicsMod : IAetherMod, IAetherHost {
		private ModContext context_;

		public string LoaderName { get; private set; }
		public string DataDir => context_.Directory;
		public string ConfigDir => context_.ConfigDirectory;
		public string GameRoot { get; } = Path.GetDirectoryName(Application.dataPath);
		public void LogInfo(string message) { context_.Log.Info(message); }
		public void LogError(string message) { context_.Log.Error(message); }
		public void SetSuppressInput(bool suppress) { }

		public IEnumerable<object> LoadedMods() {
			foreach (ModInfo mod in context_.Mods.All) {
				if (mod.State == ModState.Loaded && mod.Id != context_.Info.Id) {
					yield return mod;
				}
			}
		}

		public void OnLoad(ModContext context) {
			context_ = context;
			LoaderName = "Aether Loader " + LoaderVersion() + " (Aether.API " + AetherApi.Version + ")";
			AetherCore.Create(this, context.Root);
		}

		public void OnUnload() {
			// (the core stops in its own OnApplicationQuit: the loader unloads mods only when the game quits)
		}

		private static string LoaderVersion() {
			foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies()) {
				if (assembly.GetName().Name == "AetherLoader") {
					Version v = assembly.GetName().Version;
					return v.Major + "." + v.Minor + "." + v.Build;
				}
			}
			return "?";
		}
	}
}
