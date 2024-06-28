using System.Runtime.InteropServices;
using UnrealSharp.Attributes;

namespace UnrealSharp.Engine;

[StructLayout(LayoutKind.Sequential)]
[UStruct(IsBlittable = true)]
public partial struct TimerHandle
{
    private const uint IndexBits = 24;
    private const uint SerialNumberBits = 40;
    
    private const int MaxIndex = 1 << (int)IndexBits;
    private const ulong MaxSerialNumber = 1UL << (int)SerialNumberBits;
    
    private ulong Handle;

    public TimerHandle()
    {
        Handle = 0;
    }

    public bool IsValid()
    {
        return Handle != 0;
    }

    public override bool Equals(object obj)
    {
        if (obj is TimerHandle other)
        {
            return Handle == other.Handle;
        }
        return false;
    }

    public override int GetHashCode()
    {
        return Handle.GetHashCode();
    }

    public static bool operator ==(TimerHandle left, TimerHandle right)
    {
        return left.Handle == right.Handle;
    }

    public static bool operator !=(TimerHandle left, TimerHandle right)
    {
        return left.Handle != right.Handle;
    }
    public override string ToString()
    {
        return Handle.ToString();
    }
}