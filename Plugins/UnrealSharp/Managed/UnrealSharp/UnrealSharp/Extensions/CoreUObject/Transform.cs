using System.Runtime.CompilerServices;

namespace UnrealSharp.CoreUObject;

public partial struct Transform
{
    public Transform(Quat rotation, Vector location, Vector scale) : this()
    {
        Rotation = rotation;
        Translation = location;
        Scale3D = scale;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Vector TransformPosition(Vector v)
    {
        return Rotation.RotateVector(Scale3D * v) + Translation;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Vector TransformPositionNoScale(Vector v)
    {
        return Rotation.RotateVector(v) + Translation;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Vector TransformVector(Vector v)
    {
        return Rotation.RotateVector(Scale3D * v);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Vector TransformVectorNoScale(Vector v)
    {
        return Rotation.RotateVector(v);
    }

    public static readonly Transform ZeroTransform = new(Quat.Identity, Vector.Zero, Vector.Zero);
    public static readonly Transform Identity = new(Quat.Identity, Vector.Zero, Vector.One);
    
    public bool Equals(Transform other)
    {
        return Rotation.Equals(other.Rotation) && Translation.Equals(other.Translation) && Scale3D.Equals(other.Scale3D);
    }

    public override bool Equals(object obj)
    {
        return obj is Transform other && Equals(other);
    }

    public override int GetHashCode()
    {
        return HashCode.Combine(Rotation, Translation, Scale3D);
    }

    public override string ToString()
    {
        return $"Location: {Translation}, Rotation: {Rotation}, Scale: {Scale3D}";
    }

    public static bool operator ==(Transform left, Transform right) => left.Rotation == right.Rotation && left.Translation == right.Translation && left.Scale3D == right.Scale3D;
    public static bool operator !=(Transform left, Transform right) => !(left == right);
}
