// RingTerrainManager.cpp
#include "RingTerrainManager.h"
#include "ProceduralMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Kismet/KismetMathLibrary.h"
#include "Async/Async.h"
#include "KismetProceduralMeshLibrary.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "NavigationSystem.h"
#include "AI/NavigationSystemBase.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Character.h"
#include "Materials/MaterialInterface.h"


// Constructor
ARingTerrainManager::ARingTerrainManager()
{
    PrimaryActorTick.bCanEverTick = true;
    RootComponent = GetRootComponent();
    //Tree_Inst = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ISMC"));
}

// BeginPlay
void ARingTerrainManager::BeginPlay()
{
    Super::BeginPlay();

    // build curve table on game thread for worker safety
    if (NoiseCurveTable.Num() != NoiseCurveTableSize)
        NoiseCurveTable.SetNumZeroed(NoiseCurveTableSize);

    if (NoiseCurve && NoiseCurveTable.Num() > 0)
    {
        for (int32 i = 0; i < NoiseCurveTableSize; ++i)
        {
            float t = float(i) / float(NoiseCurveTableSize - 1);
            NoiseCurveTable[i] = NoiseCurve->GetFloatValue(t);
        }
    }
    else
    {
        for (int32 i = 0; i < NoiseCurveTableSize; ++i)
            NoiseCurveTable[i] = 1.0f;
    }

    // Derived spacing: vertices across ReferenceSize -> spacing = ReferenceSize / (ReferenceResolution-1)
    VertexSpacing = ReferenceSize / float(FMath::Max(2, ReferenceResolution) - 1);
    ChunkWorldSize = VertexSpacing * float(FMath::Max(2, ChunkResolution) - 1);

    // ensure we have some mesh components pooled
    EnsurePoolSize(MaxPoolSize);




    // initial stream
    if (UWorld* W = GetWorld())
    {
        if (APlayerController* PC = W->GetFirstPlayerController())
        {
            if (APawn* Pawn = PC->GetPawn())
            {
                UpdateStreamingWorker(Pawn->GetActorLocation());

            }
        }
    }
}

// EndPlay - safe cleanup
void ARingTerrainManager::EndPlay(const EEndPlayReason::Type Reason)
{
    // Destroy active chunk components
    for (auto& Pair : ActiveChunks)
    {
        if (UProceduralMeshComponent* M = Pair.Value)
        {
            if (IsValid(M))
            {
                M->UnregisterComponent();
                M->DestroyComponent();
            }
        }
    }
    ActiveChunks.Empty();
    ChunkLODMap.Empty();

    // Destroy pooled components safely
    for (UProceduralMeshComponent* M : MeshPool)
    {
        if (IsValid(M))
        {
            M->UnregisterComponent();
            M->DestroyComponent();
        }
    }
    MeshPool.Empty();

    Super::EndPlay(Reason);
}

void ARingTerrainManager::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    // Extend procedural rivers to cover player's view
    //ExtendRiversIfNeeded();

    // Accumulate time for throttled streaming update
    TimeSinceLastStreamingUpdate += DeltaSeconds;

    if (UWorld* W = GetWorld())
    {
        APlayerController* PC = W->GetFirstPlayerController();
        if (!PC) return;

        APawn* Pawn = PC->GetPawn();
        if (!Pawn) return;

        FVector PlayerPos = Pawn->GetActorLocation();

        // === THROTTLED STREAMING UPDATE ===
        if (TimeSinceLastStreamingUpdate >= StreamingUpdateInterval)
        {
            TimeSinceLastStreamingUpdate = 0.2f;
            UpdateStreamingWorker(PlayerPos);
        }

        // Process vegetation queues every tick (cheap and necessary)
        ProcessFoliageSpawns();
        ProcessGrassSpawn();
        ProcessBuildingSpawns();
    }
}


// RegenerateAll - destroy everything and recreate pool
void ARingTerrainManager::RegenerateAll()
{
    // destroy active meshes and clear
    for (auto& Pair : ActiveChunks)
    {
        if (Pair.Value)
        {
            if (IsValid(Pair.Value))
            {
                Pair.Value->UnregisterComponent();
                Pair.Value->DestroyComponent();
            }
        }
    }
    ActiveChunks.Empty();
    ChunkLODMap.Empty();

    // clear pool (we'll recreate as needed)
    for (UProceduralMeshComponent* M : MeshPool)
    {
        if (IsValid(M))
        {
            M->UnregisterComponent();
            M->DestroyComponent();
        }
    }
    MeshPool.Empty();

    EnsurePoolSize(MaxPoolSize);

    // regenerate roads if desired (keeps same seed)

}

/* ---------- Pool helpers ---------- */
void ARingTerrainManager::EnsurePoolSize(int32 Desired)
{
    // ensure actor has a root scene component to attach to
    USceneComponent* Parent = GetRootComponent();
    if (!Parent)
    {
        Parent = NewObject<USceneComponent>(this, TEXT("RingTerrainRoot"));
        Parent->RegisterComponent();
        SetRootComponent(Parent);
    }

    UWorld* W = GetWorld();
    if (!W) return;

    while (MeshPool.Num() < Desired)
    {
        UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(this);
        PMC->RegisterComponent();
        PMC->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform);
        PMC->SetMobility(EComponentMobility::Static);
        PMC->bUseAsyncCooking = true;
        PMC->SetVisibility(false);
        MeshPool.Add(PMC);
    }
}

UProceduralMeshComponent* ARingTerrainManager::AcquireMeshComponent()
{
    // Safe pop: get Last() then Pop()
    if (MeshPool.Num() > 0)
    {
        UProceduralMeshComponent* M = MeshPool.Last();
        MeshPool.Pop();
        if (IsValid(M))
        {
            M->SetVisibility(true);
            return M;
        }
    }

    // none available -> create new (bounded by MaxPoolSize*2)
    UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(this);
    PMC->RegisterComponent();
    USceneComponent* Parent = GetRootComponent();
    if (!Parent)
    {
        Parent = NewObject<USceneComponent>(this, TEXT("RingTerrainRoot"));
        Parent->RegisterComponent();
        SetRootComponent(Parent);
    }
    PMC->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform);
    PMC->SetMobility(EComponentMobility::Static);
    PMC->bUseAsyncCooking = true;
    return PMC;
}

void ARingTerrainManager::ReleaseMeshComponent(UProceduralMeshComponent* Mesh)
{
    if (!Mesh) return;
    Mesh->ClearAllMeshSections();
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Mesh->SetVisibility(false);

    if (MeshPool.Num() < MaxPoolSize)
    {
        MeshPool.Add(Mesh);
    }
    else
    {
        if (IsValid(Mesh))
        {
            Mesh->UnregisterComponent();
            Mesh->DestroyComponent();
        }
    }
}

/* ---------- Streaming ---------- */
FIntPoint ARingTerrainManager::CameraChunkCoordFromLocation(const FVector& Loc) const
{
    int32 CX = FMath::FloorToInt(Loc.X / ChunkWorldSize);
    int32 CY = FMath::FloorToInt(Loc.Y / ChunkWorldSize);
    return FIntPoint(CX, CY);
}

void ARingTerrainManager::UpdateStreaming(const FVector& PlayerLocation)
{
    FIntPoint Center = CameraChunkCoordFromLocation(PlayerLocation);

    TSet<FIntPoint> Desired;
    for (int32 dy = -ViewRadiusInChunks; dy <= ViewRadiusInChunks; ++dy)
    {
        for (int32 dx = -ViewRadiusInChunks; dx <= ViewRadiusInChunks; ++dx)
        {
            FIntPoint Coord = Center + FIntPoint(dx, dy);
            Desired.Add(Coord);

            int32 NewLOD = PickLODForChunk(Coord, PlayerLocation);

            if (!ActiveChunks.Contains(Coord))
            {
                // Fresh request
                ChunkLODMap.Add(Coord, NewLOD);
                RequestChunkGeneration(Coord, NewLOD, ChunkLODMap);
            }
            else
            {
                int32 CurrentLOD = ChunkLODMap[Coord];

                // --- Hysteresis buffer ---
                // Only trigger a rebuild if the new LOD differs significantly
                const int32 LODDelta = FMath::Abs(CurrentLOD - NewLOD);
                if (LODDelta >= 1)
                {
                    // Release old mesh
                    if (UProceduralMeshComponent* OldMesh = ActiveChunks[Coord])
                    {
                        ReleaseMeshComponent(OldMesh);
                    }
                    ActiveChunks.Remove(Coord);

                    // Request new mesh at updated LOD
                    ChunkLODMap[Coord] = NewLOD;
                    RequestChunkGeneration(Coord, NewLOD, ChunkLODMap);
                }
                else
                {
                    // No change, keep the same
                    ChunkLODMap[Coord] = CurrentLOD;
                }
            }
        }
    }

    // Remove chunks no longer needed
    TArray<FIntPoint> ToRemove;
    for (auto& Pair : ActiveChunks)
    {
        if (!Desired.Contains(Pair.Key))
            ToRemove.Add(Pair.Key);
    }
    for (FIntPoint K : ToRemove)
    {
        if (UProceduralMeshComponent* M = ActiveChunks[K])
        {
            ReleaseMeshComponent(M);
        }
        ActiveChunks.Remove(K);
        ChunkLODMap.Remove(K);
    }
}

