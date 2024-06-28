using UnrealSharp.CoreUObject;

namespace UnrealSharp.Interop;

[NativeCallbacks]
public static unsafe partial class FRotatorExporter
{
    public static delegate* unmanaged<out Rotator, ref Quat, void> FromQuat;
    public static delegate* unmanaged<out Rotator, ref Matrix, void> FromMatrix;
}