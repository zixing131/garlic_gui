.class public Ldemo/PrimitiveArrays;
.super Ljava/lang/Object;

.field public static table:[S

.method public static shorts()[S
    .registers 1
    const/16 v0, 4
    new-array v0, v0, [S
    fill-array-data v0, :data
    return-object v0
  :data
    .array-data 2
        -32768
        -1
        0
        32767
    .end array-data
.end method

.method public static bytes()[B
    .registers 1
    const/16 v0, 3
    new-array v0, v0, [B
    fill-array-data v0, :data
    return-object v0
  :data
    .array-data 1
        -128
        -1
        127
    .end array-data
.end method

.method public static chars()[C
    .registers 1
    const/16 v0, 3
    new-array v0, v0, [C
    fill-array-data v0, :data
    return-object v0
  :data
    .array-data 2
        0
        65
        65535
    .end array-data
.end method

.method public static ints()[I
    .registers 1
    const/16 v0, 3
    new-array v0, v0, [I
    fill-array-data v0, :data
    return-object v0
  :data
    .array-data 4
        -2147483648
        0
        2147483647
    .end array-data
.end method

.method public static longs()[J
    .registers 1
    const/16 v0, 3
    new-array v0, v0, [J
    fill-array-data v0, :data
    return-object v0
  :data
    .array-data 8
        -9223372036854775808L
        -1L
        9223372036854775807L
    .end array-data
.end method

.method public static floats()[F
    .registers 1
    const/16 v0, 2
    new-array v0, v0, [F
    fill-array-data v0, :data
    return-object v0
  :data
    .array-data 4
        1065353216
        3223322624
    .end array-data
.end method

.method public static doubles()[D
    .registers 1
    const/16 v0, 2
    new-array v0, v0, [D
    fill-array-data v0, :data
    return-object v0
  :data
    .array-data 8
        4607182418800017408L
        13836183955189006336L
    .end array-data
.end method

.method public static bools()[Z
    .registers 1
    const/16 v0, 2
    new-array v0, v0, [Z
    fill-array-data v0, :data
    return-object v0
  :data
    .array-data 1
        0
        1
    .end array-data
.end method

.method public static sized(I)[S
    .registers 2
    new-array v0, p0, [S
    return-object v0
.end method

.method public static empty()[S
    .registers 1
    const/4 v0, 0
    new-array v0, v0, [S
    return-object v0
.end method

.method public static fill([S)[S
    .registers 1
    fill-array-data p0, :data
    return-object p0
  :data
    .array-data 2
        4
        -7
    .end array-data
.end method

.method public static filled()[I
    .registers 3
    const/4 v0, 3
    const/4 v1, -2
    filled-new-array {v0, v1}, [I
    move-result-object v2
    return-object v2
.end method

.method public static filledRange()[I
    .registers 3
    const/4 v0, 3
    const/4 v1, -2
    filled-new-array/range {v0 .. v1}, [I
    move-result-object v2
    return-object v2
.end method

.method static constructor <clinit>()V
    .registers 1
    const/4 v0, 2
    new-array v0, v0, [S
    fill-array-data v0, :data
    sput-object v0, Ldemo/PrimitiveArrays;->table:[S
    return-void
  :data
    .array-data 2
        4
        -7
    .end array-data
.end method

.method public static shared()[S
    .registers 1
    sget-object v0, Ldemo/PrimitiveArrays;->table:[S
    return-object v0
.end method

.method public static floatBits()[F
    .registers 1
    const/4 v0, 4
    new-array v0, v0, [F
    fill-array-data v0, :data
    return-object v0
  :data
    .array-data 4
        0x80000000
        1
        0x7f800000
        0x7fc00123
    .end array-data
.end method

.method public static doubleBits()[D
    .registers 1
    const/4 v0, 4
    new-array v0, v0, [D
    fill-array-data v0, :data
    return-object v0
  :data
    .array-data 8
        0x8000000000000000L
        1L
        0x7ff0000000000000L
        0x7ff8000000000123L
    .end array-data
.end method