void ARingTerrainManager::UpdateStreamingWorker(const FVector& PlayerLocation)
{
    // Prevent overlapping async tasks
    if (bStreamingWorkerBusy)
        return;

    bStreamingWorkerBusy = true;

    FVector LocationCopy = PlayerLocation;

    Async(EAsyncExecution::ThreadPool, [this, LocationCopy]()
        {
            // Local containers (thread-safe)
            TSet<FIntPoint> Desired;
            TMap<FIntPoint, int32> DesiredLODMap;

            // Heavy math: compute visible chunks + LODs
            FIntPoint Center = CameraChunkCoordFromLocation(LocationCopy);

            for (int32 dy = -ViewRadiusInChunks; dy <= ViewRadiusInChunks; ++dy)
            {
                for (int32 dx = -ViewRadiusInChunks; dx <= ViewRadiusInChunks; ++dx)
                {
                    FIntPoint Coord = Center + FIntPoint(dx, dy);
                    int32 NewLOD = PickLODForChunk(Coord, LocationCopy);
                    Desired.Add(Coord);
                    DesiredLODMap.Add(Coord, NewLOD);
                }
            }

            // Push results back to game thread safely
            AsyncTask(ENamedThreads::GameThread, [this, Desired = MoveTemp(Desired), DesiredLODMap = MoveTemp(DesiredLODMap)]()
                {
                    ApplyStreamingChanges(Desired, DesiredLODMap);
                    bStreamingWorkerBusy = false;  // release lock
                });
        });
}

void ARingTerrainManager::ApplyStreamingChanges(const TSet<FIntPoint>& Desired, const TMap<FIntPoint, int32>& DesiredLODMap)
{
    // Create new chunks or update existing ones
    for (const auto& Pair : DesiredLODMap)
    {
        FIntPoint Coord = Pair.Key;
        int32 NewLOD = Pair.Value;

        if (!ActiveChunks.Contains(Coord))
        {
            ChunkLODMap.Add(Coord, NewLOD);
            RequestChunkGeneration(Coord, NewLOD, ChunkLODMap);
        }
        else
        {
            int32 CurrentLOD = ChunkLODMap[Coord];
            const int32 LODDelta = FMath::Abs(CurrentLOD - NewLOD);

            if (LODDelta >= 1)
            {
                if (UProceduralMeshComponent* OldMesh = ActiveChunks[Coord])
                {
                    ReleaseMeshComponent(OldMesh);
                }
                ActiveChunks.Remove(Coord);
                ChunkLODMap[Coord] = NewLOD;
                RequestChunkGeneration(Coord, NewLOD, ChunkLODMap);
            }
        }
    }

    // Remove chunks no longer needed
    TArray<FIntPoint> ToRemove;
    for (auto& Pair : ActiveChunks)
    {
        if (!Desired.Contains(Pair.Key))
            ToRemove.Add(Pair.Key);
    }

    for (FIntPoint K : ToRemove)
    {
        if (UProceduralMeshComponent* M = ActiveChunks[K])
        {
            ReleaseMeshComponent(M);
        }
        ActiveChunks.Remove(K);
        ChunkLODMap.Remove(K);
    }
}



