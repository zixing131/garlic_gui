.class public Ldemo/Flattened;
.super Ljava/lang/Object;
.method public static literal()I
    .registers 2
    const v0, 2147483647
    add-int/lit8 v1, v0, 2
    xor-int/lit8 v0, v1, 77
    return v0
.end method

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

# The shared hash block has several predecessors, including a fallthrough.
.method public static hashLoop()I
    .registers 2
    const/4 v1, 0
    const-string v0, "start"
  :hash
    invoke-static {v0}, Ldemo/Flattened;->hash(Ljava/lang/Object;)I
    move-result v0
  :dispatch
    sparse-switch v0, :table
    const/4 v1, -1
    return v1
  :first
    const/16 v1, 42
    const-string v0, "end"
    goto :hash
  :done
    return v1
  :table
    .sparse-switch
        100571 -> :done
        109757538 -> :first
    .end sparse-switch
.end method

.method public static hash(Ljava/lang/Object;)I
    .registers 1
    invoke-virtual {p0}, Ljava/lang/Object;->hashCode()I
    move-result p0
    return p0
.end method

# The result is copied after an unrelated register assignment.
.method public static movedState()I
    .registers 3
    const-string v0, "end"
    invoke-static {v0}, Ldemo/Flattened;->hash(Ljava/lang/Object;)I
    move-result v0
    const/16 v1, 23
    move v2, v0
    goto :dispatch
  :dispatch
    sparse-switch v2, :table
    const/4 v1, -1
    return v1
  :done
    return v1
  :table
    .sparse-switch
        100571 -> :done
    .end sparse-switch
.end method

# Java signed const/4 and arithmetic overflow must agree with Dalvik.
.method public static negativeState()I
    .registers 2
    const/4 v0, -1
    xor-int/lit8 v0, v0, 7
    goto :dispatch
  :dispatch
    sparse-switch v0, :table
    const/4 v1, -1
    return v1
  :done
    const/16 v1, 31
    return v1
  :table
    .sparse-switch
        -8 -> :done
    .end sparse-switch
.end method

# A textual const before a shared switch does not dominate the input edge.
.method public static sharedInput(I)I
    .registers 2
    if-nez p0, :dispatch
    const/4 p0, 1
  :dispatch
    packed-switch p0, :table
    const/4 v0, 3
    return v0
  :one
    const/4 v0, 5
    return v0
  :table
    .packed-switch 1
        :one
    .end packed-switch
.end method

# Merely containing hashCode does not make this a hash wrapper.
.method public static notHash(Ljava/lang/Object;)I
    .registers 2
    invoke-virtual {p0}, Ljava/lang/Object;->hashCode()I
    move-result v0
    const/16 v0, 77
    return v0
.end method

.method public static customHash()I
    .registers 2
    const-string v0, "end"
    invoke-static {v0}, Ldemo/Flattened;->notHash(Ljava/lang/Object;)I
    move-result v0
    sparse-switch v0, :table
    const/4 v1, 3
    return v1
  :match
    const/4 v1, 5
    return v1
  :table
    .sparse-switch
        77 -> :match
    .end sparse-switch
.end method

# Distinct input paths both enter a shared hash dispatcher.
.method public static branchHash(Z)I
    .registers 3
    if-eqz p0, :other
    const-string v0, "start"
    goto :hash
  :other
    const-string v0, "end"
  :hash
    invoke-static {v0}, Ldemo/Flattened;->hash(Ljava/lang/Object;)I
    move-result v0
    sparse-switch v0, :table
    const/4 v1, -1
    return v1
  :first
    const/16 v1, 42
    return v1
  :done
    const/16 v1, 23
    return v1
  :table
    .sparse-switch
        100571 -> :done
        109757538 -> :first
    .end sparse-switch
.end method

.method public static unicodeHash()I
    .registers 2
    const-string v0, "\ud83d\ude00\u0000long-state-string"
    invoke-static {v0}, Ldemo/Flattened;->hash(Ljava/lang/Object;)I
    move-result v0
    sparse-switch v0, :table
    const/4 v1, -1
    return v1
  :match
    const/16 v1, 19
    return v1
  :table
    .sparse-switch
        2098418465 -> :match
    .end sparse-switch
.end method

# D810-style backward tracking: both paths agree even after a real input branch.
.method public static joinedState(Z)I
    .registers 3
    if-eqz p0, :other
    const/16 v0, 7
    goto :join
  :other
    const/16 v0, 7
  :join
    xor-int/lit8 v0, v0, 3
    goto :dispatch
  :dispatch
    sparse-switch v0, :table
    const/4 v1, -1
    return v1
  :done
    const/16 v1, 61
    return v1
  :table
    .sparse-switch
        4 -> :done
    .end sparse-switch
.end method

# Dispatch using comparisons rather than a switch; no calls/writes may be skipped.
.method public static compareDispatcher()I
    .registers 4
    const/4 v1, 1
    const/4 v2, 2
    const/4 v0, 1
    goto :dispatch
  :dispatch
    if-eq v0, v1, :first
    if-eq v0, v2, :second
    const/4 v3, -1
    return v3
  :first
    const/16 v3, 37
    const/4 v0, 2
    goto :dispatch
  :second
    return v3
.end method
