.class public Ldemo/Polymorphic;
.super Ljava/lang/Object;

# The handle is v1; v0 has no value before the call. Reversing the 45cc
# nibbles used to read v0 as the receiver and crash the entire source job.
.method public static booleanValue(Ljava/lang/invoke/MethodHandle;)Z
    .registers 2
    invoke-polymorphic {p0}, Ljava/lang/invoke/MethodHandle;->invokeExact([Ljava/lang/Object;)Ljava/lang/Object;, ()Z
    move-result v0
    return v0
.end method

.method public static argumentOrder(Ljava/lang/invoke/MethodHandle;)I
    .registers 6
    const/16 v3, 11
    const/16 v2, 22
    const/16 v1, 33
    const/16 v4, 44
    invoke-polymorphic {p0, v3, v2, v1, v4}, Ljava/lang/invoke/MethodHandle;->invokeExact([Ljava/lang/Object;)Ljava/lang/Object;, (IIII)I
    move-result v0
    return v0
.end method

.method public static wideResult(Ljava/lang/invoke/MethodHandle;)J
    .registers 8
    move-object v0, p0
    const/4 v1, 1
    const/4 v2, 2
    const/4 v3, 3
    const/4 v4, 4
    const/4 v5, 5
    const/4 v6, 6
    invoke-polymorphic/range {v0 .. v6}, Ljava/lang/invoke/MethodHandle;->invokeExact([Ljava/lang/Object;)Ljava/lang/Object;, (IIIIII)J
    move-result-wide v0
    return-wide v0
.end method

.method public static wideTwice(Ljava/lang/invoke/MethodHandle;)J
    .registers 3
    invoke-polymorphic {p0}, Ljava/lang/invoke/MethodHandle;->invokeExact([Ljava/lang/Object;)Ljava/lang/Object;, ()J
    move-result-wide v0
    add-long/2addr v0, v0
    return-wide v0
.end method

.method public static nullArgument(Ljava/lang/invoke/MethodHandle;)Ljava/lang/Object;
    .registers 3
    const/4 v1, 0
    invoke-polymorphic {p0, v1}, Ljava/lang/invoke/MethodHandle;->invokeExact([Ljava/lang/Object;)Ljava/lang/Object;, (Ljava/lang/String;)Ljava/lang/Object;
    move-result-object v0
    return-object v0
.end method