/* ---------- Async generation ---------- */
void ARingTerrainManager::RequestChunkGeneration(const FIntPoint& ChunkCoord, int32 LOD, TMap<FIntPoint, int32> LODSnapshot)
{
    // Capture a snapshot of the curve table for thread safety
    TArray<float> CurveCopy = NoiseCurveTable; // cheap copy (float array); safe to capture by value
    TArray<TArray<float>> SplatTablesCopy = SplatCurveTables; // copy splat tables for thread safety

    // Launch worker
    /*
    OLD
    Async(EAsyncExecution::ThreadPool, [this, ChunkCoord, LOD, LODSnapshot, CurveCopy, SplatTablesCopy]() mutable
        {
            // worker thread: generate HF using only POD and snapshots
            TSharedPtr<FRingHeightField, ESPMode::ThreadSafe> HF = GenerateHeightfieldWorker(ChunkCoord, LOD, LODSnapshot, CurveCopy, SplatTablesCopy);

            // marshal back to game thread
            AsyncTask(ENamedThreads::GameThread, [this, ChunkCoord, LOD, HF]()
                {
                    if (!HF.IsValid()) return;
                    UProceduralMeshComponent* Mesh = AcquireMeshComponent();
                    if (!Mesh) return;
                    BuildMeshFromHeightfield(Mesh, ChunkCoord, LOD, HF);
                    ActiveChunks.Add(ChunkCoord, Mesh);
                    ChunkLODMap.Add(ChunkCoord, LOD);
                });
        });
        */
    Async(EAsyncExecution::ThreadPool, [this, ChunkCoord, LOD, LODSnapshot, CurveCopy, SplatTablesCopy]()
        {
            // Step 1: Generate heightfield (already async)
            auto HF = GenerateHeightfieldWorker(ChunkCoord, LOD, LODSnapshot, CurveCopy, SplatTablesCopy);

            if (!HF.IsValid()) return;

            // Step 2: Build raw mesh data (still off-thread)
            FRingMeshBuildData MeshData;
            MeshData.ChunkCoord = ChunkCoord;
            MeshData.LOD = LOD;
            MeshData.HeightField = HF;

            BuildMeshDataWorker(MeshData); // <- new off-thread math function

            // Step 3: Push result back to GameThread for mesh creation
            AsyncTask(ENamedThreads::GameThread, [this, Data = MoveTemp(MeshData)]()
                {
                    UProceduralMeshComponent* Mesh = AcquireMeshComponent();
                    if (!Mesh) return;

                    ApplyMeshDataToComponent(Mesh, Data); // replaces BuildMeshFromHeightfield()
                    ActiveChunks.Add(Data.ChunkCoord, Mesh);
                    ChunkLODMap.Add(Data.ChunkCoord, Data.LOD);
                });
        });


}
void ARingTerrainManager::BuildMeshDataWorker(FRingMeshBuildData& OutData)
{
    const auto HF = OutData.HeightField;
    if (!HF.IsValid()) return;

    // Build road distance field for this chunk
    int DFRes = FMath::Clamp(RoadDistanceFieldResolution, 8, 128);
    BuildRoadDistanceField(OutData.ChunkCoord, DFRes, OutData.RoadDistanceField);

    int32 SX = HF->SizeX;
    int32 SY = HF->SizeY;

    const double originX = double(OutData.ChunkCoord.X) * double(ChunkWorldSize);
    const double originY = double(OutData.ChunkCoord.Y) * double(ChunkWorldSize);
    const double step = double(ChunkWorldSize) / double(SX - 1);

    OutData.Vertices.SetNum(SX * SY);
    OutData.UVs.SetNum(SX * SY);
    OutData.VertexColors.SetNum(SX * SY);
    OutData.Normals.SetNumZeroed(SX * SY);

    // ----------------------------
    // First pass: vertices & UVs
    // ----------------------------
    for (int y = 0; y < SY; ++y)
    {
        for (int x = 0; x < SX; ++x)
        {
            int idx = y * SX + x;

            double WX = originX + double(x) * step;
            double WY = originY + double(y) * step;
            float H = HF->At(x, y);

            // Road carving
            for (const FRoadSpline& Road : RoadSplines)
            {
                float d, rz;
                if (ComputeDistanceToSpline(WX, WY, Road, d, rz))
                {
                    float effectiveWidth = Road.Width * RoadWidthMultiplier;
                    if (d < effectiveWidth)
                    {
                        float t = d / FMath::Max(1.f, effectiveWidth);
                        float smooth = powf(t, 1.f / FMath::Max(0.0001f, RoadBlendStrength));
                        H = FMath::Lerp(rz, H, smooth);
                    }
                    // -------- BUILDING TERRAIN FLATTENING --------
                    for (const FBuildingSpawn& B : Buildings)
                    {
                        if (B.Location != FVector::ZeroVector) {

                            const FVector P((float)WX, (float)WY, 0.0f);
                            const FVector Bpos = B.Location;

                            float Dist = FVector::Dist2D(P, Bpos);

                            const float Radius = 2000.0f;
                            const float ExtraBlend = 500.0f;

                            if (Dist < Radius + ExtraBlend)
                            {
                                float TargetHeight = B.Location.Z + B.FlattenHeightOffset;

                                if (Dist <= Radius)
                                {
                                    H = TargetHeight;
                                }
                                else
                                {
                                    float t = (Dist - Radius) / ExtraBlend;
                                    float smooth = 1.f - FMath::SmoothStep(0.f, 1.f, t);
                                    H = FMath::Lerp(H, TargetHeight, smooth);
                                }
                            }

                        }
                    }

                }
            }

            OutData.Vertices[idx] = FVector((float)WX, (float)WY, H);
            OutData.UVs[idx] = FVector2D(float(x) / float(SX - 1), float(y) / float(SY - 1));
            OutData.VertexColors[idx] = FLinearColor::White;
        }
    }

    // ---------------------------------------------------------
    // Build triangles
    // ---------------------------------------------------------
    for (int y = 0; y < SY - 1; ++y)
    {
        for (int x = 0; x < SX - 1; ++x)
        {
            int i0 = y * SX + x;
            int i1 = i0 + 1;
            int i2 = i0 + SX;
            int i3 = i2 + 1;

            OutData.TerrainTriangles.Add(i0); OutData.TerrainTriangles.Add(i2); OutData.TerrainTriangles.Add(i1);
            OutData.TerrainTriangles.Add(i1); OutData.TerrainTriangles.Add(i2); OutData.TerrainTriangles.Add(i3);
        }
    }

    // ---------------------------------------------------------
    // Compute normals/tangents
    // ---------------------------------------------------------
    UKismetProceduralMeshLibrary::CalculateTangentsForMesh(
        OutData.Vertices,
        OutData.TerrainTriangles,
        OutData.UVs,
        OutData.Normals,
        OutData.Tangents);

    // ---------------------------------------------------------
    // BUILDING + TREE + GRASS SPAWNING (ASYNC SAFE)
    // ---------------------------------------------------------
    bool bShouldSpawnTrees = !ChunksWithTrees.Contains(OutData.ChunkCoord);

    // Building exclusion helper
    auto IsNearAnyBuilding = [&](const FVector& WP)->bool
        {
            for (const FBuildingSpawn& B : Buildings)
            {
                // Use half-extents + margin to block vegetation
                if (IsInsideBuildingFootprint(WP, B, BuildingNoSpawnRadius))
                    return true;
            }
            return false;
        };

    // Per-vertex spawning
    for (int32 idx = 0; idx < OutData.Vertices.Num(); ++idx)
    {
        const FVector& WorldPos = OutData.Vertices[idx];
        float NormalZ = OutData.Normals[idx].Z;

        // Check near road
        bool bInsideAnyRoad = false;
        for (const FRoadSpline& Road : RoadSplines)
        {
            float d, rz;
            if (ComputeDistanceToSpline(WorldPos.X, WorldPos.Y, Road, d, rz))
            {
                if (d < Road.Width * RoadWidthMultiplier)
                {
                    bInsideAnyRoad = true;
                    break;
                }
            }
        }

        // -------------------------------------------
        // BUILDING SPAWN (vertex-based)
        // -------------------------------------------
        if (!bInsideAnyRoad &&
            !IsNearAnyBuilding(WorldPos) &&
            Buildings.Num() > 0 &&
            FMath::FRand() < BuildingDensity)
        {
            float H = WorldPos.Z;
            if (H > BuildingAltitudeMin && H < BuildingAltitudeMax)
            {
                if (NormalZ > BuildingMinSlope)
                {
                    int32 Bi = FMath::RandRange(0, Buildings.Num() - 1);
                    const FBuildingSpawn& B = Buildings[Bi];

                    FBuildingSpawnData Spawn;
                    Spawn.Location = WorldPos;
                    Spawn.BuildingClass = B.BuildingClass;

                    BuildingSpawnQueue.Enqueue(Spawn);
                }
            }
        }

        // -------------------------------------------
        // TREE SPAWN
        // -------------------------------------------
        if (bShouldSpawnTrees &&
            !bInsideAnyRoad &&
            !IsNearAnyBuilding(WorldPos) &&
            FoliageActors.Num() > 0 &&
            FMath::FRand() < TreeDensity)
        {
            float H = WorldPos.Z;
            if (H > TreeAltitudeMin && H < TreeAltitudeMax)
            {
                if (NormalZ > TreeSlope)
                {
                    int Index = FMath::RandRange(0, FoliageActors.Num() - 1);
                    FFoliageSpawnData Spawn;
                    Spawn.Location = WorldPos;
                    Spawn.FoliageClass = FoliageActors[Index];
                    FoliageSpawnQueue.Enqueue(Spawn);
                }
            }
        }

        // -------------------------------------------
        // GRASS SPAWN
        // -------------------------------------------
        if (!bInsideAnyRoad &&
            !IsNearAnyBuilding(WorldPos) &&
            GrassActors.Num() > 0 &&
            FMath::FRand() < GrassDensity)
        {
            float H = WorldPos.Z;
            if (H > GrassAltitudeMin && H < GrassAltitudeMax)
            {
                if (NormalZ > GrassSlope)
                {
                    int Index = FMath::RandRange(0, GrassActors.Num() - 1);
                    FGrassSpawnData Spawn;
                    Spawn.Location = WorldPos;
                    Spawn.GrassClass = GrassActors[Index];
                    GrassSpawnQueue.Enqueue(Spawn);
                }
            }
        }
    }

    // Mark chunk as having completed tree spawn
    if (bShouldSpawnTrees)
    {
        FScopeLock Lock(&StreamingLock);
        ChunksWithTrees.Add(OutData.ChunkCoord);
    }
}


void ARingTerrainManager::ApplyMeshDataToComponent(UProceduralMeshComponent* Mesh, const FRingMeshBuildData& Data)
{
    Mesh->ClearAllMeshSections();

    Mesh->CreateMeshSection_LinearColor(
        0, Data.Vertices, Data.TerrainTriangles,
        Data.Normals, Data.UVs, Data.VertexColors, Data.Tangents, true);

    if (ChunkMaterial) Mesh->SetMaterial(0, ChunkMaterial);
    Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
}



/* ---------- Road distance-field helpers (worker-safe math) ---------- */
void ARingTerrainManager::BuildRoadDistanceField(const FIntPoint& ChunkCoord, int32 DFResolution, TArray<FRoadDFCell>& OutDF)
{
    if (DFResolution < 2) DFResolution = 2;
    OutDF.SetNum(DFResolution * DFResolution);

    const double originX = double(ChunkCoord.X) * double(ChunkWorldSize);
    const double originY = double(ChunkCoord.Y) * double(ChunkWorldSize);
    const double step = double(ChunkWorldSize) / double(DFResolution - 1);

    for (int y = 0; y < DFResolution; ++y)
    {
        for (int x = 0; x < DFResolution; ++x)
        {
            double WX = originX + double(x) * step;
            double WY = originY + double(y) * step;

            float bestDist = FLT_MAX;
            float bestZ = 0.f;

            // Evaluate all splines here (only DFResolution*DFResolution tests)
            for (const FRoadSpline& Road : RoadSplines)
            {
                float d, rz;
                if (ComputeDistanceToSpline(WX, WY, Road, d, rz))
                {
                    if (d < bestDist)
                    {
                        bestDist = d;
                        bestZ = rz;
                    }
                }
            }

            FRoadDFCell& cell = OutDF[y * DFResolution + x];
            cell.Dist = bestDist;
            cell.Height = bestZ;
        }
    }
}

