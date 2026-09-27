using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using UnrealBuildTool;

// L2-S0 (the plan §10; D-SLM3812). The runtime module: the
// USuperSLMModel asset, the import pipeline, and the status-mapping surface, built directly
// against Layer 1's C++ headers (D-SLM3812 RULING) and against Layer 1's own CPU sources,
// vendored at the pinned tag (../ThirdParty/SuperSLM/VENDORED_VERSION.txt) into
// ../ThirdParty/SuperSLM and compiled into this module from source
// with the engine's own toolchain via the Private/Vendored/SuperSLMVendored_*.cpp wrappers --
// a direct-compile pattern, never a prebuilt superslm library (§9 dim 4(d)).
public class SuperSLMUnreal : ModuleRules
{
	public SuperSLMUnreal(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Layer 1 forbids implicit FP contraction: its scalar/SIMD-tier bit-equality depends
		// on exact mul-then-add rounding (SuperSLM's own design, §6/§13; this plugin's own plan §2.1
		// "all three tiers plus the scalar reference are proven bit-identical"). Precise FP is
		// the only reliable way to guarantee this from a UE module -- source pragmas do not
		// stop backend fusion under fast-math (verified at the compiler for a sibling plugin in
		// the project's private history). D-SLM3833 / guard G4 depend on the plugin never forcing a SIMD tier or
		// otherwise perturbing Layer 1's own runtime dispatch; this is the FP-rounding half of
		// that same determinism contract.
		FPSemantics = FPSemanticsMode.Precise;

		// The vendored core is compiled into this module via Private/Vendored's wrapper file,
		// which #includes each Layer-1 .cpp directly -- unity builds would merge those
		// translation units with this module's own and is disabled for the same reason
		// SuperFAISSUnreal disables it.
		bUseUnity = false;

		// Layer 1's public C++ throw contract (src/bad_alloc_wrap.h, WrapBadAllocContract)
		// uses ordinary try/catch/throw unconditionally, not only under a test-only macro --
		// every public entry point on the C++ path narrows any exception to std::bad_alloc.
		// UE modules default to exceptions disabled; this module must enable them to compile
		// Layer 1's own sources at all, matching the engine's own documented escape hatch for
		// modules wrapping an exception-throwing third-party library.
		bEnableExceptions = true;

		// Layer 1 requires C++20 (CMakeLists.txt `set(CMAKE_CXX_STANDARD 20)`, D-SLM3812 rider
		// 5) -- set explicitly rather than inheriting whatever the engine's own per-platform
		// default is, so a future engine default change cannot silently drop this module below
		// the standard Layer 1's own headers require.
		CppStandard = CppStandardVersion.Cpp20;

		// T-2241 review C2 / guard G5 (plan §9 dim 11): SuperSLMStatusMapping.h's switch
		// promotes its own unhandled-enumerator warning to an error via a scoped #pragma (see
		// that header), NOT a module-wide UBT setting -- a module-wide
		// SwitchUnhandledEnumeratorWarningLevel = Error was tried first and broke the build:
		// this module also compiles Layer 1's OWN vendored sources (Private/Vendored/*.cpp),
		// which switch over SslmSectionType and other enums with their own, legitimate
		// non-exhaustive-by-design shapes this plugin may never edit (D-SLM3812: the vendored
		// tree is pinned and unaltered). The pragma keeps the promotion scoped to exactly the
		// one switch guard G5 names.

		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "ThirdParty", "SuperSLM", "include"));

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		// L2-S1 (plan §5.1): the SuperSLM trace channel and its CPU timing scopes, defined in
		// this module. UBT links a module only against its own direct dependencies (measured on
		// UnrealEditor-SuperSLMUnrealEditor.dll.rsp: neither Core's public TraceLog dependency
		// nor this module's reaches a dependent's link line), so a module that calls a Trace
		// entry point itself -- UE::Trace::ToggleChannel -- names TraceLog in its own Build.cs.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"TraceLog",
			// L2-S2 (plan §7 item 11): the determinism self-check reads its shipped reference
			// digest file (JSON) from this plugin's own Resources directory.
			"Json",
			"Projects",
		});

		// L2-S1 (plan §4): every save blob is tagged with the Layer-1 tag and commit this build
		// compiled. They are read from the vendored tree's own record rather than restated in
		// source, so a pin bump that rewrites VENDORED_VERSION.txt moves the tag with it.
		string VendoredVersionPath = Path.Combine(ModuleDirectory, "..", "ThirdParty", "SuperSLM", "VENDORED_VERSION.txt");
		string Layer1Tag = null;
		string Layer1Commit = null;
		foreach (string Line in File.ReadAllLines(VendoredVersionPath))
		{
			string Trimmed = Line.Trim();
			if (Layer1Tag == null && Trimmed.StartsWith("Tag:"))
			{
				Layer1Tag = Trimmed.Substring(4).Trim();
			}
			else if (Layer1Commit == null && Trimmed.StartsWith("Commit:"))
			{
				Layer1Commit = Trimmed.Substring(7).Trim();
			}
		}
		if (string.IsNullOrEmpty(Layer1Tag) || string.IsNullOrEmpty(Layer1Commit))
		{
			throw new BuildException("SuperSLMUnreal: could not read 'Tag:' and 'Commit:' from " + VendoredVersionPath);
		}
		PrivateDefinitions.Add("SUPERSLM_LAYER1_TAG=\"" + Layer1Tag + "\"");
		PrivateDefinitions.Add("SUPERSLM_LAYER1_COMMIT=\"" + Layer1Commit + "\"");

		// L2-S2 (plan §7 item 11): the self-check report and its prior-run key carry the plugin
		// version, read from the .uplugin descriptor so the report cannot drift from it.
		string PluginVersion = ReadPluginVersionName();
		PrivateDefinitions.Add("SUPERSLM_PLUGIN_VERSION=\"" + PluginVersion + "\"");

		// L2-S2 (plan §7 item 11): the shipped self-check reference digests (one JSON file per
		// artifact the plugin ships a reference for), staged with every packaged build.
		RuntimeDependencies.Add("$(PluginDir)/Resources/SelfCheck/*.json", StagedFileType.NonUFS);

		// L2-S2 (plan §10.3, D-SLM7337): the GPU backend compiles into THIS module, guarded to
		// Win64. Layer 1's GPU sources call its CPU-core symbols, which are compiled into this
		// module and exported from no DLL, so a separate module could not link them.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicDefinitions.Add("SUPERSLMUNREAL_WITH_GPU=1");
			// d3d12_harness.h and the GPU sources' own relative includes live here.
			PrivateIncludePaths.Add(Path.Combine(ThirdPartyRoot(), "src", "gpu"));
			PublicSystemLibraries.AddRange(new string[] { "d3d12.lib", "dxgi.lib", "dxguid.lib" });
			CompileAndStageGpuShaders();
		}
		else
		{
			PublicDefinitions.Add("SUPERSLMUNREAL_WITH_GPU=0");
		}
	}

	private string ThirdPartyRoot()
	{
		return Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "ThirdParty", "SuperSLM"));
	}

	private string ReadPluginVersionName()
	{
		string UPluginPath = Path.Combine(PluginDirectory, "SuperSLMUnreal.uplugin");
		Match VersionMatch = Regex.Match(File.ReadAllText(UPluginPath), "\"VersionName\"\\s*:\\s*\"([^\"]*)\"");
		if (!VersionMatch.Success)
		{
			throw new BuildException("SuperSLMUnreal: could not read VersionName from " + UPluginPath);
		}
		return VersionMatch.Groups[1].Value;
	}

	// The plugin runs no CMake, so it compiles every vendored .hlsl itself with Layer 1's own
	// flags (CMakeLists.txt and build.bat: -T cs_6_2 -E main -O3 -HV 2018 -WX) and stages each .cso
	// into the plugin's OWN Binaries/Win64/shaders directory (plan §2.5 row 1, D-SLM7639,
	// D-SLM7662). USuperSLMGpuSubsystem::Configure() passes that directory, absolute, as Layer
	// 1.7.0's GpuContextConfig::shader_dir, so Layer 1 never looks beside the host executable --
	// which an installed engine's editor does not let a project plugin write. The files are staged
	// NonUFS: Layer 1 opens them with Win32 file calls, so they stay loose files outside any pak.
	// The shader count follows the vendored tree (35 at 1.7.0); no list is restated here.
	private void CompileAndStageGpuShaders()
	{
		string ShaderSourceDir = Path.Combine(ThirdPartyRoot(), "src", "gpu", "shaders");
		string[] HlslFiles = Directory.GetFiles(ShaderSourceDir, "*.hlsl");
		string[] HlsliFiles = Directory.GetFiles(ShaderSourceDir, "*.hlsli");
		Array.Sort(HlslFiles, StringComparer.Ordinal);
		if (HlslFiles.Length == 0)
		{
			throw new BuildException("SuperSLMUnreal: no .hlsl found under " + ShaderSourceDir);
		}

		// A .cso is current when it is newer than its own .hlsl and every shared .hlsli, the same
		// freshness rule Layer 1's own ShaderBinaryStalenessDiagnostic enforces.
		DateTime NewestHeader = DateTime.MinValue;
		foreach (string Header in HlsliFiles)
		{
			DateTime T = File.GetLastWriteTimeUtc(Header);
			if (T > NewestHeader)
			{
				NewestHeader = T;
			}
		}

		string OutputDir = Path.Combine(PluginDirectory, "Intermediate", "SuperSLMShaders", "Win64");
		Directory.CreateDirectory(OutputDir);

		// UnrealBuildTool can construct these rules for two targets at once (a packaging build
		// does), and both would compile into the same .cso files. A named mutex per output
		// directory makes the second wait, and then find the files current and skip them.
		string MutexName;
		using (SHA256 Sha = SHA256.Create())
		{
			byte[] Hash = Sha.ComputeHash(Encoding.UTF8.GetBytes(Path.GetFullPath(OutputDir).ToLowerInvariant()));
			MutexName = "Local\\SuperSLMUnrealShaders_" + BitConverter.ToString(Hash, 0, 16).Replace("-", "");
		}
		using (Mutex Lock = new Mutex(false, MutexName))
		{
			try
			{
				Lock.WaitOne();
			}
			catch (AbandonedMutexException)
			{
				// A build that died holding the lock left it abandoned; the lock is still ours now.
			}
			try
			{
				CompileShadersLocked(HlslFiles, NewestHeader, OutputDir);
			}
			finally
			{
				Lock.ReleaseMutex();
			}
		}
	}

	private void CompileShadersLocked(string[] HlslFiles, DateTime NewestHeader, string OutputDir)
	{
		string Dxc = null;
		List<string> ShaderNames = new List<string>();
		foreach (string Hlsl in HlslFiles)
		{
			string Name = Path.GetFileNameWithoutExtension(Hlsl);
			ShaderNames.Add(Name);
			string Cso = Path.Combine(OutputDir, Name + ".cso");
			bool bCurrent = File.Exists(Cso)
				&& File.GetLastWriteTimeUtc(Cso) >= File.GetLastWriteTimeUtc(Hlsl)
				&& File.GetLastWriteTimeUtc(Cso) >= NewestHeader;
			if (!bCurrent)
			{
				if (Dxc == null)
				{
					Dxc = FindDxc();
				}
				CompileShader(Dxc, Hlsl, Cso);
			}
			RuntimeDependencies.Add("$(PluginDir)/Binaries/Win64/shaders/" + Name + ".cso", Cso, StagedFileType.NonUFS);
		}

		// The runtime's staging check (plan §2.5 row 1) verifies exactly this list, in the same
		// directory it passes to Layer 1, derived from the vendored sources rather than restated
		// in C++.
		PrivateDefinitions.Add("SUPERSLMUNREAL_GPU_SHADER_NAMES=\"" + string.Join(",", ShaderNames) + "\"");
		PrivateDefinitions.Add("SUPERSLMUNREAL_GPU_SHADER_COUNT=" + ShaderNames.Count);
	}

	// The DirectX Shader Compiler from the Windows SDK. SUPERSLM_DXC names one explicitly;
	// otherwise the SDK version Layer 1's own build.bat pins (10.0.19041.0) is preferred, so the
	// .cso bytes come from the same compiler the certified measurements used, then the newest
	// installed SDK's.
	private static string FindDxc()
	{
		string Override = Environment.GetEnvironmentVariable("SUPERSLM_DXC");
		if (!string.IsNullOrEmpty(Override))
		{
			if (!File.Exists(Override))
			{
				throw new BuildException("SuperSLMUnreal: SUPERSLM_DXC names a file that does not exist: " + Override);
			}
			return Override;
		}

		string KitsBin = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Windows Kits", "10", "bin");
		string Pinned = Path.Combine(KitsBin, "10.0.19041.0", "x64", "dxc.exe");
		if (File.Exists(Pinned))
		{
			return Pinned;
		}
		if (Directory.Exists(KitsBin))
		{
			List<string> Versions = new List<string>(Directory.GetDirectories(KitsBin, "10.*"));
			Versions.Sort(StringComparer.Ordinal);
			Versions.Reverse();
			foreach (string VersionDir in Versions)
			{
				string Candidate = Path.Combine(VersionDir, "x64", "dxc.exe");
				if (File.Exists(Candidate))
				{
					return Candidate;
				}
			}
		}
		throw new BuildException("SuperSLMUnreal: dxc.exe (the DirectX Shader Compiler) was not found under " + KitsBin +
			". Install the Windows 10/11 SDK, or set SUPERSLM_DXC to a dxc.exe, to build the GPU backend's compute shaders.");
	}

	// dxc writes a temporary file beside the .cso, which is moved into place only when dxc
	// succeeds, so an interrupted compile never leaves a partial .cso that reads as current.
	private static void CompileShader(string Dxc, string Hlsl, string Final)
	{
		string Cso = Final + "." + Process.GetCurrentProcess().Id + ".tmp";
		ProcessStartInfo Info = new ProcessStartInfo(Dxc)
		{
			Arguments = "-T cs_6_2 -E main -Fo \"" + Cso + "\" \"" + Hlsl + "\" -O3 -HV 2018 -WX",
			UseShellExecute = false,
			RedirectStandardOutput = true,
			RedirectStandardError = true,
			CreateNoWindow = true,
		};
		using (Process P = Process.Start(Info))
		{
			string StdOut = P.StandardOutput.ReadToEnd();
			string StdErr = P.StandardError.ReadToEnd();
			P.WaitForExit();
			if (P.ExitCode != 0 || !File.Exists(Cso))
			{
				if (File.Exists(Cso))
				{
					File.Delete(Cso);
				}
				throw new BuildException("SuperSLMUnreal: dxc failed on " + Hlsl + " (exit " + P.ExitCode + "):\n" + StdOut + StdErr);
			}
		}
		File.Move(Cso, Final, true);
		Console.WriteLine("SuperSLMUnreal: dxc " + Path.GetFileName(Hlsl) + " -> " + Final);
	}
}
