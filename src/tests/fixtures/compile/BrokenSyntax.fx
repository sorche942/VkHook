// Must fail to compile: the parameter list is never closed.
float4 BrokenPS( : SV_Target
{
    return float4(0.0, 0.0, 0.0, 1.0);
}