bool ARingTerrainManager::SampleRoadDF(const TArray<FRoadDFCell>& DF, int32 DFRes, double WX, double WY, const FIntPoint& ChunkCoord, float& OutDist, float& OutZ) const
{
    if (DFRes < 2 || DF.Num() < DFRes * DFRes) return false;

    double ChunkMinX = double(ChunkCoord.X) * double(ChunkWorldSize);
    double ChunkMinY = double(ChunkCoord.Y) * double(ChunkWorldSize);

    double fx = (WX - ChunkMinX) / double(ChunkWorldSize);
    double fy = (WY - ChunkMinY) / double(ChunkWorldSize);

    fx = FMath::Clamp(fx, 0.0, 1.0);
    fy = FMath::Clamp(fy, 0.0, 1.0);

    double X = fx * double(DFRes - 1);
    double Y = fy * double(DFRes - 1);

    int x0 = int(FMath::FloorToInt((float)X));
    int y0 = int(FMath::FloorToInt((float)Y));
    int x1 = FMath::Min(x0 + 1, DFRes - 1);
    int y1 = FMath::Min(y0 + 1, DFRes - 1);

    float tx = float(X - x0);
    float ty = float(Y - y0);

    auto Idx = [&](int xi, int yi)->int { return yi * DFRes + xi; };

    const FRoadDFCell& c00 = DF[Idx(x0, y0)];
    const FRoadDFCell& c10 = DF[Idx(x1, y0)];
    const FRoadDFCell& c01 = DF[Idx(x0, y1)];
    const FRoadDFCell& c11 = DF[Idx(x1, y1)];

    float d0 = FMath::Lerp(c00.Dist, c10.Dist, tx);
    float d1 = FMath::Lerp(c01.Dist, c11.Dist, tx);
    OutDist = FMath::Lerp(d0, d1, ty);

    float z0 = FMath::Lerp(c00.Height, c10.Height, tx);
    float z1 = FMath::Lerp(c01.Height, c11.Height, tx);
    OutZ = FMath::Lerp(z0, z1, ty);

    return true;
}



float ARingTerrainManager::TerrainNormalizedAt(double WX, double WY) const
{
    return SampleNoiseTile(WX, WY);
}

float ARingTerrainManager::SampleNoiseTile(double WX, double WY) const
{
    int R = NoiseTileResolution;
    if (R <= 1 || NoiseTile.Num() < R * R) return 0.0f;

    double scale = 1.0 / 50000.0;
    double u = fmod(WX * scale, 1.0); if (u < 0) u += 1.0;
    double v = fmod(WY * scale, 1.0); if (v < 0) v += 1.0;

    double X = u * (R - 1);
    double Y = v * (R - 1);

    int x0 = int(X), y0 = int(Y);
    int x1 = (x0 + 1) % R;
    int y1 = (y0 + 1) % R;

    float tx = float(X - x0);
    float ty = float(Y - y0);

    auto At = [&](int X, int Y) { return NoiseTile[Y * R + X]; };

    float nx0 = FMath::Lerp(At(x0, y0), At(x1, y0), tx);
    float nx1 = FMath::Lerp(At(x0, y1), At(x1, y1), tx);
    return FMath::Lerp(nx0, nx1, ty);
}



// ---------- River & terrain sampling helpers (NEW) ----------

float ARingTerrainManager::SampleTerrainHeight(const FVector& Pos)
{
    float n = TerrainNormalizedAt_FullFBM(Pos.X, Pos.Y);
    return ApplyCurveAndScale(n);
}

FVector2D ARingTerrainManager::SampleTerrainGradient(const FVector& Pos)
{
    const double h = 20.0;
    float Hx1 = SampleTerrainHeight(Pos + FVector(h, 0, 0));
    float Hx2 = SampleTerrainHeight(Pos + FVector(-h, 0, 0));
    float Hy1 = SampleTerrainHeight(Pos + FVector(0, h, 0));
    float Hy2 = SampleTerrainHeight(Pos + FVector(0, -h, 0));

    return FVector2D((Hx1 - Hx2) / (2.0 * h), (Hy1 - Hy2) / (2.0 * h));
}

bool ARingTerrainManager::ComputeDistanceToRiver(double WX, double WY, const FRiverSpline& River, float& OutDist) const
{
    OutDist = MAX_flt;
    if (River.ControlPoints.Num() < 2) return false;

    FVector2D P((float)WX, (float)WY);

    for (int i = 0; i < River.ControlPoints.Num() - 1; ++i)
    {
        const FVector& A = River.ControlPoints[i];
        const FVector& B = River.ControlPoints[i + 1];

        FVector2D A2(A.X, A.Y);
        FVector2D B2(B.X, B.Y);

        FVector2D AB = B2 - A2;
        float denom = FVector2D::DotProduct(AB, AB);
        if (denom <= KINDA_SMALL_NUMBER) continue;

        float t = FVector2D::DotProduct(P - A2, AB) / denom;
        t = FMath::Clamp(t, 0.f, 1.f);

        FVector2D Closest = A2 + t * AB;
        float Dist = FVector2D::Distance(P, Closest);

        if (Dist < OutDist)
        {
            OutDist = Dist;
        }
    }

    return (OutDist < MAX_flt);
}

FVector ARingTerrainManager::GetRiverDirection(const FVector& Pos)
{
    // Noise-driven meander
    double sx = Pos.X * 0.00002;
    double sy = Pos.Y * 0.00002;
    float noiseAngle = FMath::PerlinNoise2D(FVector2D(sx, sy)) * PI * 2.0f;

    FVector NoiseDir = FVector(FMath::Cos(noiseAngle), FMath::Sin(noiseAngle), 0).GetSafeNormal();

    // Terrain slope-based guidance
    FVector2D Grad = SampleTerrainGradient(Pos);
    FVector Downhill = FVector(-Grad.X, -Grad.Y, 0.f);
    if (!Downhill.IsNearlyZero()) Downhill = Downhill.GetSafeNormal();
    else Downhill = NoiseDir;

    // Weight based on normalized terrain height (0 lowlands, 1 mountains)
    float n = TerrainNormalizedAt_FullFBM(Pos.X, Pos.Y);
    float weight = FMath::Lerp(0.2f, 0.95f, n);

    FVector Dir = FMath::Lerp(NoiseDir, Downhill, weight);
    if (Dir.IsNearlyZero()) return FVector(1, 0, 0);
    return Dir.GetSafeNormal();
}

// Extend river splines procedurally so they become effectively infinite
void ARingTerrainManager::ExtendRiversIfNeeded()
{
    if (RiverSplines.Num() == 0) return;
    if (!GetWorld()) return;
    APlayerController* PC = GetWorld()->GetFirstPlayerController();
    if (!PC) return;
    APawn* Pawn = PC->GetPawn();
    if (!Pawn) return;

    FVector PlayerPos = Pawn->GetActorLocation();
    float NeededRadius = ViewRadiusInChunks * ChunkWorldSize * 1.5f;

    for (FRiverSpline& River : RiverSplines)
    {
        if (!River.bCanExtend) continue;
        if (River.ControlPoints.Num() < 2) continue;

        FVector Tail = River.ControlPoints.Last();

        // extend forward until tail is outside needed radius
        int safety = 0;
        while (FVector::Dist2D(Tail, PlayerPos) < NeededRadius && safety++ < 512)
        {
            FVector Dir = GetRiverDirection(Tail);
            float Step = RoadSegmentLength * 0.7f; // rivers use slightly shorter segments
            FVector NewPoint = Tail + Dir * Step;
            NewPoint.Z = GlobalRiverWaterLevel;
            River.ControlPoints.Add(NewPoint);
            Tail = NewPoint;
        }

        // trim old points behind player to keep memory bounded
        while (River.ControlPoints.Num() > 100 && FVector::Dist2D(River.ControlPoints[0], PlayerPos) > NeededRadius * 2.0f)
        {
            River.ControlPoints.RemoveAt(0);
        }
    }
}

