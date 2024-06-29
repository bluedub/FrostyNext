using System.Diagnostics;
using System.Reflection;
using Frosty.Sdk;
using Frosty.Sdk.Managers;
using Frosty.Sdk.Sdk;
using Frosty.Sdk.Utils;
using UnrealSharp.Attributes;
using Object = UnrealSharp.CoreUObject.Object;

namespace ManagedFrostyNext;

[UClass]
public class FUStartupAction : Object
{
    FUStartupAction()
    {
        AppDomain.CurrentDomain.AssemblyResolve += CurrentDomain_AssemblyResolve;
    }
    
    [UFunction(FunctionFlags.BlueprintCallable)]
    void Start(string gamePath, int pid)
    {
        FileInfo game = new(gamePath);
        if (!game.Exists)
        {
            FULogger.LogErrorInternal("Game does not exist");
            return;
        }
        
        LoadGame(game, pid);
    }
    
    private static void LoadGame(FileInfo inGameFileInfo, int pid)
    {
        if (!inGameFileInfo.Exists)
        {
            FULogger.LogErrorInternal("No game exists at that path");
            return;
        }

        // set logger
        FrostyLogger.Logger = new FULogger();

        // set base directory to the directory containing the executable
        Utils.BaseDirectory = Path.GetDirectoryName(AppContext.BaseDirectory) ?? string.Empty;

        // init profile
        if (!ProfilesLibrary.Initialize(Path.GetFileNameWithoutExtension(inGameFileInfo.Name)))
        {
            return;
        }

        if (inGameFileInfo.DirectoryName is null)
        {
            FULogger.LogErrorInternal("The game needs to be in a directory containing the games data");
            return;
        }

        // init filesystem manager, this parses the layout.toc file
        if (!FileSystemManager.Initialize(inGameFileInfo.DirectoryName))
        {
            return;
        }

        // generate sdk if needed
        if (!File.Exists(ProfilesLibrary.SdkPath))
        {
            TypeSdkGenerator typeSdkGenerator = new();

            Process game = Process.GetProcessById(pid);

            if (!typeSdkGenerator.DumpTypes(game))
            {
                return;
            }

            if (!typeSdkGenerator.CreateSdk(ProfilesLibrary.SdkPath))
            {
                return;
            }
        }

        // init type library, this loads the EbxTypeSdk used to properly parse ebx assets
        if (!TypeLibrary.Initialize())
        {
            return;
        }

        // init resource manager, this parses the cas.cat files if they exist for easy asset lookup
        if (!ResourceManager.Initialize())
        {
            return;
        }

        // init asset manager, this parses the SuperBundles and loads all the assets
        if (!AssetManager.Initialize())
        {
        }
    }
    
    private static Assembly CurrentDomain_AssemblyResolve(object sender, ResolveEventArgs args)
    {
        string dllname = args.Name.Contains(",") ? args.Name.Substring(0, args.Name.IndexOf(',')) : args.Name;
        
        return Assembly.LoadFile($"{AppContext.BaseDirectory}{dllname}.dll");
    }
}