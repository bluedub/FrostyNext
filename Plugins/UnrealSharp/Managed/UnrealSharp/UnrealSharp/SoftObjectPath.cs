using System.Runtime.InteropServices;
using UnrealSharp.CoreUObject;

namespace UnrealSharp;

[StructLayout(LayoutKind.Sequential)]
public class SoftObjectPath
{
    private readonly TopLevelAssetPath AssetPath;
    private UnmanagedArray SubPathString;
    
    public override bool Equals(object obj)
    {
        if (ReferenceEquals(null, obj)) return false;
        return obj.GetType() == GetType() && Equals((SoftObjectPath)obj);
    }
    public override int GetHashCode()
    {
        return AssetPath.GetHashCode();
    }
    public static bool operator == (SoftObjectPath a, SoftObjectPath b)
    {
        if (a == null || b == null)
        {
            return true;
        }
        
        return a.AssetPath == b.AssetPath;
    }

    public static bool operator != (SoftObjectPath a, SoftObjectPath b)
    {
        return !(a == b);
    }

    public UnrealSharpObject ResolveObject()
    {
        if (AssetPath.IsNull())
        {
            return default;
        }

        return default;
    }
}