TSharedPtr<FRingHeightField, ESPMode::ThreadSafe> ARingTerrainManager::GenerateHeightfieldWorker(const FIntPoint& ChunkCoord, int32 LOD, const TMap<FIntPoint, int32>& LODSnapshot, const TArray<float>& CurveCopy, const TArray<TArray<float>>& SplatTablesCopy)
{
    // Select resolution for this LOD index (LODResolutions array stores per-LOD vertex counts)
    int32 Res = (LODResolutions.IsValidIndex(LOD) ? LODResolutions[LOD] : ChunkResolution);
    Res = FMath::Max(2, Res);

    TSharedPtr<FRingHeightField, ESPMode::ThreadSafe> HF = MakeShared<FRingHeightField, ESPMode::ThreadSafe>();
    HF->Resize(Res, Res);

    double Step = double(ChunkWorldSize) / double(Res - 1);
    double originX = double(ChunkCoord.X) * double(ChunkWorldSize);
    double originY = double(ChunkCoord.Y) * double(ChunkWorldSize);

    for (int y = 0; y < Res; ++y)
    {
        for (int x = 0; x < Res; ++x)
        {
            double WX = originX + double(x) * Step;
            double WY = originY + double(y) * Step;
            double normalized = TerrainNormalizedAt_FullFBM(WX, WY); // [0,1]
            FColor splat = GenerateProceduralSplat(WX, WY, normalized);
            float r = splat.R / 255.f;
            float g = splat.G / 255.f;
            float b = splat.B / 255.f;
            float a = splat.A / 255.f;

            // If SplatTablesCopy present, apply weighted curve mix. Otherwise fall back to single curve behavior.
            float h = 0.f;
            if (SplatTablesCopy.Num() > 0)
            {
                // Build a weighted blended curve value using the provided table copies
                float n = float(FMath::Clamp(normalized, 0.0, 1.0));
                // accumulate weighted table lookup
                float accum = 0.f;
                auto AccLookup = [&](int tableIndex, float weight)
                    {
                        if (weight <= 0.f) return;
                        if (SplatTablesCopy.IsValidIndex(tableIndex))
                        {
                            const TArray<float>& Tbl = SplatTablesCopy[tableIndex];
                            if (Tbl.Num() > 0)
                            {
                                int idx = FMath::Clamp(int(n * float(Tbl.Num() - 1)), 0, Tbl.Num() - 1);
                                accum += Tbl[idx] * weight;
                            }
                        }
                    };

                AccLookup(0, r);
                AccLookup(1, g);
                AccLookup(2, b);
                AccLookup(3, a);

                // normalize by sum of weights
                float sumw = r + g + b + a;
                if (sumw > KINDA_SMALL_NUMBER)
                    accum /= sumw;
                else
                    accum = 1.0f;

                float nn = n * accum;
                h = (nn - 0.5f) * 2.0f * Params.HeightScale;
            }
            else
            {
                h = ApplyCurveAndScale(normalized);
            }

            HF->At(x, y) = h;
        }
    }

    // Stitch edges using the LOD snapshot
    StitchHeightfieldEdges(HF, ChunkCoord, LOD, LODSnapshot);

    return HF;
}

/* ---------- Stitching (worker) ---------- */
void ARingTerrainManager::StitchHeightfieldEdges(TSharedPtr<FRingHeightField, ESPMode::ThreadSafe> HF, const FIntPoint& ChunkCoord, int32 LOD, const TMap<FIntPoint, int32>& LODSnapshot)
{
    if (!HF.IsValid()) return;
    int32 Res = HF->SizeX;
    if (Res <= 1) return;

    const FIntPoint Offs[4] = { FIntPoint(1,0), FIntPoint(-1,0), FIntPoint(0,1), FIntPoint(0,-1) };

    for (int ni = 0; ni < 4; ++ni)
    {
        FIntPoint NCoord = ChunkCoord + Offs[ni];
        if (!LODSnapshot.Contains(NCoord)) continue;
        int32 NeighborLOD = LODSnapshot[NCoord];
        int32 NeighborRes = LODResolutions.IsValidIndex(NeighborLOD) ? LODResolutions[NeighborLOD] : Res;
        if (NeighborRes >= Res) continue; // only stitch when neighbor is lower-res

        int step = (Res - 1) / (NeighborRes - 1);
        if (step <= 1) continue;

        for (int i = 0; i < NeighborRes; ++i)
        {
            double nx, ny;
            if (Offs[ni] == FIntPoint(-1, 0)) {
                nx = double(NCoord.X) * double(ChunkWorldSize) + double(NeighborRes - 1) * (double(ChunkWorldSize) / double(NeighborRes - 1));
                ny = double(NCoord.Y) * double(ChunkWorldSize) + double(i) * (double(ChunkWorldSize) / double(NeighborRes - 1));
            }
            else if (Offs[ni] == FIntPoint(1, 0)) {
                nx = double(NCoord.X) * double(ChunkWorldSize) + 0.0;
                ny = double(NCoord.Y) * double(ChunkWorldSize) + double(i) * (double(ChunkWorldSize) / double(NeighborRes - 1));
            }
            else if (Offs[ni] == FIntPoint(0, -1)) {
                nx = double(NCoord.X) * double(ChunkWorldSize) + double(i) * (double(ChunkWorldSize) / double(NeighborRes - 1));
                ny = double(NCoord.Y) * double(ChunkWorldSize) + double(NeighborRes - 1) * (double(ChunkWorldSize) / double(NeighborRes - 1));
            }
            else {
                nx = double(NCoord.X) * double(ChunkWorldSize) + double(i) * (double(ChunkWorldSize) / double(NeighborRes - 1));
                ny = double(NCoord.Y) * double(ChunkWorldSize) + 0.0;
            }

            double neighborNormalized = TerrainNormalizedAt_FullFBM(nx, ny);
            float neighborH = ApplyCurveAndScale(neighborNormalized);

            if (Offs[ni] == FIntPoint(-1, 0)) {
                int hy = i * step;
                int hx = 0;
                for (int k = 0; k <= step; ++k) {
                    int yy = hy + k;
                    if (yy >= Res) break;
                    float hiResVal = HF->At(hx, yy);
                    float neighborVal = neighborH;
                    HF->At(hx, yy) = FMath::Lerp(hiResVal, neighborVal, 0.5f);
                }
            }
            else if (Offs[ni] == FIntPoint(1, 0)) {
                int hy = i * step;
                int hx = Res - 1;
                for (int k = 0; k <= step; ++k) {
                    int yy = hy + k;
                    if (yy >= Res) break;
                    HF->At(hx, yy) = neighborH;
                }
            }
            else if (Offs[ni] == FIntPoint(0, -1)) {
                int hx = i * step;
                int hy = 0;
                for (int k = 0; k <= step; ++k) {
                    int xx = hx + k;
                    if (xx >= Res) break;
                    HF->At(xx, hy) = neighborH;
                }
            }
            else {
                int hx = i * step;
                int hy = Res - 1;
                for (int k = 0; k <= step; ++k) {
                    int xx = hx + k;
                    if (xx >= Res) break;
                    HF->At(xx, hy) = neighborH;
                }
            }
        }
    }
}

