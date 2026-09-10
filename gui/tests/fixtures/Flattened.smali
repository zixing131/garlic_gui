.class public Ldemo/Flattened;
.super Ljava/lang/Object;

.method public static run()I
    .registers 2
    const/4 v1, 0x0
    const/4 v0, 0x0
    goto :dispatch
  :dispatch
    packed-switch v0, :table
    const/4 v1, -0x1
    return v1
  :first
    const/4 v1, 0x7
    const/4 v0, 0x1
    goto :dispatch
  :second
    return v1
  :table
    .packed-switch 0x0
        :first
        :second
    .end packed-switch
.end method

.method public static sparse()I
    .registers 2
    const/16 v0, -123
    goto :dispatch
  :dispatch
    sparse-switch v0, :table
    const/4 v1, -1
    return v1
  :match
    const/4 v1, 5
    return v1
  :table
    .sparse-switch
        -123 -> :match
    .end sparse-switch
.end method

.method public static fallback()I
    .registers 2
    const v0, 123456
    goto :dispatch
  :dispatch
    packed-switch v0, :table
    const/4 v1, 3
    return v1
  :match
    const/4 v1, 5
    return v1
  :table
    .packed-switch 0
        :match
    .end packed-switch
.end method

.method public static unknown(I)I
    .registers 2
    goto :dispatch
  :dispatch
    packed-switch p0, :table
    const/4 v0, 3
    return v0
  :match
    const/4 v0, 5
    return v0
  :table
    .packed-switch 0
        :match
    .end packed-switch
.end method