/* ---------- Mesh build (game thread) ----------
   Road carving is performed here on game thread (safe access to RoadSplines).
*/
void ARingTerrainManager::BuildMeshFromHeightfield(
    UProceduralMeshComponent* Mesh,
    const FIntPoint& ChunkCoord,
    int32 LOD,
    TSharedPtr<FRingHeightField, ESPMode::ThreadSafe> HF)
{
    if (!Mesh || !HF.IsValid()) return;

    int32 SX = HF->SizeX;
    int32 SY = HF->SizeY;
    if (SX <= 1 || SY <= 1) return;

    TArray<FVector> Vertices; Vertices.SetNum(SX * SY);
    TArray<FVector2D> UVs; UVs.SetNum(SX * SY);
    TArray<FVector> Normals; Normals.SetNumZeroed(SX * SY);
    TArray<FLinearColor> VertexColors; VertexColors.SetNumZeroed(SX * SY);

    // We'll produce two triangle sets: terrain (mat 0) and roads (mat 1)
    TArray<int32> TerrainTriangles; TerrainTriangles.Reserve((SX - 1) * (SY - 1) * 6);
    TArray<int32> RoadTriangles; RoadTriangles.Reserve((SX - 1) * (SY - 1) * 6);
    TArray<int32> AllTriangles; AllTriangles.Reserve((SX - 1) * (SY - 1) * 6); // used by tangent calc

    const double originX = double(ChunkCoord.X) * double(ChunkWorldSize);
    const double originY = double(ChunkCoord.Y) * double(ChunkWorldSize);
    const double step = double(ChunkWorldSize) / double(SX - 1);

    // First pass: build vertices + UVs
    for (int y = 0; y < SY; ++y)
    {
        for (int x = 0; x < SX; ++x)
        {
            int idx = y * SX + x;
            double WX = originX + double(x) * step;
            double WY = originY + double(y) * step;
            float H = HF->At(x, y);

            // Road carving + vertex color
            bool bOnRoad = false;
            float roadZ = 0.f;
            float roadDist = FLT_MAX;
            float chosenWidth = 1.0f;

            for (const FRoadSpline& Road : RoadSplines)
            {
                float d, rz;
                if (ComputeDistanceToSpline(WX, WY, Road, d, rz))
                {
                    float effectiveWidth = Road.Width * RoadWidthMultiplier;
                    if (d < roadDist)
                    {
                        roadDist = d;
                        roadZ = rz;
                        chosenWidth = effectiveWidth;
                    }
                    if (d < effectiveWidth)
                    {
                        bOnRoad = true;
                    }
                }
            }

            if (bOnRoad)
            {
                float smooth = FMath::Clamp(roadDist / FMath::Max(1.0f, chosenWidth), 0.f, 1.f);
                float finalSmooth = 1.0f;
                if (RoadBlendStrength <= KINDA_SMALL_NUMBER)
                {
                    finalSmooth = (roadDist >= chosenWidth ? 1.0f : 0.0f);
                }
                else
                {
                    finalSmooth = FMath::Pow(smooth, 1.0f / RoadBlendStrength);
                }

                H = FMath::Lerp(roadZ, H, finalSmooth);
                VertexColors[idx] = FLinearColor(finalSmooth, finalSmooth, finalSmooth, 1.f);
            }
            else
            {
                VertexColors[idx] = FLinearColor::White;
            }

            Vertices[idx] = FVector((float)WX, (float)WY, H);
            UVs[idx] = FVector2D(float(x) / float(SX - 1), float(y) / float(SY - 1));
        }
    }

    // Second pass: triangles
    for (int y = 0; y < SY - 1; ++y)
    {
        for (int x = 0; x < SX - 1; ++x)
        {
            int i0 = y * SX + x;
            int i1 = i0 + 1;
            int i2 = i0 + SX;
            int i3 = i2 + 1;

            bool b0 = (VertexColors[i0].R < 0.5f);
            bool b1 = (VertexColors[i1].R < 0.5f);
            bool b2 = (VertexColors[i2].R < 0.5f);
            bool b3 = (VertexColors[i3].R < 0.5f);

            bool triAIsRoad = b0 || b2 || b1;
            bool triBIsRoad = b1 || b2 || b3;

            if (triAIsRoad)
            {
                RoadTriangles.Add(i0); RoadTriangles.Add(i2); RoadTriangles.Add(i1);
                AllTriangles.Add(i0); AllTriangles.Add(i2); AllTriangles.Add(i1);
            }
            else
            {
                TerrainTriangles.Add(i0); TerrainTriangles.Add(i2); TerrainTriangles.Add(i1);
                AllTriangles.Add(i0); AllTriangles.Add(i2); AllTriangles.Add(i1);
            }

            if (triBIsRoad)
            {
                RoadTriangles.Add(i1); RoadTriangles.Add(i2); RoadTriangles.Add(i3);
                AllTriangles.Add(i1); AllTriangles.Add(i2); AllTriangles.Add(i3);
            }
            else
            {
                TerrainTriangles.Add(i1); TerrainTriangles.Add(i2); TerrainTriangles.Add(i3);
                AllTriangles.Add(i1); AllTriangles.Add(i2); AllTriangles.Add(i3);
            }
        }
    }

    // Normals/tangents
    TArray<FProcMeshTangent> Tangents;
    UKismetProceduralMeshLibrary::CalculateTangentsForMesh(Vertices, AllTriangles, UVs, Normals, Tangents);

    // Mesh sections
    Mesh->ClearAllMeshSections();
    Mesh->CreateMeshSection_LinearColor(0, Vertices, TerrainTriangles, Normals, UVs, VertexColors, Tangents, true);
    Mesh->CreateMeshSection_LinearColor(1, Vertices, RoadTriangles, Normals, UVs, VertexColors, Tangents, true);

    if (ChunkMaterial) Mesh->SetMaterial(0, ChunkMaterial);
    if (RoadMaterial) Mesh->SetMaterial(1, RoadMaterial);

    Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Mesh->bUseAsyncCooking = true;
    Mesh->SetCollisionObjectType(ECC_WorldStatic);
    Mesh->SetCollisionResponseToAllChannels(ECR_Block);
    Mesh->SetCanEverAffectNavigation(true);
    Mesh->bUseComplexAsSimpleCollision = true;

    // -------------------------------------------------
    // Vegetation spawns
    // -------------------------------------------------

    // --- Trees only once per chunk ---
    bool bShouldSpawnTrees = !ChunksWithTrees.Contains(ChunkCoord);

    for (int idx = 0; idx < Vertices.Num(); ++idx)
    {
        FVector WorldPos = Vertices[idx];

        // Check roads
        float nearestRoadDist = FLT_MAX;
        bool bInsideAnyRoad = false;
        for (const FRoadSpline& Road : RoadSplines)
        {
            float d, rz;
            if (ComputeDistanceToSpline(WorldPos.X, WorldPos.Y, Road, d, rz))
            {
                nearestRoadDist = FMath::Min(nearestRoadDist, d);
                float effectiveWidth = Road.Width * RoadWidthMultiplier;
                if (d < effectiveWidth)
                {
                    bInsideAnyRoad = true;
                    break;
                }
            }
        }

        // Trees (only once)
        if (bShouldSpawnTrees && !bInsideAnyRoad && FoliageActors.Num() > 0 && FMath::FRand() < TreeDensity)
        {
            float H = WorldPos.Z;
            if (H > TreeAltitudeMin && H < TreeAltitudeMax)
            {
                if (Normals[idx].Z > TreeSlope)
                {
                    int32 Index = FMath::RandRange(0, FoliageActors.Num() - 1);
                    FFoliageSpawnData Spawn;
                    Spawn.Location = WorldPos;
                    Spawn.FoliageClass = FoliageActors[Index];
                    FoliageSpawnQueue.Enqueue(Spawn);
                }
            }
        }

        // Grass (every rebuild, adapts to LOD resolution)
        if (!bInsideAnyRoad && GrassActors.Num() > 0 && FMath::FRand() < GrassDensity)
        {
            float H = WorldPos.Z;
            if (H > GrassAltitudeMin && H < GrassAltitudeMax)
            {
                if (Normals[idx].Z < GrassSlope)
                {
                    int32 Index = FMath::RandRange(0, GrassActors.Num() - 1);
                    FGrassSpawnData GrassSpawn;
                    GrassSpawn.Location = WorldPos;
                    GrassSpawn.GrassClass = GrassActors[Index];
                    GrassSpawnQueue.Enqueue(GrassSpawn);
                }
            }
        }
    }

    if (bShouldSpawnTrees)
    {
        ChunksWithTrees.Add(ChunkCoord);
    }

    // Nav rebuild
    if (UWorld* World = GetWorld())
    {
        if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
        {
            NavSys->Build();
            NavSys->OnNavigationBoundsUpdated(nullptr);
        }
    }
}


// Vegetation spawn processors
void ARingTerrainManager::ProcessFoliageSpawns()
{
    FFoliageSpawnData SpawnData;
    while (FoliageSpawnQueue.Dequeue(SpawnData))
    {
        if (UWorld* World = GetWorld())
        {
            FRotator Rot(0.f, FMath::FRandRange(0, 360.0f), 0.0f);
            FActorSpawnParameters FoliageParams;
            FoliageParams.Owner = this;
            World->SpawnActor<AActor>(SpawnData.FoliageClass, SpawnData.Location, Rot, FoliageParams);
            //Tree_Inst->AddInstance(FTransform(Rot, SpawnData.Location, FVector(5.0f,5.0f,5.0f)));
        }
    }
}

void ARingTerrainManager::ProcessGrassSpawn()
{
    FGrassSpawnData GrassData;
    while (GrassSpawnQueue.Dequeue(GrassData))
    {
        if (UWorld* World = GetWorld())
        {
            FRotator Rot(0.f, FMath::FRandRange(0, 360.0f), 0.0f);
            FActorSpawnParameters GrassParams;
            GrassParams.Owner = this;
            World->SpawnActor<AActor>(GrassData.GrassClass, GrassData.Location, Rot, GrassParams);
        }
    }
}

/* ---------- Simple terrain helpers ---------- */
float ARingTerrainManager::GetHeight(FVector2D Location) {
    return PerlinNoiseExt(Location, .00001f, 20000, FVector2D(.1f));
}

float ARingTerrainManager::PerlinNoiseExt(const FVector2D Location, const float Scale, const float Amplitude, const FVector2D offset) {
    return FMath::PerlinNoise2D(Location * Scale + FVector2D(.1f, .1f) + offset) * Amplitude;
}

/* ---------- Noise (pure math) ---------- */
static inline double Fade5d(double t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

static inline uint64 SplitMix64(uint64 x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static inline float HashFloatFromXY(int64 xi, int64 yi) {
    uint64 key = (uint64(xi) << 32) ^ (uint64(yi) + 0x9E3779B97F4A7C15ULL);
    return float(SplitMix64(key) & 0xffffffffu) / float(0xFFFFFFFFu);
}

static inline void Gradient2DFromHash(int64 ix, int64 iy, float& gx, float& gy) {
    float h = HashFloatFromXY(ix, iy);
    float angle = h * 2.0f * 3.14159265358979323846f;
    gx = cosf(angle); gy = sinf(angle);
}

float ARingTerrainManager::TerrainNormalizedAt_FullFBM(double WX, double WY) const
{
    double sum = 0.0;
    double amp = 1.0;
    double freq = Params.BaseScale;
    double norm = 0.0;

    for (int32 o = 0; o < Params.Octaves; ++o)
    {
        double sx = WX * freq;
        double sy = WY * freq;

        int64 x0 = int64(FMath::FloorToInt((float)sx));
        int64 y0 = int64(FMath::FloorToInt((float)sy));
        double fx = sx - double(x0);
        double fy = sy - double(y0);

        double u = Fade5d(fx);
        double v = Fade5d(fy);

        float gx00, gy00, gx10, gy10, gx01, gy01, gx11, gy11;
        Gradient2DFromHash(x0, y0, gx00, gy00);
        Gradient2DFromHash(x0 + 1, y0, gx10, gy10);
        Gradient2DFromHash(x0, y0 + 1, gx01, gy01);
        Gradient2DFromHash(x0 + 1, y0 + 1, gx11, gy11);

        double ox00 = fx, oy00 = fy;
        double ox10 = fx - 1, oy10 = fy;
        double ox01 = fx, oy01 = fy - 1;
        double ox11 = fx - 1, oy11 = fy - 1;

        double n00 = gx00 * ox00 + gy00 * oy00;
        double n10 = gx10 * ox10 + gy10 * oy10;
        double n01 = gx01 * ox01 + gy01 * oy01;
        double n11 = gx11 * ox11 + gy11 * oy11;

        double nx0 = n00 + (n10 - n00) * u;
        double nx1 = n01 + (n11 - n01) * u;
        double val = nx0 + (nx1 - nx0) * v; // ~[-1,1]

        sum += val * amp;
        norm += amp;
        amp *= Params.Gain;
        freq *= Params.Lacunarity;
    }

    double fbm = sum / FMath::Max(norm, 1e-6);
    double normalized = (fbm + 1.0) * 0.5; // -> [0,1]

    if (Params.Ridge > 0.0f)
    {
        double ridge = 1.0 - fabs(2.0 * normalized - 1.0);
        normalized = FMath::Lerp((float)normalized, (float)ridge, Params.Ridge);
    }

    double baseHeight = (normalized - 0.5) * 2.0 * Params.HeightScale;

    if (baseHeight > 20000.0)
    {
        double mountain = FMath::PerlinNoise2D(FVector2D(WX, WY) * MountainScale);
        mountain = (mountain + 1.0) * 0.5;
        double mountainHeight = (mountain - 0.5) * 2.0 * MountainHeight;
        baseHeight += mountainHeight * MountainStrength;
    }

    double finalNormalized = (baseHeight / Params.HeightScale) * 0.5 + 0.5;
    return float(FMath::Clamp(finalNormalized, 0.0, 1.0));
}

float ARingTerrainManager::ApplyCurveAndScale(double Normalized) const
{
    float n = float(FMath::Clamp(Normalized, 0.0, 1.0));
    if (NoiseCurveTable.Num() > 0)
    {
        int idx = FMath::Clamp(int(n * float(NoiseCurveTable.Num() - 1)), 0, NoiseCurveTable.Num() - 1);
        n *= NoiseCurveTable[idx];
    }
    return (n - 0.5f) * 2.0f * Params.HeightScale;
}

int32 ARingTerrainManager::PickLODForChunk(const FIntPoint& ChunkCoord, const FVector& PlayerLocation) const
{
    // Find the world-space center of the chunk
    FVector2D center(
        (ChunkCoord.X + 0.5f) * ChunkWorldSize,
        (ChunkCoord.Y + 0.5f) * ChunkWorldSize);

    float dist = FVector2D::Distance(
        FVector2D(PlayerLocation.X, PlayerLocation.Y),
        center);

    // Normalize distance to [0..1] by dividing by adaptive radius
    float factor = (AdaptiveLODRadius > 0.f) ? dist / AdaptiveLODRadius : 1.f;

    // Apply falloff shaping
    factor = FMath::Pow(factor, LODFalloffExponent);

    // Scale into discrete LODs
    int32 lod = FMath::Clamp(
        FMath::FloorToInt(factor * LODResolutions.Num()),
        0,
        LODResolutions.Num() - 1);

    return lod;
}


double ARingTerrainManager::MountainNoise(double WX, double WY) const {
    double n = FMath::PerlinNoise2D(FVector2D(WX, WY) * 10000.0);
    return (n + 1.0) * .001;
}


/* ---------- Procedural splat generation & blending (thread-safe math) ---------- */

FColor ARingTerrainManager::GenerateProceduralSplat(double WX, double WY, double NormalizedHeight) const
{
    // Simple layered noise + height-based rules to choose biome weights.
    // This is fully deterministic (no UObject access) and cheap; tweak scales to taste.
    float nx = float(WX * SplatMapFrequency); // spatial frequency
    float ny = float(WY * SplatMapFrequency);

    float biomeNoise = FMath::PerlinNoise2D(FVector2D(nx, ny));
    biomeNoise = (biomeNoise + 1.0f) * SplatMapScale; // -> [0,1]

    float detailNoise = FMath::PerlinNoise2D(FVector2D(nx * 2.3f + 13.37f, ny * 2.7f + 42.0f));
    detailNoise = (detailNoise + 1.0f) * SplatMapScale;

    float r = 0.f, g = 0.f, b = 0.f, a = 0.f;

    // Height-driven baseline biomes
    if (NormalizedHeight < 0.30)
    {
        // Lowlands -> plains (red)
        r = 1.0f - detailNoise * 0.5f;
        g = detailNoise * 0.5f * biomeNoise;
    }
    else if (NormalizedHeight < 0.55)
    {
        // Mid -> forest (green)
        g = 0.6f + 0.4f * biomeNoise;
        r = 0.4f * (1.0f - biomeNoise);
        b = 0.1f * detailNoise;
    }
    else if (NormalizedHeight < 0.75)
    {
        // Higher -> rocky (blue)
        b = 0.5f + 0.5f * detailNoise;
        g = 0.2f * (1.0f - detailNoise);
    }
    else
    {
        // Peaks -> snow (alpha channel)
        a = 0.7f + 0.3f * detailNoise;
        b = 0.2f * (1.0f - detailNoise);
    }

    // Spatially vary mixes slightly
    float blend = biomeNoise * 0.5f;
    r = FMath::Clamp(r * (1.0f - blend) + blend * detailNoise, 0.0f, 1.0f);
    g = FMath::Clamp(g * (1.0f - blend) + blend * detailNoise, 0.0f, 1.0f);
    b = FMath::Clamp(b * (1.0f - blend) + blend * detailNoise, 0.0f, 1.0f);
    a = FMath::Clamp(a, 0.0f, 1.0f);

    float sum = r + g + b + a;
    if (sum <= KINDA_SMALL_NUMBER)
    {
        // fallback to white (equal mix)
        return FColor(255, 255, 255, 255);
    }
    r /= sum; g /= sum; b /= sum; a /= sum;

    return FColor(uint8(r * 255.f), uint8(g * 255.f), uint8(b * 255.f), uint8(a * 255.f));
}

float ARingTerrainManager::ApplySplatCurves(double Normalized, const TArray<TArray<float>>& SplatTables) const
{
    float n = float(FMath::Clamp(Normalized, 0.0, 1.0));
    if (SplatTables.Num() == 0)
    {
        // Fall back to single NoiseCurveTable behavior
        if (NoiseCurveTable.Num() > 0)
        {
            int idx = FMath::Clamp(int(n * float(NoiseCurveTable.Num() - 1)), 0, NoiseCurveTable.Num() - 1);
            n *= NoiseCurveTable[idx];
        }
        return (n - 0.5f) * 2.0f * Params.HeightScale;
    }

    // We assume up to 4 channels (R,G,B,A). If there are fewer tables, treat missing as multiplier 1.
    float weights[4] = { 0,0,0,0 };
    // This function can't sample world splat pixels, callers should weight before invoking this
    // Instead we expect the caller to compute a weighted sum externally. For convenience here,
    // if only one table present we apply it directly.
    // But for safety, return the normalized height scaled by average of tables weighted equally.
    float accum = 0.0f;
    int count = 0;
    for (int i = 0; i < SplatTables.Num(); ++i)
    {
        const TArray<float>& Tbl = SplatTables[i];
        if (Tbl.Num() == 0) continue;
        int idx = FMath::Clamp(int(n * float(Tbl.Num() - 1)), 0, Tbl.Num() - 1);
        accum += Tbl[idx];
        ++count;
    }
    float avg = (count > 0) ? (accum / float(count)) : 1.0f;
    float nn = n * avg;
    return (nn - 0.5f) * 2.0f * Params.HeightScale;
}
/* ---------- Random road generation (game thread) ---------- */

void ARingTerrainManager::GenerateRandomRivers()
{
    RiverSplines.Empty();

    FRandomStream Rng(RandomSeed + 99991); // Different seed from roads

    const int32 NumRivers = 4 + Rng.RandRange(0, 3); // 4–7 rivers
    const float StartRadius = ReferenceSize * 0.95f; // near outer ring
    const float TwoPi = 6.28318530718f;

    for (int32 r = 0; r < NumRivers; r++)
    {
        FRiverSpline River;

        // -----------------------------
        //  Choose a random start point
        // -----------------------------
        const float Angle = Rng.FRand() * TwoPi;

        FVector Start(
            FMath::Cos(Angle) * StartRadius,
            FMath::Sin(Angle) * StartRadius,
            0.0f
        );

        // Get terrain height for start
        Start.Z = SampleTerrainHeight(Start);

        River.ControlPoints.Add(Start);

        // -----------------------------
        // Generate the spline inward
        // -----------------------------
        const int32 NumSegments = 12 + Rng.RandRange(0, 8);

        FVector Pos = Start;

        for (int32 i = 0; i < NumSegments; i++)
        {
            // Flow downhill: GetRiverDirection() returns a downhill vector on terrain
            FVector DownDir = GetRiverDirection(Pos);
            DownDir.Z = 0.f;
            DownDir.Normalize();

            // Length of each segment (rivers travel farther than roads)
            float Step = 4000.f + Rng.FRandRange(0.f, 2000.f);

            // Add natural meander
            float MeanderAngle = Rng.FRandRange(-0.9f, 0.9f);
            FRotator Rot(0, FMath::RadiansToDegrees(MeanderAngle), 0);
            DownDir = Rot.RotateVector(DownDir);

            FVector Next = Pos + DownDir * Step;

            // Ensure we stay inside the terrain ring
            float DistToCenter = FVector(Next.X, Next.Y, 0).Size();
            if (DistToCenter < ReferenceSize * 0.1f)
                break; // stop river near center

            // Apply terrain height
            float H = SampleTerrainHeight(Next);
            Next.Z = H - Rng.FRandRange(100.f, 300.f); // river cuts slightly below terrain

            River.ControlPoints.Add(Next);

            Pos = Next;
        }

        // -----------------------------
        // Finalize river properties
        // -----------------------------
        River.Width = Rng.FRandRange(1200.f, 4000.f);
        River.BankBlendStrength = Rng.FRandRange(0.3f, 1.2f);

        // WaterLevel = lowest Z in spline
        float MinZ = River.ControlPoints[0].Z;
        for (const FVector& P : River.ControlPoints)
            MinZ = FMath::Min(MinZ, P.Z);

        River.WaterLevel = MinZ;

        RiverSplines.Add(River);
    }

    // ----------------------------------
    //  Force terrain regeneration
    // ----------------------------------
    RegenerateAll(); // <---- This is the correct function in your project
}


/* ---------- Road helpers (game thread) ---------- */

// Unconstrained computation of distance to spline (returns true if spline has >= 2 points)
bool ARingTerrainManager::ComputeDistanceToSpline(double WX, double WY, const FRoadSpline& Road, float& OutDist, float& OutZ) const
{
    OutDist = MAX_flt;
    OutZ = 0.f;

    if (Road.ControlPoints.Num() < 2) return false;

    FVector2D P((float)WX, (float)WY);

    for (int i = 0; i < Road.ControlPoints.Num() - 1; ++i)
    {
        const FVector& A = Road.ControlPoints[i];
        const FVector& B = Road.ControlPoints[i + 1];

        FVector2D A2(A.X, A.Y);
        FVector2D B2(B.X, B.Y);

        FVector2D AB = B2 - A2;
        float denom = FVector2D::DotProduct(AB, AB);
        if (denom <= 0.f) continue;

        float t = FVector2D::DotProduct(P - A2, AB) / denom;
        t = FMath::Clamp(t, 0.f, 1.f);

        FVector2D Closest = A2 + t * AB;
        float Dist = FVector2D::Distance(P, Closest);

        if (Dist < OutDist)
        {
            OutDist = Dist;
            OutZ = FMath::Lerp(A.Z, B.Z, t);
        }
    }

    // Always return true here if there was at least one segment
    return (OutDist < MAX_flt);
}

// Compute the nearest distance from (WX,WY) to polyline segments in Road, and interpolate Z along that segment.
// Returns true if found segment (and OutDist < Road.Width). OutZ is set to the interpolated Z on the closest point.
bool ARingTerrainManager::GetDistanceToSpline(double WX, double WY, const FRoadSpline& Road, float& OutDist, float& OutZ) const
{
    // Use ComputeDistanceToSpline then clamp/compare against stored road width
    if (!ComputeDistanceToSpline(WX, WY, Road, OutDist, OutZ))
        return false;

    return (OutDist < Road.Width);
}

bool ARingTerrainManager::IsNearRoad(const FVector& WorldPos, const TArray<FRoadSpline>& Roads) const
{
    for (const FRoadSpline& Road : Roads)
    {
        float dist, rz;
        if (ComputeDistanceToSpline(WorldPos.X, WorldPos.Y, Road, dist, rz))
        {
            if (dist < Road.Width)
                return true;
        }
    }
    return false;
}

/* ---------- Misc (unused worker stub / LOD helpers) ---------- */
void ARingTerrainManager::UpdateChunksWorker(FVector PlayerPos)
{
    // (kept for future usage; not active)
    int32 PlayerChunkX = FMath::FloorToInt(PlayerPos.X / ChunkWorldSize);
    int32 PlayerChunkY = FMath::FloorToInt(PlayerPos.Y / ChunkWorldSize);
    FIntPoint PlayerChunk(PlayerChunkX, PlayerChunkY);
}

int32 ARingTerrainManager::CalculateLODFromDistance(const FIntPoint& Coord, const FIntPoint& PlayerChunk) const
{
    const int32 dx = FMath::Abs(Coord.X - PlayerChunk.X);
    const int32 dy = FMath::Abs(Coord.Y - PlayerChunk.Y);
    const int32 ring = FMath::Max(dx, dy);
    const int32 step = FMath::Max(ChunksPerLODRing, 1);
    const int32 lod = ring / step;
    return FMath::Clamp(lod, 0, MaxLOD);
}

bool ARingTerrainManager::IsInsideBuildingFootprint(
    const FVector& WorldPos,
    const FBuildingSpawn& Building,
    float Margin) const
{
    const FVector2D P(WorldPos.X, WorldPos.Y);
    const FVector2D C(Building.Location.X, Building.Location.Y);

    FVector2D Ext = Building.HalfExtents + FVector2D(Margin, Margin);

    FVector2D Delta = FVector2D(FMath::Abs(P.X - C.X), FMath::Abs(P.Y - C.Y));
    return (Delta.X <= Ext.X && Delta.Y <= Ext.Y);
}

void ARingTerrainManager::SpawnBuildingsIfNeeded()
{
    if (!GetWorld()) return;

    for (const FBuildingSpawn& B : Buildings)
    {
        if (!B.BuildingClass) continue;

        // Already spawned?
        if (SpawnedBuildingActors.Contains(B.Location))
            continue;

        FVector SpawnPos = B.Location;
        SpawnPos.Z += B.FlattenHeightOffset;

        AActor* A = GetWorld()->SpawnActor<AActor>(
            B.BuildingClass,
            SpawnPos,
            FRotator::ZeroRotator
        );

        if (A)
            SpawnedBuildingActors.Add(B.Location, A);
    }
}

void ARingTerrainManager::ProcessBuildingSpawns()
{
    FBuildingSpawnData SpawnData;

    while (BuildingSpawnQueue.Dequeue(SpawnData))
    {
        if (UWorld* World = GetWorld())
        {
            FActorSpawnParameters SpawnParams;
            SpawnParams.Owner = this;

            FRotator Rot(0, FMath::FRandRange(0, 360.f), 0);

            AActor* A = World->SpawnActor<AActor>(
                SpawnData.BuildingClass,
                SpawnData.Location,
                Rot,
                SpawnParams
            );

            if (A)
            {
                SpawnedBuildingActors.Add(SpawnData.Location, A);
            }
        }
    }
}

